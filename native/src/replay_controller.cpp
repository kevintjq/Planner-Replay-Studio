#include "replay_controller.h"

#include "camera_presets.h"
#include "design_tokens.h"
#include "transform_tree.h"

#include <QtConcurrentRun>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QPointer>
#include <QQuickItemGrabResult>
#include <QSaveFile>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

QStringList ReplayController::tfFrames() const
{
    QStringList frames = record_->transforms ? record_->transforms->frames() : QStringList();
    for (const auto &frame : {record_->frameId, record_->bodyFrame})
        if (!frame.isEmpty() && !frames.contains(frame)) frames.append(frame);
    frames.sort();
    return frames;
}

ReplayController::ReplayController(QObject *parent) : QObject(parent), record_(replay::makeDemo())
{
    connect(this, &ReplayController::positionChanged, this, &ReplayController::activeLayoutChanged);
    connect(this, &ReplayController::layoutsChanged, this, &ReplayController::activeLayoutChanged);
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
    refreshDisplayVisibility();
    position_ = keepSession ? std::min(position_, duration()) : 0.;
    if (!keepSession) {
        const auto camera = project_ ? project_->cameraKind() : QString();
        const auto match = std::find_if(replay::CameraPresets.begin(), replay::CameraPresets.end(),
            [&](const auto &preset) { return camera == QString::fromLatin1(preset.id); });
        cameraIndex_ = match != replay::CameraPresets.end() ? int(match - replay::CameraPresets.begin()) : 0;
        emit cameraChanged();
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
        emit trajectorySettingsLoaded(project_ ? project_->trajectorySettings().toVariantMap() : QVariantMap{});
    }
    refreshSceneLayers();
    refreshCloudViews();
    refreshMotions();
    refreshVideoLayers();
    refreshLayoutCues();
    emit transparentCanvasChanged();
    emit sceneStyleChanged();
    emit markersChanged();
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
    const auto selected = layout.displays[size_t(index)];
    if (selected.enabled == enabled)
        return;
    // A single RViz TF node produces dynamic and static render layers. Keep its
    // checkbox consistent across both layers and across project/RViz exports.
    for (auto &display : layout.displays)
        if (display.id == selected.id || (!selected.sourcePath.isEmpty() && display.sourcePath == selected.sourcePath))
            display.enabled = enabled;
    auto document = projectSnapshot();
    document.setTopicLayout(layout);
    project_ = std::move(document);
    refreshDisplayVisibility();
    emit recordingChanged();
}

void ReplayController::refreshDisplayVisibility()
{
    displayVisibility_.clear();
    if (project_)
        for (const auto &display : project_->topicLayout().displays)
            displayVisibility_.insert(display.id, display.enabled);
}

