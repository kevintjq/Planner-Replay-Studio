#include "trajectory_controller.h"
#include "video_reader.h"
#include "still_image_writer.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

void PreviewImages::setImage(const QImage &image)
{
    const QMutexLocker locker(&lock_);
    image_ = image;
}

quint64 PreviewImages::setTrajectory(std::shared_ptr<const replay::FixedCameraTrajectory> trajectory)
{
    const QMutexLocker locker(&lock_);
    if (trajectory_ == trajectory) return sourceRevision_;
    trajectory_ = std::move(trajectory);
    thumbnails_.clear();
    maskSnapshots_.clear(); edits_ = {};
    return ++sourceRevision_;
}

void PreviewImages::setEdits(const replay::TrajectoryEdits &edits)
{
    const QMutexLocker locker(&lock_);
    // Content-addressed URLs keep unchanged pixels and their cache alive.
    // Retain a bounded mask history for queued requests of an older version;
    // an evicted stale request is rejected rather than given newer pixels.
    for (auto i = edits.frames.cbegin(); i != edits.frames.cend(); ++i) if (!i->maskStrokes.empty()) {
        const auto key = replay::trajectoryMaskContentKey(*i);
        if (!maskSnapshots_.contains(key)) {
            size_t bytes = sizeof(replay::TrajectoryFrameEdit);
            for (const auto &stroke : i->maskStrokes) bytes += sizeof(stroke) + size_t(stroke.points.size()) * sizeof(QPointF);
            maskSnapshots_.insert(key, new replay::TrajectoryFrameEdit(*i), int(std::max<size_t>(1, (bytes + 1023) / 1024)));
        }
    }
    edits_ = edits;
}

QImage PreviewImages::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QImage image;
    std::shared_ptr<const replay::FixedCameraTrajectory> track;
    std::optional<size_t> source;
    replay::TrajectoryFrameEdit edit;
    replay::TrajectoryEdits vectorEdits;
    QString kind, vectorKind;
    {
        const QMutexLocker locker(&lock_);
        if (id.startsWith("background/")) {
            bool valid = false; const auto revision = id.section('/',1,1).toULongLong(&valid);
            if (!valid || revision != sourceRevision_) return {};
            image = trajectory_ ? trajectory_->background : image_;
        } else if (id.startsWith("vector/")) {
            bool valid = false; const auto revision = id.section('/',1,1).toULongLong(&valid);
            if (!valid || revision != sourceRevision_ || !trajectory_) return {};
            track = trajectory_; vectorEdits = edits_; vectorKind = id.section('/',2,2);
        } else if (id.startsWith("frame/") || id.startsWith("source/") || id.startsWith("mask/") || id.startsWith("sprite/")) {
            const auto parts = id.split('/');
            bool revisionOk = false, indexOk = false;
            const auto revision = parts.value(1).toULongLong(&revisionOk);
            const auto index = parts.value(2).toULongLong(&indexOk);
            if (parts.size() < 3 || !revisionOk || !indexOk || revision != sourceRevision_ ||
                !trajectory_ || index >= trajectory_->frames.size())
                return {};
            if (const auto *cached = thumbnails_.object(id))
                image = *cached;
            else {
                track = trajectory_;
                source = size_t(index);
                kind = parts[0]; edit = edits_.frames.value(replay::trajectoryTimestamp(track->frames[*source].seconds));
                const auto content = parts.value(3);
                if (kind != "source" && !content.isEmpty() && content != replay::trajectoryMaskContentKey(edit)) {
                    if (content == "base") edit.maskStrokes.clear();
                    else if (const auto *snapshot = maskSnapshots_.object(content)) edit.maskStrokes = snapshot->maskStrokes;
                    else return {};
                }
            }
        } else {
            image = image_;
        }
    }
    if (!vectorKind.isEmpty()) {
        try {
            const auto layers = replay::trajectoryLayers(*track, {}, 1., {}, vectorEdits);
            int part = 0; QRectF area(QPointF(), layers.canvasSize);
            if (vectorKind == "line") { part = replay::TrajectoryPath; vectorEdits.showLine = true; }
            else if (vectorKind == "start" && layers.start) { part = replay::TrajectoryStart; vectorEdits.start.enabled = true; area = replay::trajectoryEndpointBounds(*layers.start, vectorEdits.start); }
            else if (vectorKind == "goal" && layers.goal) { part = replay::TrajectoryGoal; vectorEdits.goal.enabled = true; area = replay::trajectoryEndpointBounds(*layers.goal, vectorEdits.goal); }
            if (!part || area.isEmpty()) return {};
            const auto pixels = area.toAlignedRect(); image = QImage(pixels.size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image); painter.translate(-pixels.topLeft());
            replay::paintTrajectoryLayers(painter, *track, layers, QRectF(QPointF(), layers.canvasSize), vectorEdits, {}, part); painter.end();
        } catch (const std::exception &) { return {}; }
    }
    if (source) {
        const auto &frame = track->frames[*source];
        try { image = kind == "source" ? replay::trajectorySourceCrop(frame) : replay::trajectorySprite(frame, edit); }
        catch (const std::exception &) { return {}; }
        if (kind == "mask" && !image.isNull()) {
            auto mask = image.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < mask.height(); ++y) {
                auto *row = mask.scanLine(y);
                for (int x = 0; x < mask.width(); ++x) {
                    const auto alpha = row[4*x+3]; row[4*x] = row[4*x+1] = row[4*x+2] = alpha; row[4*x+3] = 255;
                }
            }
            image = std::move(mask);
        }
        if (kind == "frame") image = image.scaled({160, 100}, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QMutexLocker locker(&lock_);
        if (track == trajectory_ && !image.isNull())
            thumbnails_.insert(id, new QImage(image), int((image.sizeInBytes() + 1023) / 1024));
    }
    if (size)
        *size = image.size();
    if (!image.isNull() && requestedSize.isValid() &&
        (image.width() > requestedSize.width() || image.height() > requestedSize.height()))
        return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

