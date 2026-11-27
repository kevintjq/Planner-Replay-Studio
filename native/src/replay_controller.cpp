#include "replay_controller.h"

#include <QtConcurrentRun>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

ReplayController::ReplayController(QObject *parent) : QObject(parent), record_(replay::makeDemo())
{
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] {
        setPosition(start_ + clock_.nsecsElapsed() / 1.e9);
        if (position_ >= duration())
            pause();
    });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        auto result = watcher_.result();
        loading_ = false;
        if (!cancelRequested_ && result.record) {
            path_ = result.path;
            mapMode_ = result.mode;
            project_ = std::move(result.project);
            status_ = result.record->odometryTopic.isEmpty() ? tr("点云已载入。此记录没有里程计。")
                : result.record->poses.empty() ? tr("点云已载入。所选里程计与点云坐标系不匹配。") : tr("载入完成");
            if (project_)
                status_ += tr(" · 项目已载入。");
            if (!result.record->topicWarnings.isEmpty()) {
                const auto &warnings = result.record->topicWarnings;
                status_ += tr(" · 暂不可显示 %1 个导入项（消息类型、话题或坐标系不匹配）：%2")
                               .arg(warnings.size()).arg(warnings.mid(0, 5).join(QStringLiteral("、")));
            }
            if (project_) {
                const auto fixedFrame = project_->topicLayout().fixedFrame;
                if (!fixedFrame.isEmpty() && fixedFrame != result.record->frameId)
                    status_ += tr(" · 固定坐标系 %1 已保存，当前使用 %2。").arg(fixedFrame, result.record->frameId);
            }
            if (!result.skipped.isEmpty())
                status_ += tr(" · 无法按原插件显示：%1").arg(result.skipped.mid(0, 5).join(QStringLiteral("、")));
            install(std::move(result.record), result.keepSession);
        } else {
            status_ = cancelRequested_ ? tr("已取消读取") : result.issue;
            emit recordingChanged();
        }
        emit stateChanged();
    });
    connect(&folderWatcher_, &QFutureWatcher<QString>::finished, this, [this] {
        loading_ = false;
        status_ = folderWatcher_.result();
        emit stateChanged();
    });
}

ReplayController::~ReplayController()
{
    cancelRequested_ = true;
    watcher_.waitForFinished();
    folderWatcher_.waitForFinished();
}

void ReplayController::install(std::shared_ptr<const replay::Recording> record, bool keepSession)
{
    pause();
    record_ = std::move(record);
    position_ = keepSession ? std::min(position_, duration()) : 0.;
    if (!keepSession) {
        undo_.clear();
        redo_.clear();
        const auto nextClips = project_ ? project_->videos() : std::vector<replay::VideoClip>();
        QHash<QString, double> durations;
        for (const auto &clip : nextClips) {
            if (videoDurations_.contains(clip.path))
                durations.insert(clip.path, videoDurations_.value(clip.path));
        }
        videoDurations_.swap(durations);
        emit editHistoryChanged();
    }
    refreshVideoLayers();
    emit recordingChanged();
    emit positionChanged();
}

QVariantList ReplayController::topicDisplays() const
{
    QVariantList result;
    if (!project_)
        return result;

    const auto layout = project_->topicLayout();
    for (const auto &display : layout.displays) {
        const auto label = display.name.isEmpty() ? display.topic : display.name + QStringLiteral(" · ") + display.topic;
        result.append(QVariantMap{{"label", label}, {"enabled", display.enabled}, {"color", display.color}});
    }
    return result;
}

void ReplayController::setTopicDisplayEnabled(int index, bool enabled)
{
    if (loading_ || !project_)
        return;
    auto layout = project_->topicLayout();
    if (index < 0 || size_t(index) >= layout.displays.size())
        return;
    if (layout.displays[size_t(index)].enabled != enabled) {
        layout.displays[size_t(index)].enabled = enabled;
        applyTopicLayout(layout);
    }
}

void ReplayController::openRviz(const QUrl &url)
{
    if (loading_ || !url.isLocalFile())
        return;
    try {
        applyTopicLayout(replay::readRviz(url.toLocalFile()));
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
        emit stateChanged();
    }
}

void ReplayController::applyTopicLayout(const replay::RvizLayout &layout)
{
    auto document = projectSnapshot();
    document.setTopicLayout(layout);
    if (!path_.isEmpty()) {
        load(path_, {cloudTopic(), odometryTopic(), mapMode_}, std::move(document), true, layout.skipped);
        return;
    }

    project_ = std::move(document);
    status_ = tr("已保存 %1 个 RViz 话题显示。打开录包后载入匹配的图层。").arg(layout.displays.size());
    if (!layout.skipped.isEmpty())
        status_ += tr(" · 无法按原插件显示：%1").arg(layout.skipped.mid(0, 5).join(QStringLiteral("、")));
    emit recordingChanged();
    emit stateChanged();
}