void ReplayController::setFixedFrame(const QString &frame)
{
    if (loading_ || path_.isEmpty())
        return;
    auto normalized = frame.trimmed();
    while (normalized.startsWith('/'))
        normalized.remove(0, 1);
    auto layout = projectSnapshot().topicLayout();
    if (layout.fixedFrame != normalized) {
        layout.fixedFrame = normalized;
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

QString ReplayController::rvizCompatibility() const
{
    if (!project_) return {};
    const auto layout = project_->topicLayout();
    if (layout.sourceYaml.isEmpty()) return {};
    auto report = tr("原始分组、插件及属性完整保存在项目；本地渲染为支持子集。 / Original RViz configuration retained; native rendering is a subset.");
    if (!layout.skipped.isEmpty()) report += tr(" 未按原插件渲染：%1").arg(layout.skipped.join(QStringLiteral("、")));
    return report;
}

void ReplayController::exportRviz(const QUrl &url)
{
    if (loading_ || !url.isLocalFile()) return;
    try {
        if (QFileInfo(url.toLocalFile()).suffix().compare("rviz", Qt::CaseInsensitive) != 0 || referencesFile(url.toLocalFile()))
            throw std::runtime_error("请选择 .rviz 文件，不能覆盖项目素材。 / Invalid RViz destination.");
        auto layout = projectSnapshot().topicLayout();
        if (layout.sourceYaml.isEmpty() && layout.displays.empty())
            throw std::runtime_error("请先导入 RViz 配置。 / Import an RViz configuration first.");
        if (layout.fixedFrame.isEmpty()) layout.fixedFrame = worldFrame();
        const auto bytes = replay::serializeRviz(layout);
        QSaveFile file(url.toLocalFile());
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("无法写入 RViz 配置。 / Cannot write RViz configuration.");
        status_ = tr("已导出 RViz 配置；保留原插件属性。本地视角图层过滤等 UAV 专属设置不属于 RViz 配置。 / RViz configuration exported.");
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); }
    emit stateChanged();
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
    refreshDisplayVisibility();
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
    project.setCameraKind(QString::fromLatin1(replay::CameraPresets[size_t(cameraIndex_)].id));
    project.setValue("cloud_topic", cloudTopic());
    project.setValue("odom_topic", odometryTopic());
    project.setValue("cloud_mode", replacing() ? QStringLiteral("替换整图") : QStringLiteral("累积扫描"));
    return project;
}

void ReplayController::setTrajectorySettings(const QVariantMap &settings)
{
    if (loading_)
        return;
    try {
        auto document = projectSnapshot();
        document.setTrajectorySettings(QJsonObject::fromVariantMap(settings));
        project_ = std::move(document);
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
        emit stateChanged();
    }
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

QString ReplayController::speedSelection() const
{
    return project_ ? project_->values().value("speed_topic").toString() : QString();
}

void ReplayController::setSpeedTopic(const QString &topic)
{
    if (loading_ || path_.isEmpty())
        return;
    auto document = projectSnapshot();
    document.setValue("speed_topic", topic);
    load(path_, {cloudTopic(), odometryTopic(), mapMode_}, std::move(document), true);
}

void ReplayController::setPoseSource(bool tf, const QString &world, const QString &body)
{
    if (loading_ || path_.isEmpty())
        return;
    auto document = projectSnapshot();
    document.setValue("pose_source", tf ? QStringLiteral("TF") : QStringLiteral("Odometry"));
    document.setValue("world", world);
    document.setValue("body", body);
    load(path_, {cloudTopic(), odometryTopic(), mapMode_}, std::move(document), true);
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
        const auto values = project->values();
        settings.planTopic = values.value("plan_topic").toString();
        settings.corridorTopic = values.value("corridor_topic").toString();
        settings.goalTopic = values.value("goal_topic").toString();
        settings.cameraTopic = values.value("camera_info_topic").toString(values.value("camera_topic").toString());
        const bool legacyScan = project->json().value("enabled").toObject().value("scan").toBool(false);
        settings.scanTopic = values.value("scan_topic").toString();
        if (settings.scanTopic == QStringLiteral("自动识别"))
            settings.scanTopic.clear();
        settings.autoScan = legacyScan && settings.scanTopic.isEmpty();
        bool validScanAge = false;
        const double scanSeconds = values.value("scan_max_age").toString().toDouble(&validScanAge);
        if (validScanAge)
            settings.scanMaximumAge = scanSeconds;
        settings.speedTopic = values.value("speed_topic").toString();
        settings.tfPose = values.value("pose_source").toString().compare("TF", Qt::CaseInsensitive) == 0;
        settings.world = values.value("world").toString();
        settings.body = values.value("body").toString();
        settings.extrinsics = project->extrinsics();
        bool validGap = false;
        const double gap = values.value("max_gap").toString().toDouble(&validGap);
        if (validGap)
            settings.maximumGap = gap;
        settings.planFrame = values.value("plan_frame").toString().trimmed();
        while (settings.planFrame.startsWith('/'))
            settings.planFrame.remove(0, 1);
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
    const auto value = record_->speedAt(record_->stampAt(position_));
    return value ? QString::number(*value, 'f', 1) : QStringLiteral("—");
}

QVariantMap ReplayController::speedGauge() const
{
    return project_ ? project_->speedGauge() : QVariantMap{{"visible", false}, {"limit", 10.},
        {"scale", 1.}, {"x", .05}, {"y", .32}, {"start", 0.}, {"end", -1.}, {"transparent", true}};
}

void ReplayController::setSpeedGauge(const QVariantMap &settings)
{
    if (loading_)
        return;
    try {
        auto project = projectSnapshot();
        project.setSpeedGauge(settings);
        project_ = std::move(project);
        emit recordingChanged();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
        emit stateChanged();
    }
}

QVariantMap ReplayController::moviePreferences() const
{
    auto prefs = project_ ? project_->moviePreferences() : QVariantMap{{"speed", 1.}, {"audio", QString()}};
    const auto id = prefs.value("audio").toString();
    const auto layer = std::find_if(clips_.begin(), clips_.end(), [&](const auto &clip) {
        return !id.isEmpty() && clip.id == id;
    });
    prefs.insert("audioIndex", layer == clips_.end() ? -1 : int(layer - clips_.begin()));
    return prefs;
}

void ReplayController::setMoviePreferences(double speed, int audioIndex)
{
    if (loading_ || (speed != 1. && speed != .5 && speed != .25) ||
        audioIndex < -1 || (audioIndex >= 0 && size_t(audioIndex) >= clips_.size()))
        return;
    auto project = projectSnapshot();
    project.setMoviePreferences(speed, audioIndex < 0 ? QString() : clips_[size_t(audioIndex)].id);
    project_ = std::move(project);
    emit recordingChanged();
}

int ReplayController::pointCount() const
{
    const auto &frame = record_->frameAt(record_->stampAt(position_));
    return frame.end - (record_->replace ? frame.first : 0);
}

double ReplayController::duration() const
{
    const auto recorded = record_->duration();
    if (recorded > 0. || !project_)
        return recorded;

    double end = 6.;
    for (const auto &clip : clips_) {
        const auto media = videoDurations_.value(clip.path);
        if (media > 0.)
            end = std::max(end, clip.timing.replaySeconds(media));
    }
    for (const auto &value : sceneLayers_)
        end = std::max(end, value.toMap().value("end").toDouble());
    return end;
}

QStringList ReplayController::cameraLabels() const
{
    QStringList labels;
    for (const auto &preset : replay::CameraPresets)
        labels.append(QString::fromUtf8(preset.label));
    return labels;
}

void ReplayController::setCamera(int index)
{
    if (loading_ || index < 0 || size_t(index) >= replay::CameraPresets.size() || cameraIndex_ == index)
        return;

    auto project = projectSnapshot();
    project.setCameraKind(QString::fromLatin1(replay::CameraPresets[size_t(index)].id));
    project_ = std::move(project);
    cameraIndex_ = index;
    emit cameraChanged();
}

QColor ReplayController::sceneBackground() const
{
    return QColor(project_ ? project_->sceneBackground() : QString::fromLatin1(DesignTokens::SceneBackground));
}

void ReplayController::setSceneBackground(const QColor &color)
{
    if (!color.isValid() || color.alpha() != 255 || color == sceneBackground()) return;
    auto document = projectSnapshot();
    document.setSceneBackground(color.name());
    project_ = std::move(document);
    emit sceneStyleChanged();
}

void ReplayController::setTransparentCanvas(bool transparent)
{
    if (loading_ || transparentCanvas() == transparent)
        return;
    auto document = projectSnapshot();
    document.setTransparentCanvas(transparent);
    project_ = std::move(document);
    emit transparentCanvasChanged();
}

bool ReplayController::referencesFile(const QString &path) const
{
    const QFileInfo output(path);
    if (!output.exists())
        return false;
    const auto canonical = output.canonicalFilePath();
    const auto document = projectSnapshot();
    if (canonical == QFileInfo(document.path()).canonicalFilePath())
        return true;
    const auto media = document.media();
    return std::any_of(media.begin(), media.end(), [&](const auto &asset) {
        return QFileInfo(asset).canonicalFilePath() == canonical;
    });
}

void ReplayController::exportFrame(const QUrl &url, QQuickItem *canvas)
{
    if (loading_ || capturing_ || !canvas || !url.isLocalFile())
        return;
    const auto path = url.toLocalFile();
    const auto document = projectSnapshot();
    const QFileInfo output(path);
    if (output.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0 ||
        (output.exists() && output.canonicalFilePath() == QFileInfo(document.path()).canonicalFilePath())) {
        status_ = tr("请选择有效的 PNG 文件路径。");
        emit stateChanged();
        return;
    }
    if (output.exists()) {
        const auto media = document.media();
        if (std::any_of(media.begin(), media.end(), [&](const auto &asset) {
                return QFileInfo(asset).canonicalFilePath() == output.canonicalFilePath();
            })) {
            status_ = tr("PNG 输出不能覆盖项目引用的素材。");
            emit stateChanged();
            return;
        }
    }

    pause();
    capturing_ = true;
    status_ = tr("正在导出当前画面…");
    grabFrame(canvas, path);
}

namespace {
QImage unmatte(const QImage &black, const QImage &white)
{
    if (black.size() != white.size() || black.isNull())
        return {};

    const auto dark = black.convertToFormat(QImage::Format_RGB32);
    const auto light = white.convertToFormat(QImage::Format_RGB32);
    QImage result(black.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
        return {};

    for (int y = 0; y < result.height(); ++y) {
        const auto *below = reinterpret_cast<const QRgb *>(dark.constScanLine(y));
        const auto *above = reinterpret_cast<const QRgb *>(light.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const int red = qRed(below[x]), green = qGreen(below[x]), blue = qBlue(below[x]);
            const int difference = qRed(above[x]) - red + qGreen(above[x]) - green + qBlue(above[x]) - blue;
            const int alpha = 255 - std::clamp((difference + 1) / 3, 0, 255);
            out[x] = qRgba(std::min(red, alpha), std::min(green, alpha), std::min(blue, alpha), alpha);
        }
    }
    return result;
}
}

void ReplayController::grabFrame(QQuickItem *canvas, const QString &path, int matte, const QImage &black)
{
    captureMatte_ = matte;
    emit stateChanged();
    frameGrab_ = canvas ? canvas->grabToImage() : nullptr;
    if (!frameGrab_) {
        finishFrame(path, {});
        return;
    }
    connect(frameGrab_.data(), &QQuickItemGrabResult::ready, this, [this, path, canvas = QPointer(canvas), matte, black] {
        const auto grab = std::move(frameGrab_);
        const auto image = grab->image();
        if (matte == 0 && transparentCanvas() && !image.isNull() && !image.hasAlphaChannel())
            grabFrame(canvas, path, 1);
        else if (matte == 1 && !image.isNull())
            grabFrame(canvas, path, 2, image);
        else
            finishFrame(path, matte == 2 ? unmatte(black, image) : image);
    }, Qt::SingleShotConnection);
}

void ReplayController::finishFrame(const QString &path, const QImage &image)
{
    QSaveFile output(path);
    const bool success = !image.isNull() && output.open(QIODevice::WriteOnly) && image.save(&output, "PNG") && output.commit();
    if (success)
        status_ = tr("已导出 PNG：%1").arg(QDir::toNativeSeparators(path));
    else
        status_ = tr("无法导出 PNG：%1").arg(QDir::toNativeSeparators(path));
    captureMatte_ = 0;
    capturing_ = false;
    emit stateChanged();
    emit frameExported(success);
}

void ReplayController::refreshLayoutCues()
{
    layouts_ = project_ ? project_->layoutCues() : QJsonArray();
    emit layoutsChanged();
}

QVariantMap ReplayController::activeLayout() const
{
    const auto bound = std::upper_bound(layouts_.constBegin(), layouts_.constEnd(), position_,
        [](double time, const auto &cue) { return time < cue.toObject().value("time").toDouble(); });
    const auto current = bound != layouts_.constBegin() ? std::prev(bound)->toObject() : QJsonObject();
    QVariantMap value = current.toVariantMap();
    const auto mode = current.value("mode").toString("mix");
    value.insert("mode", mode);
    const auto videoId = current.value("video_id").toString();
    if (mode == "video") {
        const auto selected = std::find_if(clips_.begin(), clips_.end(), [&](const auto &clip) { return clip.id == videoId; });
        if (selected != clips_.end()) {
            const double seconds = videoSeconds(int(std::distance(clips_.begin(), selected)));
            const double media = videoDurations_.value(selected->path);
            value.insert("fullHasMedia", seconds >= 0. && (media <= 0. || seconds < media));
        }
    }
    const auto backdropId = [this](const QJsonObject &cue) -> QString {
        const auto mode = cue.value("mode").toString();
        if (mode == "backdrop")
            return cue.value("video_id").toString();
        const auto excludedId = mode == "inset" ? cue.value("video_id").toString() : QString();
        for (const auto &clip : clips_) {
            if (clip.backdrop && clip.id != excludedId)
                return clip.id;
        }
        return {};
    };
    value.insert("backdropId", backdropId(current));
    value.insert("progress", 1.);
    if (mode != "inset")
        return value;

    const double transition = current.value("transition").toDouble(.4);
    const double elapsed = position_ - current.value("time").toDouble();
    if (transition <= 0. || elapsed >= transition)
        return value;
    const auto previous = std::distance(layouts_.constBegin(), bound) < 2
        ? QJsonObject() : std::prev(bound, 2)->toObject();
    const auto previousMode = previous.value("mode").toString();
    if (previousMode == "video" || previousMode == "cloud" || backdropId(previous) != videoId)
        return value;

    const double progress = std::clamp(elapsed / transition, 0., 1.);
    value.insert("progress", progress * progress * (3. - 2. * progress));
    return value;
}

void ReplayController::setLayoutCue(const QString &mode, int videoIndex, double transition)
{
    if (loading_ || duration() <= 0.)
        return;
    pause();
    try {
        const QString videoId = videoIndex >= 0 && size_t(videoIndex) < clips_.size()
            ? clips_[size_t(videoIndex)].id : QString();
        auto document = projectSnapshot();
        document.setLayoutCue(position_, mode, videoId, transition);
        project_ = std::move(document);
        refreshLayoutCues();
        status_ = tr("布局标记已更新。");
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::removeLayoutCue(double time)
{
    if (loading_ || !project_)
        return;
    project_->removeLayoutCue(time);
    refreshLayoutCues();
    status_ = tr("已删除布局标记。");
    emit stateChanged();
}

void ReplayController::setScanOptions(const QString &topic, double maximumAge)
{
    if (loading_ || path_.isEmpty() || (!topic.isEmpty() && !scanTopics().contains(topic)) ||
        !std::isfinite(maximumAge) || maximumAge <= 0. || maximumAge > 60.)
        return;
    auto document = projectSnapshot();
    document.setValue("scan_topic", topic);
    document.setValue("scan_max_age", QString::number(maximumAge));
    document.setLayerEnabled("scan", !topic.isEmpty());
    load(path_, {cloudTopic(), odometryTopic(), mapMode_}, std::move(document), true);
}

bool ReplayController::planningLayerEnabled(const QString &role) const
{
    if (role == "scan" && record_->scanTopic.isEmpty())
        return false;
    return !project_ || project_->isLayerEnabled(role);
}

QVariantMap ReplayController::planningLayers() const
{
    QVariantMap layers;
    if (!record_->scanTopic.isEmpty())
        layers.insert("scan", QVariantMap{{"topic", record_->scanTopic}, {"enabled", planningLayerEnabled("scan")}});
    for (const auto &overlay : record_->topicOverlays) {
        const auto &display = overlay.display;
        if (!display.plannerRole.isEmpty() && !overlay.samples.empty())
            layers.insert(display.plannerRole, QVariantMap{{"topic", display.topic},
                          {"enabled", planningLayerEnabled(display.plannerRole)}});
    }
    return layers;
}

void ReplayController::setPlanningLayerEnabled(const QString &role, bool enabled)
{
    if (loading_ || (role != "plan" && role != "corridor" && role != "goal" && role != "camera" && role != "scan"))
        return;
    auto document = projectSnapshot();
    document.setLayerEnabled(role, enabled);
    project_ = std::move(document);
    emit recordingChanged();
}

void ReplayController::setComparePlans(bool compare)
{
    if (loading_ || compare == comparePlans())
        return;
    auto document = projectSnapshot();
    document.setComparePlans(compare);
    project_ = std::move(document);
    emit recordingChanged();
}

QVariantList ReplayController::alignmentCandidates() const
{
    if (record_->poses.size() < 5 || duration() < .8)
        return {};
    struct Candidate { double score, time; QString label; };
    std::vector<Candidate> candidates;
    const double step = std::max(.15, duration() / 1200.);
    for (double t = step; t + step < duration(); t += step) {
        const auto a = record_->poseAt(record_->stampAt(t - step));
        const auto b = record_->poseAt(record_->stampAt(t));
        const auto c = record_->poseAt(record_->stampAt(t + step));
        if (!a || !b || !c)
            continue;
        const auto forward = [](const QQuaternion &rotation) {
            const auto v = rotation.rotatedVector({1, 0, 0});
            return std::atan2(v.y(), v.x());
        };
        const double yaw = std::abs(std::remainder(forward(c->rotation) - forward(a->rotation), 6.283185307179586)) / (2 * step);
        const double dv = ((c->position - b->position).length() - (b->position - a->position).length()) / step;
        if (yaw >= .25 && yaw / .55 >= std::abs(dv) / .65)
            candidates.push_back({yaw / .55, t, tr("转弯")});
        else if (dv < -.20)
            candidates.push_back({-dv / .4, t, tr("减速／急停")});
        else if (c->position.z() - a->position.z() > .18 && dv > 0)
            candidates.push_back({dv / .4 + .5, t, tr("上升／起飞")});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](auto &a, auto &b) { return a.score > b.score; });
    std::vector<Candidate> selected;
    for (const auto &entry : candidates) {
        if (std::none_of(selected.begin(), selected.end(), [&](auto &prior) { return std::abs(prior.time - entry.time) < 1.; }))
            selected.push_back(entry);
        if (selected.size() == 8)
            break;
    }
    std::sort(selected.begin(), selected.end(), [](auto &a, auto &b) { return a.time < b.time; });
    QVariantList result;
    for (const auto &entry : selected)
        result.append(QVariantMap{{"time", entry.time}, {"label", QString::number(entry.time, 'f', 2) + " s · " + entry.label}});
    return result;
}

QVariantList ReplayController::planningEvents() const
{
    QVariantList events;
    for (const auto &[stamp, name] : record_->planningEvents) {
        const double time = double(stamp - record_->frames.front().stamp) / 1.e9;
        if (time >= 0. && time <= duration())
            events.append(QVariantMap{{"time", time}, {"label", QString::number(time, 'f', 3) + " s · " + name}});
    }
    return events;
}

QVariantList ReplayController::markers() const
{
    return project_ ? project_->timelineMarkers().toVariantList() : QVariantList();
}

void ReplayController::addMarker()
{
    if (loading_ || duration() <= 0.)
        return;
    pause();
    try {
        auto document = projectSnapshot();
        document.addTimelineMarker(position_);
        project_ = std::move(document);
        status_ = tr("已在 %1 秒添加标记。").arg(position_, 0, 'f', 3);
        emit markersChanged();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::renameMarker(double time, const QString &label)
{
    changeMarker(time, label);
}

void ReplayController::removeMarker(double time)
{
    changeMarker(time, std::nullopt);
}

void ReplayController::changeMarker(double time, const std::optional<QString> &label)
{
    if (loading_ || !project_)
        return;

    try {
        project_->replaceTimelineMarker(time, label);
        status_ = label ? tr("已重命名标记。") : tr("已删除标记。");
        emit markersChanged();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

QVariantMap ReplayController::mainView() const
{
    return project_ ? project_->mainView() : QVariantMap{{"width", 1.}, {"zoom", 1.}, {"x", .5}, {"y", .5}};
}

void ReplayController::refreshMotions()
{
    motions_ = project_ ? project_->layerMotions() : QJsonArray();
    emit motionsChanged();
}

QVariantList ReplayController::layerMotions() const
{
    return motions_.toVariantList();
}

QRectF ReplayController::animatedLayer(const QString &kind, const QString &id, double time,
                                      const QRectF &base, const QSizeF &canvas) const
{
    if (!base.isValid() || !canvas.isValid())
        return base;
    for (const auto &value : motions_) {
        const auto motion = value.toObject();
        const double start = motion.value("start").toDouble();
        const double cue = motion.value("time").toDouble();
        const double duration = motion.value("duration").toDouble(.4);
        if (motion.value("kind").toString() != kind || motion.value("layer_id").toString() != id ||
            time < start || time >= cue + duration || duration <= 0.)
            continue;
        const double scale = std::min(canvas.width() * motion.value("large_width").toDouble(1.) / base.width(),
                                      canvas.height() / base.height());
        if (scale <= 1.)
            break;
        const double fraction = std::clamp((time - cue) / duration, 0., 1.);
        const double ease = fraction * fraction * (3. - 2. * fraction);
        const auto position = motion.value("large_position").toArray();
        const double largeWidth = base.width() * scale, largeHeight = base.height() * scale;
        const double x = (canvas.width() - largeWidth) * (position.size() == 2 ? position[0].toDouble(.5) : .5);
        const double y = (canvas.height() - largeHeight) * (position.size() == 2 ? position[1].toDouble(.5) : .5);
        return {std::lerp(x, base.x(), ease), std::lerp(y, base.y(), ease),
                std::lerp(largeWidth, base.width(), ease), std::lerp(largeHeight, base.height(), ease)};
    }
    return base;
}

void ReplayController::addLayerMotion(const QString &kind, const QString &id, double begin, double duration,
                                      double width, double x, double y, double normalWidth)
{
    if (loading_)
        return;
    try {
        auto document = projectSnapshot();
        document.addLayerMotion(kind, id, begin, position_, duration, width, x, y);
        if (kind == "main")
            document.setMainViewWidth(normalWidth);
        project_ = std::move(document);
        refreshMotions();
        emit recordingChanged();
        status_ = tr("已添加缩窗动画。");
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::removeLayerMotion(int index)
{
    if (loading_ || !project_)
        return;
    project_->removeLayerMotion(index);
    refreshMotions();
}

void ReplayController::refreshCloudViews()
{
    cloudViews_.clear();
    if (project_) {
        for (const auto &view : project_->cloudViews()) {
            auto info = view.toMap();
            const auto kind = info.value("kind").toString();
            const auto camera = std::find_if(replay::CameraPresets.begin(), replay::CameraPresets.end(),
                [&](const auto &preset) { return QString::fromLatin1(preset.id) == kind; });
            if (camera == replay::CameraPresets.end())
                continue;
            info.insert("camera", int(camera - replay::CameraPresets.begin()));
            info.insert("label", QString::fromUtf8(camera->label));
            cloudViews_.append(std::move(info));
        }
    }
    emit cloudViewsChanged();
}

void ReplayController::addCloudView(int camera)
{
    if (loading_ || camera <= 0 || size_t(camera) >= replay::CameraPresets.size())
        return;
    try {
        auto document = projectSnapshot();
        document.addCloudView(QString::fromLatin1(replay::CameraPresets[size_t(camera)].id));
        project_ = std::move(document);
        refreshCloudViews();
        status_ = tr("已添加点云机位。");
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::setCloudView(const QString &id, const QVariantMap &settings)
{
    if (loading_ || !project_)
        return;
    try {
        project_->setCloudView(id, settings);
        refreshCloudViews();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
        emit stateChanged();
    }
}

void ReplayController::removeCloudView(const QString &id)
{
    if (loading_ || !project_)
        return;
    project_->removeCloudView(id);
    refreshCloudViews();
    refreshMotions();
}

void ReplayController::refreshSceneLayers()
{
    sceneLayers_ = project_ ? project_->sceneLayers() : QVariantList();
    emit sceneLayersChanged();
}

void ReplayController::addSceneLayer(const QString &kind, const QString &content)
{
    if (loading_)
        return;
    try {
        auto document = projectSnapshot();
        const double end = duration() > 0. ? std::min(duration(), position_ + 3.) : position_ + 3.;
        const double start = end <= position_ ? std::max(0., end - 3.) : position_;
        document.addSceneLayer(kind, content, start, end);
        project_ = std::move(document);
        refreshSceneLayers();
        emit recordingChanged();
        status_ = tr("已添加合成图层。");
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::addSceneImage(const QUrl &file)
{
    if (file.isLocalFile())
        addSceneLayer(QStringLiteral("images"), file.toLocalFile());
}

void ReplayController::addSceneText(const QString &text)
{
    addSceneLayer(QStringLiteral("texts"), text);
}

void ReplayController::editSceneLayer(const QString &id, const QVariantMap &changes)
{
    if (loading_ || !project_)
        return;
    try {
        project_->editSceneLayer(id, QJsonObject::fromVariantMap(changes));
        refreshSceneLayers();
        setPosition(position_);
        emit recordingChanged();
        status_ = tr("合成图层已更新。");
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::removeSceneLayer(const QString &id)
{
    if (loading_ || !project_)
        return;
    project_->removeSceneLayer(id);
    refreshSceneLayers();
    refreshMotions();
    setPosition(position_);
    emit recordingChanged();
    status_ = tr("已移除合成图层。");
    emit stateChanged();
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
            {"url", QUrl::fromLocalFile(clip.path)}, {"id", clip.id}, {"label", clip.label},
            {"width", clip.width}, {"opacity", clip.opacity},
            {"horizontal", clip.horizontal}, {"vertical", clip.vertical},
            {"backdropScale", clip.backdropScale}, {"backdropX", clip.backdropX}, {"backdropY", clip.backdropY},
            {"offset", QString::number(timing.offset, 'g', 16)},
            {"trimIn", QString::number(timing.trimIn, 'g', 16)},
            {"trimOut", timing.trimOut ? QString::number(*timing.trimOut, 'g', 16) : QString()},
            {"rate", timing.rate()}, {"anchors", anchors}, {"crop", crop},
            {"segments", segments}
        });
    }
    videoLayers_ = std::move(layers);
    emit videoLayersChanged();
    emit activeLayoutChanged();
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
    QMetaObject::invokeMethod(this, [this] {
        refreshVideoLayers();
        emit recordingChanged();
    }, Qt::QueuedConnection);
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