TrajectoryController::TrajectoryController(PreviewImages &images) : images_(images)
{
    CanvasLayerModel::Binding binding;
    binding.getState = [this] { return canvasState(); };
    binding.storeState = [this](const QVariantMap &value) { auto next = state_; next.canvas = value; commit(std::move(next)); };
    binding.rawLayers = [this] { return canvasLayers(); };
    binding.exists = [this](const QString &id) {
        const auto rows = canvasLayers();
        return std::any_of(rows.cbegin(), rows.cend(), [&](const QVariant &value) { return value.toMap().value("id").toString() == id; });
    };
    binding.remove = [this](const QStringList &ids) { removeCanvasLayers(ids); };
    binding.duplicate = [this](const QString &id) { return duplicateCanvasLayer(id); };
    binding.beginEdit = [this] { beginCanvasEdit(); };
    binding.endEdit = [this](bool cancel) { endCanvasEdit(cancel); };
    binding.duration = [this] { return lastTime(); };
    binding.loading = [this] { return working(); };
    canvas_ = new CanvasLayerModel(std::move(binding), this);
    connect(this, &TrajectoryController::changed, canvas_, [this] { canvasLayers_.setLayers(canvasLayers()); canvas_->refresh(); });
    connect(&exportWatcher_, &QFutureWatcher<FrameExport>::finished, this, [this] {
        finishFrameExport(exportWatcher_.result());
    });
    connect(&batchWatcher_, &QFutureWatcher<replay::TrajectoryBatchResult>::finished, this, [this] {
        const auto result = batchWatcher_.result();
        exporting_ = false; exportDirectory_ = result.directory;
        status_ = result.error.isEmpty() ? tr("已导出 %1 张图片：%2 / Batch exported.").arg(result.completed).arg(result.directory) : result.error;
        emit changed(); emit batchExported(result.error.isEmpty(), status_);
        if (pendingSettings_) restoreSettings(*std::exchange(pendingSettings_, std::nullopt));
    });
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(50);
    connect(&previewTimer_, &QTimer::timeout, this, &TrajectoryController::renderPreview);
    connect(&previewWatcher_, &QFutureWatcher<Preview>::finished, this, [this] {
        const auto result = previewWatcher_.result();
        if (result.generation == generation_) {
            foregroundCounts_ = result.counts;
            layerData_ = result.layers;
            previewError_ = result.error;
            if (result.image.isNull() && previewError_.isEmpty())
                previewError_ = tr("最新预览为空，无法导出。 / Latest preview is empty.");
            const auto gate = result.counts.exportError(state_.edits);
            foregroundReady_ = previewError_.isEmpty() && gate.isEmpty();
            if (!result.image.isNull())
                setImage(result.image);
            if (previewError_.isEmpty())
                publishedPreviewGeneration_ = result.generation;
            if (!previewError_.isEmpty())
                status_ = previewError_;
            else if (!gate.isEmpty()) status_ = gate;
        }
        if (previewDirty_ && !previewTimer_.isActive())
            renderPreview();
        emit changed();
        resumePreviewExport();
    });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        auto result = watcher_.result();
        working_ = false;
        if (pendingSettings_) {
            const auto settings = std::exchange(pendingSettings_, std::nullopt);
            restoreSettings(*settings);
            return;
        }
        if (cancelled_) {
            resumeTracking_ = false;
            status_ = tr("已取消。");
        } else {
            status_ = result.message;
            if (result.trajectory) {
                trajectory_ = std::move(result.trajectory);
                sourceRevision_ = images_.setTrajectory(trajectory_);
                updatePreview();
            } else if (!result.image.isNull()) {
                initial_ = std::move(result.image);
                setImage(initial_);
                if (std::exchange(resumeTracking_, false))
                    QTimer::singleShot(0, this, [this] { track(selectionBox_, end_); });
            }
        }
        emit changed();
    });
}

TrajectoryController::~TrajectoryController()
{
    cancelled_ = true;
    if (previewCancelled_)
        *previewCancelled_ = true;
    watcher_.waitForFinished();
    previewWatcher_.waitForFinished();
    exportWatcher_.waitForFinished();
    batchWatcher_.waitForFinished();
}

QString TrajectoryController::fileName() const { return QFileInfo(file_).fileName(); }
double TrajectoryController::lastTime() const
{
    return trajectory_ && !trajectory_->frames.empty() ? trajectory_->frames.back().seconds : start_;
}
QUrl TrajectoryController::image() const
{
    return !revision_ || (!trajectory_ && initial_.isNull()) ? QUrl{} :
        QUrl(QStringLiteral("image://trajectory/") + QString::number(revision_));
}
void TrajectoryController::setImage(const QImage &image) { images_.setImage(image); ++revision_; }

void TrajectoryController::reportError(const std::exception &error)
{
    status_ = QString::fromUtf8(error.what());
    emit changed();
}

void TrajectoryController::finishFrameExport(FrameExport result)
{
    const auto error = result.error;
    frameExportReport_ = std::move(result.report);
    frameExportReport_["success"] = error.isEmpty(); frameExportReport_["error"] = error;
    exporting_ = false;
    status_ = error.isEmpty() ? frameExportReport_.value("scope")=="layers" ?
        tr("已导出 %1 个选中图层，含 %2 个 UAV 实例。 / Selected layers exported.").arg(frameExportReport_.value("paintedLayers").toInt()).arg(frameExportReport_.value("composedUavInstances").toInt()) :
        tr("已导出完整质量画面。 / Full-quality frame exported.") : error;
    emit changed(); emit frameExported(error.isEmpty(), status_);
    if (pendingSettings_) restoreSettings(*std::exchange(pendingSettings_, std::nullopt));
}

void TrajectoryController::exportAfterPreview(std::function<void()> start, std::function<void(QString)> fail)
{
    if (!trajectory_ || publishedPreviewGeneration_ == generation_) { start(); return; }
    // The caller freezes edits while waiting, but preview jobs never depend on
    // working(): an older job can drain before this generation is published.
    pendingPreviewExport_ = PendingPreviewExport{generation_, std::move(start), std::move(fail)};
    status_ = tr("正在等待最新预览后导出… / Waiting for the latest preview before export…");
    emit changed();
    previewTimer_.stop(); renderPreview();
    resumePreviewExport();
}

void TrajectoryController::failPreviewExport(const QString &error)
{
    if (!pendingPreviewExport_) return;
    auto fail = std::move(pendingPreviewExport_->fail);
    pendingPreviewExport_.reset();
    fail(error);
}

void TrajectoryController::resumePreviewExport()
{
    if (!pendingPreviewExport_) return;
    if (cancelled_ || pendingPreviewExport_->generation != generation_) {
        failPreviewExport(tr("已取消。 / Cancelled.")); return;
    }
    if (publishedPreviewGeneration_ != pendingPreviewExport_->generation) {
        if (!previewError_.isEmpty()) failPreviewExport(previewError_);
        return;
    }
    auto start = std::move(pendingPreviewExport_->start);
    pendingPreviewExport_.reset();
    start();
}