void ReplayController::openBag(const QUrl &url)
{
    if (!url.isLocalFile())
        return;
    const auto path = url.toLocalFile();
    if (!path_.isEmpty() || !project_) {
        load(path, {});
        return;
    }

    const auto layout = project_->topicLayout();
    if (layout.fixedFrame.isEmpty() && layout.displays.empty()) {
        load(path, {});
        return;
    }

    auto document = replay::ProjectDocument::create(QStringLiteral("bag"), path);
    document.setTopicLayout(layout);
    load(path, {}, std::move(document));
}

void ReplayController::openDemo()
{
    if (loading_)
        return;
    path_.clear();
    project_.reset();
    mapMode_.reset();
    status_.clear();
    install(replay::makeDemo());
    emit stateChanged();
}

void ReplayController::openProject(const QUrl &url)
{
    if (loading_ || !url.isLocalFile())
        return;
    try {
        auto project = replay::ProjectDocument::load(url.toLocalFile());
        project.videos();
        if (!project.bag().isEmpty()) {
            if (!QFileInfo(project.bag()).isFile())
                throw std::runtime_error(tr("找不到项目录包：%1").arg(project.bag()).toUtf8().constData());
            const auto settings = project.values();
            const auto mode = settings.value("cloud_mode").toString();
            std::optional<replay::MapMode> mapMode;
            if (mode == QStringLiteral("替换整图"))
                mapMode = replay::MapMode::Replace;
            else if (mode == QStringLiteral("累积扫描"))
                mapMode = replay::MapMode::Accumulate;
            const auto bag = project.bag();
            load(bag, {settings.value("cloud_topic").toString(),
                       settings.value("odom_topic").toString(), mapMode}, std::move(project));
            return;
        }

        path_.clear();
        mapMode_.reset();
        project_ = std::move(project);
        status_ = tr("项目设置已保留。");
        if (project_->source() == "demo") {
            install(replay::makeDemo());
        } else {
            auto empty = std::make_shared<replay::Recording>();
            empty->name = QFileInfo(project_->path()).fileName();
            empty->frames.push_back({0, 0, 0});
            install(std::move(empty));
        }
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

replay::ProjectDocument ReplayController::projectSnapshot() const
{
    if (project_)
        return *project_;
    auto project = replay::ProjectDocument::create(demo() ? QStringLiteral("demo") : QStringLiteral("bag"), path_);
    project.setValue("cloud_topic", cloudTopic());
    project.setValue("odom_topic", odometryTopic());
    project.setValue("cloud_mode", replacing() ? QStringLiteral("替换整图") : QStringLiteral("累积扫描"));
    return project;
}

void ReplayController::saveProjectAs(const QUrl &url)
{
    if (loading_ || !url.isLocalFile())
        return;
    try {
        project_ = projectSnapshot().saveAs(url.toLocalFile());
        status_ = tr("项目已保存：%1").arg(QFileInfo(project_->path()).fileName());
        emit recordingChanged();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::exportProjectFolder(const QUrl &parent)
{
    if (loading_ || !parent.isLocalFile())
        return;
    auto document = projectSnapshot();
    QString name = QFileInfo(document.path()).completeBaseName();
    if (name.isEmpty())
        name = QFileInfo(path_).completeBaseName().isEmpty() ? QStringLiteral("Planner Replay")
                                                            : QFileInfo(path_).completeBaseName();
    const QDir directory(parent.toLocalFile());
    QString destination = directory.filePath(name + QStringLiteral(" - project"));
    for (int i = 2; QFileInfo::exists(destination); ++i)
        destination = directory.filePath(QStringLiteral("%1 - project (%2)").arg(name).arg(i));

    pause();
    cancelRequested_ = false;
    progress_ = 0.;
    loading_ = true;
    status_ = tr("正在打包项目和素材…");
    emit stateChanged();
    folderWatcher_.setFuture(QtConcurrent::run([this, document, destination] {
        try {
            const auto manifest = document.exportFolder(destination, [this] { return cancelRequested_.load(); });
            return tr("项目已打包：%1").arg(QDir::toNativeSeparators(manifest));
        } catch (const std::exception &error) {
            return QString::fromUtf8(error.what());
        }
    }));
}

void ReplayController::setCloudTopic(const QString &topic)
{
    if (!path_.isEmpty() && !topic.isEmpty() && topic != cloudTopic()) {
        auto document = project_;
        if (document)
            document->setValue("cloud_topic", topic);
        load(path_, {topic, odometryTopic(), mapMode_}, std::move(document), true);
    }
}

void ReplayController::setOdometryTopic(const QString &topic)
{
    if (!path_.isEmpty() && !topic.isEmpty() && topic != odometryTopic()) {
        auto document = project_;
        if (document)
            document->setValue("odom_topic", topic);
        load(path_, {cloudTopic(), topic, mapMode_}, std::move(document), true);
    }
}

void ReplayController::setReplacing(bool replace)
{
    if (!path_.isEmpty() && replace != replacing()) {
        auto document = project_;
        if (document)
            document->setValue("cloud_mode", replace ? QStringLiteral("替换整图") : QStringLiteral("累积扫描"));
        load(path_, {cloudTopic(), odometryTopic(), replace ? replay::MapMode::Replace : replay::MapMode::Accumulate},
             std::move(document), true);
    }
}

void ReplayController::load(const QString &path, const replay::LoadOptions &options,
                            std::optional<replay::ProjectDocument> project, bool keepSession,
                            const QStringList &skipped)
{
    if (loading_)
        return;
    pause();
    status_ = tr("正在读取 ROS1 bag");
    loading_ = true;
    progress_ = 0.;
    cancelRequested_ = false;
    emit stateChanged();
    auto settings = options;
    if (project) {
        settings.topicLayout = project->topicLayout();
        settings.showMainCloud = project->json().value("composition").toObject().value("show_main_cloud").toBool(true);
    }
    watcher_.setFuture(QtConcurrent::run([this, path, options = std::move(settings), project = std::move(project),
                                          keepSession, skipped]() mutable {
        try {
            int lastPercent = -1;
            auto updateProgress = [this, &lastPercent](double value) {
                const int percent = int(value * 100);
                if (percent != lastPercent) {
                    lastPercent = percent;
                    QMetaObject::invokeMethod(this, [this, value] {
                        progress_ = value;
                        emit stateChanged();
                    }, Qt::QueuedConnection);
                }
            };
            auto record = replay::loadBag(path, options, [this] { return cancelRequested_.load(); }, updateProgress);
            return Result{std::move(record), path, {}, options.mapMode, std::move(project), keepSession, skipped};
        } catch (const std::exception &error) {
            return Result{nullptr, path, QString::fromUtf8(error.what()), {}, {}};
        }
    }));
}

void ReplayController::cancel()
{
    if (loading_) {
        cancelRequested_ = true;
        status_ = tr("正在取消…");
        emit stateChanged();
    }
}

void ReplayController::seek(double seconds)
{
    if (!std::isfinite(seconds))
        return;
    setPosition(seconds);
    if (playing()) {
        start_ = position_;
        clock_.restart();
    }
}

void ReplayController::setPosition(double seconds)
{
    seconds = std::clamp(seconds, 0., duration());
    if (seconds == position_)
        return;
    position_ = seconds;
    emit positionChanged();
}

void ReplayController::togglePlayback()
{
    if (playing()) {
        pause();
    } else if (!loading_ && duration() > 0.) {
        if (position_ >= duration())
            seek(0.);
        start_ = position_;
        clock_.start();
        timer_.start();
        emit playingChanged();
    }
}

void ReplayController::pause()
{
    if (playing()) {
        timer_.stop();
        emit playingChanged();
    }
}

QString ReplayController::speed() const
{
    const auto pose = record_->poseAt(record_->stampAt(position_));
    return pose && pose->speed ? QString::number(*pose->speed, 'f', 1) : QStringLiteral("—");
}

int ReplayController::pointCount() const
{
    const auto &frame = record_->frameAt(record_->stampAt(position_));
    return frame.end - (record_->replace ? frame.first : 0);
}

void ReplayController::refreshVideoLayers()
{
    clips_ = project_ ? project_->videos() : std::vector<replay::VideoClip>();
    QVariantList layers;
    for (const auto &clip : clips_) {
        const auto &timing = clip.timing;
        QVariantList anchors, crop, segments;
        const double mediaDuration = videoDurations_.value(clip.path);
        int part = 0;
        for (const auto &[first, last] : timing.spans()) {
            double end = last ? timing.replaySeconds(*last) : duration();
            if (mediaDuration > 0.)
                end = std::min(end, timing.replaySeconds(mediaDuration));
            const double start = timing.replaySeconds(first);
            const double visibleStart = std::max(0., start);
            const double visibleEnd = std::min(duration(), end);
            if (visibleEnd > visibleStart) {
                segments.append(QVariantMap{{"part", part}, {"first", visibleStart}, {"last", visibleEnd},
                    {"leftEdge", start >= 0.}, {"rightEdge", end <= duration()}});
            }
            ++part;
        }
        for (const auto &[replaySeconds, sourceSeconds] : timing.anchors)
            anchors.append(QVariant::fromValue(QVariantList{replaySeconds, sourceSeconds}));
        for (double value : timing.crop)
            crop.append(value);
        layers.append(QVariantMap{
            {"url", QUrl::fromLocalFile(clip.path)}, {"label", clip.label},
            {"width", clip.width}, {"opacity", clip.opacity},
            {"horizontal", clip.horizontal}, {"vertical", clip.vertical},
            {"offset", QString::number(timing.offset, 'g', 16)},
            {"trimIn", QString::number(timing.trimIn, 'g', 16)},
            {"trimOut", timing.trimOut ? QString::number(*timing.trimOut, 'g', 16) : QString()},
            {"rate", timing.rate()}, {"anchors", anchors}, {"crop", crop},
            {"segments", segments}
        });
    }
    videoLayers_ = std::move(layers);
    emit videoLayersChanged();
}

double ReplayController::videoSeconds(int index) const
{
    if (index < 0 || size_t(index) >= clips_.size())
        return -1.;
    const auto &timing = clips_[size_t(index)].timing;
    const double seconds = timing.sourceSeconds(position_);
    return seconds >= -1e-9 && timing.includes(seconds) ? std::max(0., seconds) : -1.;
}

void ReplayController::addVideo(const QUrl &url)
{
    if (loading_ || !url.isLocalFile())
        return;
    try {
        auto document = projectSnapshot();
        document.addVideo(url.toLocalFile());
        project_ = std::move(document);
        refreshVideoLayers();
        status_ = tr("已添加实拍视频。");
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::editVideo(int index, const QJsonObject &changes)
{
    if (loading_ || !project_)
        return;
    try {
        const auto before = project_->videoTiming(index);
        project_->setVideoTiming(index, changes);
        const auto after = project_->videoTiming(index);
        if (before == after)
            return;
        undo_.push_back({index, before, after});
        redo_.clear();
        refreshVideoLayers();
        emit editHistoryChanged();
        status_ = tr("视频设置已更新。");
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::setVideoTrim(int index, const QString &trimIn, const QString &trimOut,
                                    const QString &offset)
{
    bool inOk, outOk, offsetOk;
    const double in = trimIn.toDouble(&inOk);
    const double out = trimOut.toDouble(&outOk);
    const double shift = offset.toDouble(&offsetOk);
    const bool openEnd = trimOut.trimmed().isEmpty();
    if (!inOk || !std::isfinite(in) || !offsetOk || !std::isfinite(shift) ||
        (!openEnd && (!outOk || !std::isfinite(out)))) {
        status_ = tr("请输入有效的秒数，结束时间可留空。");
        emit stateChanged();
        return;
    }
    editVideo(index, {{"trim_in", in}, {"trim_out", openEnd ? QJsonValue(QJsonValue::Null) : QJsonValue(out)},
                      {"offset", shift}});
}

void ReplayController::setVideoAnchor(int index, int slot, const QString &seconds)
{
    if (index < 0 || size_t(index) >= clips_.size() || slot < 0 || slot > 1)
        return;
    bool ok;
    const auto source = seconds.toDouble(&ok);
    const auto &anchors = clips_[size_t(index)].timing.anchors;
    if (!ok || !std::isfinite(source) || source < 0. || (slot == 1 && anchors.empty())) {
        status_ = tr("请先设置锚点 1，并输入有效的视频时间。");
        emit stateChanged();
        return;
    }
    QJsonArray values;
    for (const auto &[replaySeconds, sourceSeconds] : anchors)
        values.append(QJsonArray{replaySeconds, sourceSeconds});
    const QJsonArray anchor{position_, source};
    if (slot < values.size())
        values.replace(slot, anchor);
    else
        values.append(anchor);
    editVideo(index, {{"anchors", values}});
}

void ReplayController::clearVideoAnchors(int index)
{
    editVideo(index, {{"anchors", QJsonArray{}}});
}

void ReplayController::setVideoDuration(int index, const QUrl &url, double seconds)
{
    if (index < 0 || size_t(index) >= clips_.size() || !std::isfinite(seconds) || seconds <= 0.)
        return;
    const auto path = url.toLocalFile();
    if (clips_[size_t(index)].path != path || videoDurations_.value(path) == seconds)
        return;
    videoDurations_.insert(path, seconds);
    // Media metadata may arrive while QML is creating the delegates.
    QMetaObject::invokeMethod(this, [this] { refreshVideoLayers(); }, Qt::QueuedConnection);
}

void ReplayController::storeVideoSpans(int index, const std::vector<replay::VideoTiming::Range> &ranges)
{
    editVideo(index, {{"trim_in", 0.}, {"trim_out", QJsonValue::Null},
                      {"segments", replay::VideoTiming::encodeSpans(ranges)}});
}

void ReplayController::splitVideo(int index, double at)
{
    changeVideoSegment(index, at, false);
}

void ReplayController::deleteVideoSegment(int index, double at)
{
    changeVideoSegment(index, at, true);
}

void ReplayController::changeVideoSegment(int index, double at, bool remove)
{
    if (loading_ || index < 0 || size_t(index) >= clips_.size() || !std::isfinite(at))
        return;

    constexpr double epsilon = 1e-8;
    const auto &clip = clips_[size_t(index)];
    const auto &timing = clip.timing;
    const double stamp = timing.sourceSeconds(at);
    const double mediaDuration = videoDurations_.value(clip.path);
    auto spans = timing.spans();
    if (mediaDuration <= 0. || stamp < mediaDuration - epsilon) {
        for (auto part = spans.begin(); part != spans.end(); ++part) {
            const auto [first, last] = *part;
            if (stamp < first - epsilon || (last && stamp >= *last - epsilon))
                continue;
            if (remove) {
                spans.erase(part);
            } else {
                if (stamp <= first + epsilon)
                    break;
                part->second = stamp;
                spans.insert(part + 1, {stamp, last});
            }
            storeVideoSpans(index, spans);
            return;
        }
    }
    status_ = tr("请把播放头移到所选视频片段内。");
    emit stateChanged();
}

void ReplayController::trimVideoSegment(int index, int segment, double first, double last)
{
    if (loading_ || index < 0 || size_t(index) >= clips_.size() ||
        segment < 0 || !std::isfinite(first) || !std::isfinite(last) || last <= first)
        return;
    const auto &clip = clips_[size_t(index)];
    const auto &timing = clip.timing;
    auto spans = timing.spans();
    const size_t part = size_t(segment);
    if (part >= spans.size())
        return;

    const auto [oldFirst, oldLast] = spans[part];
    const double start = timing.sourceSeconds(first);
    const double end = timing.sourceSeconds(last);
    const double mediaDuration = videoDurations_.value(clip.path);
    const double lower = part > 0 ? spans[part - 1].second.value() : 0.;
    std::optional<double> upper;
    if (part + 1 < spans.size())
        upper = spans[part + 1].first;
    if (mediaDuration > 0. && (!upper || mediaDuration < *upper))
        upper = mediaDuration;

    constexpr double epsilon = 1e-8;
    const double visibleFirst = std::max(0., timing.replaySeconds(oldFirst));
    double visibleLast = oldLast ? std::min(duration(), timing.replaySeconds(*oldLast)) : duration();
    if (mediaDuration > 0.)
        visibleLast = std::min(visibleLast, timing.replaySeconds(mediaDuration));
    // Keep the original endpoint when the view clips it or the source ends implicitly.
    const double nextFirst = std::abs(first - visibleFirst) < epsilon ? oldFirst : std::max(lower, start);
    const auto nextLast = std::abs(last - visibleLast) < epsilon ? oldLast
        : std::optional<double>(upper ? std::min(end, *upper) : end);
    if ((nextLast && *nextLast <= nextFirst + epsilon) ||
        (mediaDuration > 0. && nextFirst >= mediaDuration - epsilon))
        return;
    if (spans[part] != replay::VideoTiming::Range{nextFirst, nextLast}) {
        spans[part] = {nextFirst, nextLast};
        storeVideoSpans(index, spans);
    }
}

void ReplayController::undo()
{
    restoreVideoEdit(false);
}

void ReplayController::redo()
{
    restoreVideoEdit(true);
}

void ReplayController::restoreVideoEdit(bool redo)
{
    auto &history = redo ? redo_ : undo_;
    auto &destination = redo ? undo_ : redo_;
    if (loading_ || !project_ || history.empty())
        return;

    const auto edit = history.back();
    project_->replaceVideoTiming(edit.index, redo ? edit.after : edit.before);
    destination.push_back(edit);
    history.pop_back();
    refreshVideoLayers();
    status_ = redo ? tr("已重做视频编辑。") : tr("已撤销视频编辑。");
    emit editHistoryChanged();
    emit stateChanged();
}
