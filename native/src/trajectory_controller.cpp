#include "trajectory_controller.h"
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
    trajectory_ = std::move(trajectory);
    thumbnails_.clear();
    return ++sourceRevision_;
}

QImage PreviewImages::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QImage image;
    std::shared_ptr<const replay::FixedCameraTrajectory> track;
    std::optional<size_t> source;
    {
        const QMutexLocker locker(&lock_);
        if (id.startsWith("frame/")) {
            const auto parts = id.split('/');
            bool revisionOk = false, indexOk = false;
            const auto revision = parts.value(1).toULongLong(&revisionOk);
            const auto index = parts.value(2).toULongLong(&indexOk);
            if (parts.size() != 3 || !revisionOk || !indexOk || revision != sourceRevision_ ||
                !trajectory_ || index >= trajectory_->frames.size())
                return {};
            if (const auto *cached = thumbnails_.object(id))
                image = *cached;
            else {
                track = trajectory_;
                source = size_t(index);
            }
        } else {
            image = image_;
        }
    }
    if (source) {
        const auto &frame = track->frames[*source];
        image = frame.cachedSprite.isEmpty() ? frame.sprite : QImage(frame.cachedSprite);
        image = image.scaled({160, 100}, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
    connect(&exportWatcher_, &QFutureWatcher<QString>::finished, this, [this] {
        const auto error = exportWatcher_.result();
        status_ = error.isEmpty() ? tr("已导出完整质量画面。 / Full-quality frame exported.") : error;
        emit changed(); emit frameExported(error.isEmpty(), status_);
    });
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(50);
    connect(&previewTimer_, &QTimer::timeout, this, &TrajectoryController::renderPreview);
    connect(&previewWatcher_, &QFutureWatcher<Preview>::finished, this, [this] {
        const auto result = previewWatcher_.result();
        if (result.generation == generation_) {
            if (!result.image.isNull())
                setImage(result.image);
            if (!result.error.isEmpty())
                status_ = result.error;
        }
        if (previewDirty_ && !previewTimer_.isActive())
            renderPreview();
        emit changed();
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
    previewTimer_.stop();
    trajectory_.reset();
    sourceRevision_ = images_.setTrajectory({});
    sampled_.clear();
    selected_.clear();
    selectionAnchor_ = -1;
    frames_.setFrames({}, {}, sourceRevision_);
    frames_.setSelection({});
    undo_.clear();
    redo_.clear();
}

void TrajectoryController::open(const QUrl &file)
{
    if (working_ || !file.isLocalFile())
        return;
    file_ = file.toLocalFile();
    state_ = {};
    selectionBox_ = {};
    end_.clear();
    preview(0.);
}

void TrajectoryController::preview(double seconds)
{
    if (working_ || file_.isEmpty() || !std::isfinite(seconds) || seconds < 0.)
        return;
    start_ = seconds;
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

void TrajectoryController::track(const QRectF &selection, const QString &endSeconds)
{
    if (working_ || initial_.isNull())
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
    begin(tr("正在跟踪无人机并缓存拖影帧…"), [this, file = file_, start = start_, end, fixedEnd, selection] {
        auto result = std::make_shared<replay::FixedCameraTrajectory>(replay::trackTrajectory(file, start,
            fixedEnd ? std::optional(end) : std::nullopt, selection, [this] { return cancelled_.load(); },
            [this](double value) {
                QMetaObject::invokeMethod(this, [this, value] {
                    if (working_) { progress_ = value; emit changed(); }
                }, Qt::QueuedConnection);
            }));
        return Result{{}, result, result->lostAt ? tr("跟踪在 %1 秒丢失，已保留此前的帧。").arg(*result->lostAt, 0, 'f', 3)
                                               : tr("已生成拖影，可编辑采样、不透明度与排除区间。")};
    });
}

void TrajectoryController::commit(State next)
{
    if (working_ || next == state_)
        return;
    if (undo_.size() == 100)
        undo_.erase(undo_.begin());
    undo_.push_back(state_);
    redo_.clear();
    state_ = std::move(next);
    if (trajectory_)
        updatePreview();
    emit settingsEdited(settings());
    emit changed();
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
    frames_.setFrames(trajectory_, sampled_, sourceRevision_);
    frames_.setEdits(state_.edits);
    frames_.setSelection(selected_);
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
    previewWatcher_.setFuture(QtConcurrent::run([track = trajectory_, samples = sampled_, state = state_,
                                                generation = generation_, cancel = previewCancelled_] {
        try {
            return Preview{replay::renderTrajectory(*track, samples, state.opacity, {}, {1120, 760},
                state.edits, [cancel] { return cancel->load(); }), {}, generation};
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
    next.edits.frames[replay::trajectoryTimestamp(found->seconds)].manual = true;
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
void TrajectoryController::undo()
{
    if (working_ || undo_.empty()) return;
    redo_.push_back(state_); state_ = undo_.back(); undo_.pop_back();
    if (trajectory_) updatePreview();
    emit settingsEdited(settings()); emit changed();
}
void TrajectoryController::redo()
{
    if (working_ || redo_.empty()) return;
    undo_.push_back(state_); state_ = redo_.back(); redo_.pop_back();
    if (trajectory_) updatePreview();
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
    if (working_ || initial_.isNull()) return;
    clearResult(); state_.edits = {}; selectionBox_ = {};
    setImage(initial_); status_ = tr("在起始画面重新框选无人机。");
    emit settingsEdited(settings()); emit changed();
}

QVariantMap TrajectoryController::settings() const
{
    if (file_.isEmpty()) return {};
    return {{"path", file_}, {"start", start_}, {"end", end_}, {"interval", state_.interval},
        {"opacity", state_.opacity}, {"mode", state_.mode}, {"count", state_.count},
        {"selection", QVariantList{selectionBox_.x(), selectionBox_.y(), selectionBox_.width(), selectionBox_.height()}},
        {"edits", state_.edits.toJson().toVariantMap()}};
}

void TrajectoryController::restoreSettings(const QVariantMap &settings)
{
    if (working_) { pendingSettings_ = settings; cancelled_ = true; return; }
    try {
        State next;
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
    if (working_ || !trajectory_ || !target.isLocalFile()) return;
    const QString path = target.toLocalFile();
    if (QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) != 0) return;
    if (QFileInfo(path).exists() && QFileInfo(path).canonicalFilePath() == QFileInfo(file_).canonicalFilePath()) {
        status_ = tr("导出不能覆盖源素材。 / Cannot overwrite source media.");
        emit changed(); return;
    }
    begin(tr("正在保存原分辨率 PNG…"), [this, path, track = trajectory_, samples = sampled_, state = state_] {
        QSaveFile file(path);
        const auto exported = replay::renderTrajectory(*track, samples, state.opacity, {}, {}, state.edits,
            [this] { return cancelled_.load(); });
        if (cancelled_) return Result{};
        if (!file.open(QIODevice::WriteOnly) || exported.isNull() || !exported.save(&file, "PNG"))
            throw std::runtime_error("Unable to write trajectory PNG.");
        if (cancelled_) return Result{};
        if (!file.commit()) throw std::runtime_error("Unable to save trajectory PNG.");
        return Result{{}, {}, tr("已保存原分辨率拖影 PNG。")};
    });
}
void TrajectoryController::exportFrame(const QUrl &target, const QVariantMap &options)
{
    if (working() || (initial_.isNull() && !trajectory_)) return;
    const auto path = target.toLocalFile();
    const int width = options.value("width", 1920).toInt(), height = options.value("height", 1080).toInt(), dpi = options.value("dpi", 300).toInt();
    const QColor background(options.value("background", "transparent").toString());
    const auto reject = [this](const QString &error) { status_ = error; emit changed(); emit frameExported(false, error); };
    if (!target.isLocalFile() || !QStringList{"png", "jpg", "jpeg", "tif", "tiff", "pdf", "svg"}.contains(QFileInfo(path).suffix().toLower()) ||
        (QFileInfo(path).exists() && QFileInfo(path).canonicalFilePath() == QFileInfo(file_).canonicalFilePath())) {
        reject(tr("无效输出格式或尝试覆盖源素材。 / Invalid output or source overwrite.")); return;
    }
    if (width < 16 || height < 16 || width > 8192 || height > 8192 || qint64(width) * height > 40'000'000 || dpi < 36 || dpi > 2400 || !background.isValid()) {
        reject(tr("尺寸 16–8192，最多 4000 万像素，DPI 36–2400；请检查背景颜色。")); return;
    }
    cancelled_ = false;
    status_ = tr("正在从源素材重新合成导出… / Rendering source-quality frame…");
    exportWatcher_.setFuture(QtConcurrent::run([this, path, width, height, dpi, background,
        initial = initial_, track = trajectory_, samples = sampled_, state = state_, time = start_] {
        try {
            // 直接重用全质量轨迹合成，不读取预览缩略图，也不截取编辑控件。
            QImage frame = track ? replay::renderTrajectory(*track, samples, state.opacity, {}, {width, height}, state.edits,
                [this] { return cancelled_.load(); }, true) : initial.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (cancelled_) return tr("已取消。 / Cancelled.");
            if (frame.isNull()) return tr("源画面为空。 / Empty source frame.");
            QImage output(width, height, QImage::Format_ARGB32_Premultiplied);
            if (output.isNull()) return tr("内存不足。 / Insufficient memory.");
            output.fill(background); QPainter painter(&output);
            painter.drawImage(QPoint((width - frame.width()) / 2, (height - frame.height()) / 2), frame); painter.end();
            if (cancelled_) return tr("已取消。 / Cancelled.");
            return replay::writeStillImage(output, path, dpi, time);
        } catch (const std::exception &error) { return QString::fromUtf8(error.what()); }
    }));
    emit changed();
}
void TrajectoryController::cancel() { cancelled_ = true; }