void TrajectoryController::begin(const QString &status, std::function<Result()> job)
{
    cancelled_ = false;
    working_ = true;
    progress_ = 0.;
    status_ = status;
    emit changed();
    watcher_.setFuture(QtConcurrent::run([job = std::move(job)] {
        try { return job(); }
        catch (const std::exception &error) { return Result{{}, {}, QString::fromUtf8(error.what())}; }
    }));
}

void TrajectoryController::clearResult()
{
    ++generation_;
    if (previewCancelled_)
        *previewCancelled_ = true;
    previewDirty_ = false;
    previewError_.clear();
    previewTimer_.stop();
    trajectory_.reset();
    layerData_ = {};
    foregroundReady_ = false; foregroundCounts_ = {};
    sourceRevision_ = images_.setTrajectory({});
    sampled_.clear();
    selected_.clear();
    selectionAnchor_ = -1;
    frames_.setFrames({}, {}, sourceRevision_);
    frames_.setSelection({});
    undo_.clear();
    redo_.clear();
    canvasBefore_.reset();
}

void TrajectoryController::open(const QUrl &file)
{
    if (working() || !file.isLocalFile())
        return;
    file_ = file.toLocalFile();
    state_ = {};
    trackingOptions_ = {};
    selectionBox_ = {};
    end_.clear();
    preview(0.);
}

void TrajectoryController::preview(double seconds)
{
    if (working() || file_.isEmpty() || !std::isfinite(seconds) || seconds < 0.)
        return;
    start_ = seconds; position_ = seconds;
    clearResult();
    state_.edits = {};
    initial_ = {};
    selectionBox_ = {};
    emit settingsEdited(settings());
    begin(tr("正在读取起始帧…"), [file = file_, seconds] {
        const auto image = replay::readTrajectoryFrame(file, seconds);
        return Result{image, {}, image.isNull() ? tr("该时刻没有视频帧。") : tr("在起始画面框选无人机。")};
    });
}
void TrajectoryController::seek(double seconds)
{
    if (working() || !std::isfinite(seconds) || seconds < 0. || file_.isEmpty()) return;
    if (!trajectory_) { preview(seconds); return; }
    const auto next = std::clamp(seconds, start_, lastTime());
    if (position_ == next) return;
    position_ = next; updatePreview(); emit settingsEdited(settings()); emit changed();
}

void TrajectoryController::track(const QRectF &selection, const QString &endSeconds)
{
    if (working() || initial_.isNull())
        return;
    const bool fixedEnd = !endSeconds.trimmed().isEmpty();
    bool validEnd = false;
    const double end = endSeconds.toDouble(&validEnd);
    if (fixedEnd && (!validEnd || !std::isfinite(end) || end <= start_)) {
        status_ = tr("结束时间必须晚于开始时间。"); emit changed(); return;
    }
    selectionBox_ = selection;
    end_ = endSeconds;
    emit settingsEdited(settings());
    begin(tr("正在跟踪无人机并缓存拖影帧…"), [this, file = file_, start = start_, end, fixedEnd, selection, options = trackingOptions_] {
        auto result = std::make_shared<replay::FixedCameraTrajectory>(replay::trackTrajectory(file, start,
            fixedEnd ? std::optional(end) : std::nullopt, selection, [this] { return cancelled_.load(); },
            [this](double value) {
                QMetaObject::invokeMethod(this, [this, value] {
                    if (working_) { progress_ = value; emit changed(); }
                }, Qt::QueuedConnection);
            }, options));
        QString message = result->lostAt ? tr("跟踪在 %1 秒丢失，已保留此前的帧。").arg(*result->lostAt, 0, 'f', 3)
                                        : tr("已完成跟踪和前景提取，正在检查实际可见实例。");
        const auto failed = std::count_if(result->frames.begin(), result->frames.end(), [](const auto &frame) { return frame.alphaPixels <= 0; });
        if (failed == qsizetype(result->frames.size())) message = tr("前景提取失败：全部 %1 帧均无可见无人机，拖影导出已禁用。请指定背景或修正蒙版。 / All foreground extractions failed.").arg(failed);
        if (failed) message += tr(" %1/%2 帧未能可靠提取前景，已透明保留；请更换参考或调整选区。 / Foreground extraction incomplete.").arg(failed).arg(result->frames.size());
        if (!result->backgroundWarning.isEmpty()) message += ' ' + result->backgroundWarning;
        return Result{{}, result, message};
    });
}

void TrajectoryController::commit(State next)
{
    if (working() || next == state_)
        return;
    if (!canvasBefore_) pushUndo();
    state_ = std::move(next);
    if (trajectory_)
        updatePreview();
    emit settingsEdited(settings());
    emit changed();
}

TrajectoryController::History TrajectoryController::history() const
{
    return {state_, trajectory_, initial_, file_, end_, selectionBox_, start_, position_, trackingOptions_};
}
void TrajectoryController::pushUndo()
{
    if (undo_.size() == 100) undo_.erase(undo_.begin());
    undo_.push_back(history()); redo_.clear();
}
void TrajectoryController::beginCanvasEdit()
{
    if (!working() && !canvasBefore_) canvasBefore_ = history();
}
void TrajectoryController::endCanvasEdit(bool cancel)
{
    if (!canvasBefore_) return;
    auto before = std::move(*canvasBefore_); canvasBefore_.reset();
    if (cancel) restoreHistory(before);
    else if (before.state != state_) {
        if (undo_.size() == 100) undo_.erase(undo_.begin());
        undo_.push_back(std::move(before)); redo_.clear();
    }
    emit settingsEdited(settings()); emit changed();
}

QVariantMap TrajectoryController::canvasLayer(const QString &requestedId) const
{
    const auto rows = canvasLayers();
    for (const auto &row : rows) if (row.toMap().value("id").toString() == requestedId) return row.toMap();
    return {};
}
QVariantMap TrajectoryController::canvasState() const
{
    auto value = state_.canvas;
    const auto size = trajectory_ ? trajectory_->background.size() : initial_.isNull() ? QSize(1920,1080) : initial_.size();
    if (!value.contains("outputWidth")) value["outputWidth"] = size.width();
    if (!value.contains("outputHeight")) value["outputHeight"] = size.height();
    return value;
}
QVariantList TrajectoryController::canvasLayers() const
{
    if (!trajectory_) {
        if (initial_.isNull() || state_.canvas.value("background_removed").toBool()) return {};
        return {QVariantMap{{"id", "background"}, {"sourceId", "background"}, {"name", tr("Source video · 源视频")},
            {"kind", "trajectory-background"}, {"order", 0}, {"source", image()}, {"x", 0.}, {"y", 0.}, {"width", 1.}, {"height", 1.},
            {"sourceWidth", initial_.width()}, {"sourceHeight", initial_.height()}, {"opacity", 1.}}};
    }
    auto rows = replay::trajectoryCanvasLayerRows(*trajectory_, layerData_, state_.edits, state_.opacity, state_.canvas);
    const auto style = state_.edits.toJson();
    for (auto &value : rows) {
        auto row = value.toMap(); const auto id = row.value("sourceId").toString();
        const QString kind = id == "background" ? "background" : id.startsWith("sprite:") ? "sprite" : "vector";
        const QString index = kind == "sprite" ? QString::number(row.value("frameIndex").toInt()) : kind == "vector" ? id : QString();
        if (kind == "background") row["source"] = QUrl(QString("image://trajectory/background/%1").arg(sourceRevision_));
        else if (kind == "sprite") {
            const auto stamp = row.value("timeNs").toString().toLongLong();
            row["source"] = QUrl(QString("image://trajectory/sprite/%1/%2/%3").arg(sourceRevision_).arg(index).arg(replay::trajectoryMaskContentKey(state_.edits.frames.value(stamp))));
        } else row["source"] = QUrl(QString("image://trajectory/vector/%1/%2/%3").arg(sourceRevision_).arg(index).arg(generation_));
        if (kind == "vector") row["style"] = style.value(id == "line" ? "line_style" : id+"_style").toObject().toVariantMap();
        value = row;
    }
    return rows;
}
void TrajectoryController::paintCanvasLayer(QPainter &painter, const QString &id, const QRectF &target) const
{
    if (trajectory_) replay::paintTrajectoryLayer(painter,*trajectory_,layerData_,id,target,state_.edits,state_.canvas);
}
QString TrajectoryController::duplicateCanvasLayer(const QString &id)
{
    if (working() || canvasLayer(id).isEmpty()) return {};
    const QString copy = "trajectory-copy-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto next = state_; auto copies = next.canvas.value("copies").toMap();
    copies[copy] = canvasLayer(id).value("sourceId"); next.canvas["copies"] = copies; commit(std::move(next));
    return copy;
}
void TrajectoryController::removeCanvasLayers(const QStringList &ids)
{
    if (working() || ids.isEmpty()) return;
    auto next = state_; auto copies = next.canvas.value("copies").toMap();
    auto items = next.canvas.value("items").toMap();
    for (const auto &id : ids) {
        if (copies.remove(id)) { items.remove(id); continue; }
        if (id == "background") next.canvas["background_removed"] = true;
        else if (id == "line") { next.edits.showLine = false; next.edits.lineOnly = false; }
        else if (id == "start") next.edits.start.enabled = false;
        else if (id == "goal") next.edits.goal.enabled = false;
        else if (id.startsWith("sprite:")) {
            bool valid = false; const auto stamp = id.mid(7).toLongLong(&valid);
            if (valid) { next.edits.frames[stamp].removed = true; next.edits.frames[stamp].manual = false; }
        }
        items.remove(id);
    }
    next.canvas["copies"] = copies; next.canvas["items"] = items;
    commit(std::move(next));
}
void TrajectoryController::restoreHistory(const History &entry)
{
    const auto previousFile = file_;
    ++generation_; if (previewCancelled_) *previewCancelled_ = true;
    previewDirty_ = false; previewTimer_.stop();
    state_ = entry.state; trajectory_ = entry.trajectory; initial_ = entry.initial;
    file_ = entry.file; end_ = entry.end; selectionBox_ = entry.selection; start_ = entry.start; position_ = entry.position;
    trackingOptions_ = entry.trackingOptions; selectionAnchor_ = -1;
    if (!trajectory_ || file_ != previousFile) selected_.clear();
    else {
        QSet<qint64> available;
        for (const auto index : replay::editedTrajectorySamples(*trajectory_, state_.interval, state_.mode, state_.count, state_.edits))
            available.insert(replay::trajectoryTimestamp(trajectory_->frames[index].seconds));
        selected_.intersect(available);
    }
    sourceRevision_ = images_.setTrajectory(trajectory_); foregroundReady_ = false;
    if (trajectory_) updatePreview();
    else {
        sampled_.clear(); layerData_ = {}; foregroundCounts_ = {}; frames_.setFrames({}, {}, sourceRevision_);
        frames_.setSelection({}); setImage(initial_);
    }
}
void TrajectoryController::deleteSelectedFrames()
{
    if (working() || !trajectory_ || selected_.isEmpty()) return;
    auto next = state_;
    for (const auto stamp : selected_) {
        next.edits.frames[stamp].removed = true;
        next.edits.frames[stamp].manual = false;
    }
    selected_.clear(); commit(std::move(next));
}
void TrajectoryController::clearGenerated()
{
    if (working() || !trajectory_) return;
    pushUndo();
    auto next = history(); next.trajectory.reset(); next.state.edits = {}; next.state.canvas = {}; next.selection = {};
    restoreHistory(next); status_ = tr("已移除生成结果，保留源视频；可撤销。 / Generated result removed; source retained.");
    emit settingsEdited(settings()); emit changed();
}
void TrajectoryController::removeVideo()
{
    if (working() || file_.isEmpty()) return;
    pushUndo();
    History empty; restoreHistory(empty);
    status_ = tr("已从项目移除视频和生成结果，磁盘素材保留；可撤销。 / Video removed from project; source file retained.");
    emit settingsEdited(settings()); emit changed();
}

void TrajectoryController::configure(double interval, double opacity, int mode, int count)
{
    if (!std::isfinite(interval) || interval < 0. || !std::isfinite(opacity) || opacity < 0. || opacity > 1. ||
        mode < 0 || mode > 3 || (mode == 0 && interval < .1) || count < 1 || count > 1000000)
        return;
    auto next = state_;
    next.interval = interval; next.opacity = opacity; next.mode = mode; next.count = count;
    commit(std::move(next));
}

void TrajectoryController::updatePreview()
{
    const auto sampled = replay::editedTrajectorySamples(*trajectory_, state_.interval, state_.mode, state_.count, state_.edits);
    if (sampled_ != sampled) selectionAnchor_ = -1;
    sampled_ = sampled;
    // Model signals can immediately start asynchronous thumbnail requests.
    // Prepare saved mask snapshots before publishing their content URLs.
    images_.setEdits(state_.edits);
    frames_.setFrames(trajectory_, sampled_, sourceRevision_);
    frames_.setEdits(state_.edits);
    frames_.setSelection(selected_);
    foregroundReady_ = false;
    previewError_.clear();
    ++generation_;
    if (previewCancelled_)
        *previewCancelled_ = true;
    previewDirty_ = true;
    previewTimer_.start();
}

void TrajectoryController::renderPreview()
{
    if (!trajectory_ || !previewDirty_ || previewWatcher_.isRunning())
        return;
    previewDirty_ = false;
    previewCancelled_ = std::make_shared<std::atomic_bool>(false);
    // 单个后台合成任务 + 最新请求；过时结果不会覆盖新编辑，不在 UI 线程读磁盘帧。
    previewWatcher_.setFuture(QtConcurrent::run([track = trajectory_, samples = sampled_, state = state_, time = position_,
                                                generation = generation_, cancel = previewCancelled_] {
        try {
            auto layers = replay::trajectoryLayers(*track, samples, state.opacity, {}, state.edits, [cancel] { return cancel->load(); });
            auto size = QSize(state.canvas.value("outputWidth",track->background.width()).toInt(),state.canvas.value("outputHeight",track->background.height()).toInt());
            size.scale({1120, 760}, Qt::KeepAspectRatio);
            QImage image(size, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image);
            replay::TrajectoryCompositionStats counts;
            replay::paintTrajectoryLayers(painter, *track, layers, image.rect(), state.edits, [cancel] { return cancel->load(); },
                replay::TrajectoryAll, state.canvas, time, &counts);
            painter.end();
            return Preview{std::move(image), {}, generation, counts, std::move(layers)};
        } catch (const std::exception &error) {
            return Preview{{}, QString::fromUtf8(error.what()), generation};
        }
    }));
    emit changed();
}

void TrajectoryController::toggleFrame(int row)
{
    if (const auto stamp = frames_.stampAt(row)) {
        auto next = state_;
        next.edits.frames[*stamp].excluded = !next.edits.frames.value(*stamp).excluded;
        commit(std::move(next));
    }
}

void TrajectoryController::selectFrame(int row, bool extend, bool toggle)
{
    if (working()) return;
    const auto stamp = frames_.stampAt(row);
    if (!stamp)
        return;
    if (extend && selectionAnchor_ >= 0) {
        if (!toggle)
            selected_.clear();
        for (int i = std::min(row, selectionAnchor_); i <= std::max(row, selectionAnchor_); ++i)
            if (const auto value = frames_.stampAt(i)) selected_.insert(*value);
    } else {
        if (!toggle)
            selected_.clear();
        if (!toggle || !selected_.remove(*stamp))
            selected_.insert(*stamp);
        selectionAnchor_ = row;
    }
    frames_.setSelection(selected_);
    emit changed();
}

void TrajectoryController::editSelectedFrames(bool excluded, double opacity)
{
    if (!std::isfinite(opacity) || opacity > 1. || (opacity < 0. && opacity != -1.))
        return;
    auto next = state_;
    for (const auto stamp : selected_) {
        auto &frame = next.edits.frames[stamp];
        if (opacity < 0.) frame.excluded = excluded;
        else frame.opacity = opacity;
    }
    commit(std::move(next));
}

void TrajectoryController::addManualFrame(double seconds)
{
    if (!trajectory_ || !std::isfinite(seconds) || seconds < start_ || seconds > lastTime())
        return;
    // 选择不早于输入时间的第一张实际跟踪帧，保存实际源时间。
    const auto &frames = trajectory_->frames;
    const auto found = std::lower_bound(frames.begin(), frames.end(), seconds,
        [](const auto &frame, double time) { return frame.seconds < time - 1e-8; });
    if (found == frames.end())
        return;
    auto next = state_;
    auto &edit = next.edits.frames[replay::trajectoryTimestamp(found->seconds)];
    edit.manual = true;
    edit.removed = false;
    commit(std::move(next));
}

QVariantList TrajectoryController::excludedRanges() const
{
    QVariantList result;
    for (const auto &range : state_.edits.ranges)
        result.append(QVariantMap{{"id", range.id}, {"begin", double(range.begin) / 1e9},
            {"end", double(range.end) / 1e9}, {"parts", range.parts}});
    return result;
}

void TrajectoryController::addExcludedRange(double begin, double end, int parts)
{
    editExcludedRange({}, begin, end, parts);
}

void TrajectoryController::editExcludedRange(const QString &id, double begin, double end, int parts)
{
    try {
        replay::TrajectoryRange range{id.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128) : id,
            replay::trajectoryTimestamp(begin), replay::trajectoryTimestamp(end), parts};
        auto next = state_;
        const auto found = std::find_if(next.edits.ranges.begin(), next.edits.ranges.end(),
            [&](const auto &old) { return old.id == id; });
        if (id.isEmpty()) next.edits.ranges.push_back(range);
        else if (found != next.edits.ranges.end()) *found = range;
        else return;
        replay::TrajectoryExclusions valid(next.edits);
        commit(std::move(next));
    } catch (const std::exception &error) { reportError(error); }
}

void TrajectoryController::removeExcludedRange(const QString &id)
{
    auto next = state_;
    std::erase_if(next.edits.ranges, [&](const auto &range) { return range.id == id; });
    commit(std::move(next));
}

void TrajectoryController::splitExcludedRange(const QString &id, double at)
{
    try {
        const auto stamp = replay::trajectoryTimestamp(at);
        auto next = state_;
        const auto range = std::find_if(next.edits.ranges.begin(), next.edits.ranges.end(),
            [&](const auto &range) { return range.id == id; });
        if (range == next.edits.ranges.end() || stamp <= range->begin || stamp >= range->end)
            return;
        auto second = *range;
        range->end = stamp; second.begin = stamp;
        second.id = QUuid::createUuid().toString(QUuid::Id128);
        next.edits.ranges.push_back(std::move(second));
        commit(std::move(next));
    } catch (const std::exception &error) { reportError(error); }
}

void TrajectoryController::setGenerationRange(double begin, double end)
{
    try {
        auto next = state_;
        if (begin == -1. && end == -1.) next.edits.trim.reset();
        else {
            const auto from = replay::trajectoryTimestamp(begin), to = replay::trajectoryTimestamp(end);
            if (to <= from) throw std::invalid_argument("Generation end must be after its beginning.");
            next.edits.trim = {from, to};
        }
        commit(std::move(next));
    } catch (const std::exception &error) { reportError(error); }
}

void TrajectoryController::setShowLine(bool enabled)
{
    auto next = state_; next.edits.showLine = enabled; commit(std::move(next));
}
void TrajectoryController::setLineOnly(bool enabled)
{
    auto next = state_; next.edits.lineOnly = enabled; commit(std::move(next));
}
void TrajectoryController::setSkipInvalid(bool enabled)
{
    auto next = state_; next.edits.skipInvalid = enabled; commit(std::move(next));
}
void TrajectoryController::setLineStyle(const QVariantMap &style)
{
    try {
        auto next = state_; auto json = next.edits.toJson(); auto line = json.value("line_style").toObject();
        for (auto i = style.cbegin(); i != style.cend(); ++i) line.insert(i.key(), QJsonValue::fromVariant(i.value()));
        json["line_style"] = line; next.edits = replay::TrajectoryEdits::fromJson(json); commit(std::move(next));
    } catch (const std::exception &error) { reportError(error); }
}
void TrajectoryController::setEndpointStyle(bool goal, const QVariantMap &style)
{
    try {
        auto next = state_; auto json = next.edits.toJson(); const QString key = goal ? "goal_style" : "start_style";
        auto endpoint = json.value(key).toObject();
        for (auto i = style.cbegin(); i != style.cend(); ++i) endpoint.insert(i.key(), QJsonValue::fromVariant(i.value()));
        json[key] = endpoint; next.edits = replay::TrajectoryEdits::fromJson(json); commit(std::move(next));
    } catch (const std::exception &error) { reportError(error); }
}

QVariantMap TrajectoryController::foregroundCounts() const
{
    const auto &c = foregroundCounts_;
    const int extracted = trajectory_ ? int(std::count_if(trajectory_->frames.cbegin(), trajectory_->frames.cend(), [](const auto &frame) { return frame.alphaPixels > 0; })) : 0;
    return {{"tracked", c.tracked}, {"sampled", c.sampled}, {"successful", c.successful}, {"failed", c.failed},
        {"extracted", extracted}, {"extractionFailed", trajectory_ ? int(trajectory_->frames.size()) - extracted : 0},
        {"filtered", c.filtered}, {"composed", c.composed}, {"lineSegments", c.lineSegments},
        {"registered", trajectory_ ? trajectory_->registeredFrames : 0},
        {"registrationRejected", trajectory_ ? trajectory_->registrationRejected : 0}};
}

QVariantMap TrajectoryController::maskImages() const
{
    if (!trajectory_ || selected_.isEmpty()) return {};
    const auto stamp = *std::min_element(selected_.cbegin(), selected_.cend());
    const auto found = std::lower_bound(trajectory_->frames.begin(), trajectory_->frames.end(), stamp,
        [](const auto &frame, qint64 stamp) { return replay::trajectoryTimestamp(frame.seconds) < stamp; });
    if (found == trajectory_->frames.end()) return {};
    const auto index = found - trajectory_->frames.begin();
    const auto edit = state_.edits.frames.value(stamp);
    const auto content = replay::trajectoryMaskContentKey(edit);
    const auto url = [&](const QString &kind) { return QUrl(QString("image://trajectory/%1/%2/%3/%4").arg(kind).arg(sourceRevision_).arg(index).arg(kind == "source" ? QStringLiteral("base") : content)); };
    // Inspect the same immutable sprite as the preview, reusing its image cache.
    // Alpha only describes visible pixels; it does not certify clean extraction.
    const auto currentMask = images_.requestImage(url("sprite").path().mid(1), nullptr, {});
    return {{"source", url("source")}, {"mask", url("mask")}, {"sprite", url("sprite")},
        {"seconds", found->seconds}, {"reason", found->foregroundError}, {"width", found->bounds.width()}, {"height", found->bounds.height()},
        {"manuallyEdited", !edit.maskStrokes.empty()}, {"currentMaskAvailable", !currentMask.isNull()},
        {"currentAlphaPixels", replay::trajectoryAlphaPixels(currentMask)}};
}

void TrajectoryController::paintMask(const QVariantList &points, double radius, bool erase)
{
    if (!trajectory_ || selected_.isEmpty() || points.isEmpty() || points.size() > 100000 || !std::isfinite(radius) || radius <= 0. || radius > .5) return;
    replay::TrajectoryMaskStroke stroke; stroke.radius = radius; stroke.erase = erase;
    for (const auto &value : points) {
        const auto point = value.toPointF();
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || point.x() < 0. || point.x() > 1. || point.y() < 0. || point.y() > 1.) return;
        stroke.points.append(point);
    }
    auto next = state_;
    const auto stamp = *std::min_element(selected_.cbegin(), selected_.cend());
    next.edits.frames[stamp].maskStrokes.push_back(std::move(stroke));
    commit(std::move(next));
}
void TrajectoryController::resetMask()
{
    if (selected_.isEmpty()) return;
    auto next = state_;
    const auto stamp = *std::min_element(selected_.cbegin(), selected_.cend());
    next.edits.frames[stamp].maskStrokes.clear(); commit(std::move(next));
}
void TrajectoryController::setBackgroundReference(const QUrl &file, const QString &seconds, bool registerCamera)
{
    if (working() || trajectory_) return;
    bool valid = false;
    const double time = seconds.trimmed().isEmpty() ? -1. : seconds.toDouble(&valid);
    if ((!seconds.trimmed().isEmpty() && (!valid || !std::isfinite(time) || time < 0.)) || (!file.isEmpty() && !file.isLocalFile())) return;
    trackingOptions_.backgroundFile = file.toLocalFile();
    trackingOptions_.backgroundSeconds = time < 0. ? std::nullopt : std::optional(time);
    trackingOptions_.registerCamera = registerCamera;
    emit settingsEdited(settings()); emit changed();
}
void TrajectoryController::undo()
{
    if (working() || undo_.empty()) return;
    redo_.push_back(history()); const auto entry = undo_.back(); undo_.pop_back(); restoreHistory(entry);
    emit settingsEdited(settings()); emit changed();
}
void TrajectoryController::redo()
{
    if (working() || redo_.empty()) return;
    undo_.push_back(history()); const auto entry = redo_.back(); redo_.pop_back(); restoreHistory(entry);
    emit settingsEdited(settings()); emit changed();
}
void TrajectoryController::restoreFrames()
{
    auto next = state_;
    next.edits.ranges.clear();
    for (auto &frame : next.edits.frames) frame.excluded = false;
    commit(std::move(next));
}
void TrajectoryController::resetPreview()
{
    if (working() || initial_.isNull()) return;
    if (trajectory_) clearGenerated();
    selectionBox_ = {}; status_ = tr("在起始画面重新框选无人机。");
    emit settingsEdited(settings()); emit changed();
}

QVariantMap TrajectoryController::settings() const
{
    if (file_.isEmpty()) return {};
    return {{"path", file_}, {"start", start_}, {"end", end_}, {"interval", state_.interval},
        {"opacity", state_.opacity}, {"mode", state_.mode}, {"count", state_.count},
        {"selection", QVariantList{selectionBox_.x(), selectionBox_.y(), selectionBox_.width(), selectionBox_.height()}},
        {"canvas", state_.canvas}, {"position", position_},
        {"edits", state_.edits.toJson().toVariantMap()}, {"background_file", trackingOptions_.backgroundFile},
        {"background_seconds", trackingOptions_.backgroundSeconds.value_or(-1.)}, {"register_camera", trackingOptions_.registerCamera}};
}

void TrajectoryController::restoreSettings(const QVariantMap &settings)
{
    if (working()) { pendingSettings_ = settings; cancelled_ = true; return; }
    try {
        State next;
        next.canvas = settings.value("canvas").toMap();
        next.edits = replay::TrajectoryEdits::fromJson(QJsonObject::fromVariantMap(settings.value("edits").toMap()));
        next.interval = settings.value("interval", .5).toDouble();
        next.opacity = settings.value("opacity", .9).toDouble();
        next.mode = settings.value("mode", 0).toInt(); next.count = settings.value("count", 12).toInt();
        const auto start = settings.value("start", 0.).toDouble();
        replay::trajectoryTimestamp(start);
        if (!std::isfinite(next.interval) || next.interval < 0. || !std::isfinite(next.opacity) ||
            next.opacity < 0. || next.opacity > 1. || next.mode < 0 || next.mode > 3 ||
            (next.mode == 0 && next.interval < .1) || next.count < 1 || next.count > 1000000)
            throw std::invalid_argument("Invalid saved trajectory sampling settings.");
        const auto box = settings.value("selection").toList();
        QRectF selection;
        if (box.size() == 4) selection = {box[0].toDouble(), box[1].toDouble(), box[2].toDouble(), box[3].toDouble()};
        clearResult(); state_ = std::move(next); start_ = start;
        position_ = settings.value("position", start).toDouble();
        if (!std::isfinite(position_) || position_ < 0.) position_ = start;
        trackingOptions_.backgroundFile = settings.value("background_file").toString();
        const double referenceTime = settings.value("background_seconds", -1.).toDouble();
        if (!std::isfinite(referenceTime) || referenceTime < -1.) throw std::invalid_argument("Invalid background reference time.");
        trackingOptions_.backgroundSeconds = referenceTime < 0. ? std::nullopt : std::optional(referenceTime);
        trackingOptions_.registerCamera = settings.value("register_camera").toBool();
        file_ = settings.value("path").toString(); end_ = settings.value("end").toString();
        selectionBox_ = selection; initial_ = {};
        resumeTracking_ = !selection.isEmpty();
        if (file_.isEmpty()) { emit changed(); return; }
        if (!QFileInfo(file_).isFile()) throw std::runtime_error("Trajectory video is missing. Relocate the source video before resuming.");
        begin(tr("正在恢复轨迹图源视频与编辑状态…"), [file = file_, start] {
            const auto image = replay::readTrajectoryFrame(file, start);
            return Result{image, {}, image.isNull() ? tr("无法恢复起始帧。") : tr("已恢复轨迹图编辑设置。")};
        });
    } catch (const std::exception &error) { reportError(error); }
}

void TrajectoryController::save(const QUrl &target)
{
    if (working() || !trajectory_ || !target.isLocalFile()) return;
    const QString path = target.toLocalFile();
    if (QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) != 0) return;
    if (QFileInfo(path).exists() && QFileInfo(path).canonicalFilePath() == QFileInfo(file_).canonicalFilePath()) {
        status_ = tr("导出不能覆盖源素材。 / Cannot overwrite source media.");
        emit changed(); return;
    }
    auto render = [this, path, track = trajectory_, samples = sampled_, state = state_, time = position_] {
        QSaveFile file(path);
        const auto exported = replay::renderTrajectory(*track, samples, state.opacity, {}, {}, state.edits,
            [this] { return cancelled_.load(); }, false, nullptr, true, state.canvas, time);
        if (cancelled_) return Result{};
        if (!file.open(QIODevice::WriteOnly) || exported.isNull() || !exported.save(&file, "PNG"))
            throw std::runtime_error("Unable to write trajectory PNG.");
        if (cancelled_) return Result{};
        if (!file.commit()) throw std::runtime_error("Unable to save trajectory PNG.");
        return Result{{}, {}, tr("已保存原分辨率拖影 PNG。")};
    };
    cancelled_ = false; working_ = true; progress_ = 0.;
    exportAfterPreview([this, render = std::move(render)]() mutable {
        begin(tr("正在保存原分辨率 PNG…"), std::move(render));
    }, [this](const QString &error) {
        working_ = false; status_ = error; emit changed();
        if (pendingSettings_) restoreSettings(*std::exchange(pendingSettings_, std::nullopt));
    });
}
void TrajectoryController::exportFrame(const QUrl &target, const QVariantMap &options)
{
    if (working() || (initial_.isNull() && !trajectory_)) return;
    const auto path = target.toLocalFile();
    const auto scope = options.value("scope","canvas").toString();
    const auto layerIds = options.value("layerIds").toStringList();
    frameExportReport_ = {{"scope",scope},{"layerIds",layerIds},{"success",false},{"composedUavInstances",0},{"cleanForegroundAccepted",false}};
    const int width = options.value("width", 1920).toInt(), height = options.value("height", 1080).toInt(), dpi = options.value("dpi", 300).toInt();
    const QColor background(options.value("background", "transparent").toString());
    const auto reject = [this](const QString &error) { frameExportReport_["error"]=error; status_ = error; emit changed(); emit frameExported(false, error); };
    if ((scope!="canvas" && scope!="layers") || options.value("allViews").toBool()) {
        reject(tr("轨迹单帧导出支持完整画布或选中图层。 / Use canvas or selected layers scope.")); return;
    }
    if (scope=="layers" && (layerIds.isEmpty() || std::any_of(layerIds.cbegin(),layerIds.cend(),[](const auto &id){return id.trimmed().isEmpty();}))) {
        reject(tr("请选择至少一个有效图层。 / Select at least one valid layer.")); return;
    }
    if (!target.isLocalFile() || !QStringList{"png", "jpg", "jpeg", "tif", "tiff", "pdf", "svg"}.contains(QFileInfo(path).suffix().toLower()) ||
        (QFileInfo(path).exists() && QFileInfo(path).canonicalFilePath() == QFileInfo(file_).canonicalFilePath())) {
        reject(tr("无效输出格式或尝试覆盖源素材。 / Invalid output or source overwrite.")); return;
    }
    if (width < 16 || height < 16 || width > 8192 || height > 8192 || qint64(width) * height > 40'000'000 || dpi < 36 || dpi > 2400 || !background.isValid()) {
        reject(tr("尺寸 16–8192，最多 4000 万像素，DPI 36–2400；请检查背景颜色。")); return;
    }
    cancelled_ = false;
    exporting_ = true; progress_ = 0.;
    auto render = [this, path, width, height, dpi, background,
        file = file_, track = trajectory_, samples = sampled_, state = state_, time = position_, scope, layerIds] {
        FrameExport result;
        result.report = {{"scope",scope},{"layerIds",layerIds},{"composedUavInstances",0},{"foregroundGateApplied",false},{"cleanForegroundAccepted",false}};
        const auto finish = [&](const QString &error) { result.error=error; result.report["success"]=error.isEmpty(); result.report["error"]=error; return result; };
        const auto selected = scope=="layers" ? std::optional(layerIds) : std::nullopt;
        try {
            // 直接重用全质量轨迹合成，不读取预览缩略图，也不截取编辑控件。
            if (track) {
                result.report["content"]="trajectory";
                const auto error = replay::writeTrajectoryStillImage(*track,samples,state.edits,state.opacity,state.canvas,path,{width,height},dpi,time,background,[this] { return cancelled_.load(); },selected,&result.report);
                return finish(error);
            }
            QImage frame; double outputTime = time; QString sourceStamp;
            {
                replay::VideoReader reader(file, [this] { return cancelled_.load(); });
                if (!reader.select(time, [this] { return cancelled_.load(); })) return finish(tr("该时间无源视频画面。 / No source frame at this time."));
                outputTime = reader.seconds;
                sourceStamp = QString("pts=%1; time_base=%2/%3; origin_pts=%4; kind=%5").arg(reader.pts()).arg(reader.timeBaseNumerator())
                    .arg(reader.timeBaseDenominator()).arg(reader.originPts()).arg(reader.hasSourcePts() ? "source_pts" : "decoder_recovered");
                sourceStamp += QString("; coded_size=%1x%2; sample_aspect_ratio=%3/%4; display_rotation_degrees=%5; display_size=%6x%7; pixel_normalization=square_pixels_then_display_rotation")
                    .arg(reader.codedSize().width()).arg(reader.codedSize().height()).arg(reader.sampleAspectNumerator())
                    .arg(reader.sampleAspectDenominator()).arg(reader.displayRotation()).arg(reader.frame.cols).arg(reader.frame.rows);
                frame = replay::imageOf(reader.frame);
            }
            result.report["content"]="source"; result.report["time"]=outputTime; result.report["sourceTimestamp"]=sourceStamp;
            result.report["foregroundValidation"]="not_applicable_source_video";
            if (cancelled_) return finish(tr("已取消。 / Cancelled."));
            if (frame.isNull()) return finish(tr("源画面为空。 / Empty source frame."));
            if (selected) {
                replay::FixedCameraTrajectory sourceCanvas; sourceCanvas.background=frame;
                const auto error = replay::writeTrajectoryStillImage(sourceCanvas,{},state.edits,1.,state.canvas,path,{width,height},dpi,outputTime,background,[this] { return cancelled_.load(); },selected,&result.report);
                return finish(error);
            }
            frame = frame.scaled(width,height,Qt::KeepAspectRatio,Qt::SmoothTransformation);
            QImage output(width, height, QImage::Format_ARGB32_Premultiplied);
            if (output.isNull()) return finish(tr("内存不足。 / Insufficient memory."));
            output.fill(background); QPainter painter(&output);
            painter.drawImage(QPoint((width - frame.width()) / 2, (height - frame.height()) / 2), frame); painter.end();
            if (cancelled_) return finish(tr("已取消。 / Cancelled."));
            if (!sourceStamp.isEmpty()) output.setText("SourceTimestamp", sourceStamp);
            result.report["paintedLayers"]=1; result.report["width"]=width; result.report["height"]=height; result.report["dpi"]=dpi;
            return finish(replay::writeStillImage(output, path, dpi, outputTime));
        } catch (const std::exception &error) { return finish(QString::fromUtf8(error.what())); }
    };
    auto start = [this, render = std::move(render)]() mutable {
        status_ = tr("正在从源素材重新合成导出… / Rendering source-quality frame…");
        exportWatcher_.setFuture(QtConcurrent::run(std::move(render)));
        emit changed();
    };
    exportAfterPreview(std::move(start), [this](const QString &error) { finishFrameExport({error,frameExportReport_}); });
}
void TrajectoryController::exportFrames(const QUrl &parentDirectory, const QVariantMap &options)
{
    if (working() || file_.isEmpty() || (initial_.isNull() && !trajectory_)) return;
    if (!parentDirectory.isLocalFile()) {
        status_ = tr("请选择本地输出文件夹。 / Select a local output directory.");
        emit changed(); emit batchExported(false, status_); return;
    }
    cancelled_ = false; exporting_ = true; progress_ = 0.; exportDirectory_.clear();
    auto render = [this, file = file_, directory = parentDirectory.toLocalFile(),
        track = trajectory_, samples = sampled_, state = state_, time = position_, options] {
        return replay::exportTrajectoryBatch(file, directory, track, samples, state.edits, state.opacity, time, options,
            [this] { return cancelled_.load(); }, [this](double value) {
                QMetaObject::invokeMethod(this, [this, value] {
                    if (exporting_) { progress_ = value; emit changed(); }
                }, Qt::QueuedConnection);
            },state.canvas);
    };
    auto start = [this, render = std::move(render)]() mutable {
        status_ = tr("正在批量合成完整质量画面… / Rendering full-quality batch…");
        batchWatcher_.setFuture(QtConcurrent::run(std::move(render)));
        emit changed();
    };
    const auto fail = [this](const QString &error) {
        exporting_ = false; status_ = error; emit changed(); emit batchExported(false,status_);
        if (pendingSettings_) restoreSettings(*std::exchange(pendingSettings_, std::nullopt));
    };
    if (options.value("content","source").toString() == "trajectory") exportAfterPreview(std::move(start),fail);
    else start();
}
void TrajectoryController::cancel()
{
    cancelled_ = true;
    failPreviewExport(tr("已取消。 / Cancelled."));
}
