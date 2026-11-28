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
#include <QSettings>
#include <QUuid>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace {
bool independentDataKind(const QString &kind)
{
    return QStringList{"cloud", "laser", "path", "marker", "position", "tf"}.contains(kind);
}
QStringList independentMessageTypes(const QString &pluginClass)
{
    static const QHash<QString, QStringList> types{{"PointCloud2", {"sensor_msgs/PointCloud2"}},
        {"LaserScan", {"sensor_msgs/LaserScan"}}, {"Path", {"nav_msgs/Path"}},
        {"Marker", {"visualization_msgs/Marker"}}, {"MarkerArray", {"visualization_msgs/MarkerArray"}},
        {"Odometry", {"nav_msgs/Odometry"}}, {"Pose", {"geometry_msgs/PoseStamped"}},
        {"PointStamped", {"geometry_msgs/PointStamped"}},
        {"PoseWithCovariance", {"geometry_msgs/PoseWithCovarianceStamped"}},
        {"TF", {"tf2_msgs/TFMessage", "tf/tfMessage"}}};
    return types.value(pluginClass.section('/', -1));
}
const replay::TopicCatalogEntry *independentTopicEntry(const replay::Recording &record, const QString &topic)
{
    const auto found = std::find_if(record.topicCatalog.begin(), record.topicCatalog.end(), [&](const auto &item) { return item.name == topic; });
    return found == record.topicCatalog.end() ? nullptr : &*found;
}
bool independentCompatible(const replay::TopicCatalogEntry *entry, const replay::TopicDisplay &display)
{
    return entry && !entry->schemaConflict && independentMessageTypes(display.pluginClass).contains(entry->type);
}
const replay::TopicCatalogEntry *independentTopicEntry(const replay::Recording &record, const replay::TopicDisplay &display)
{
    const auto *entry = independentTopicEntry(record, display.topic);
    if (display.kind != "tf" || independentCompatible(entry, display)) return entry;
    const auto *staticEntry = independentTopicEntry(record, QStringLiteral("/tf_static"));
    return independentCompatible(staticEntry, display) || !entry ? staticEntry : entry;
}
std::shared_ptr<const replay::Recording> emptyRecording()
{
    auto result = std::make_shared<replay::Recording>();
    result->name = QStringLiteral("No Data · 未载入数据"); result->radius = 1.f;
    return result;
}
}

QStringList ReplayController::tfFrames() const
{
    QStringList frames = record_->transforms ? record_->transforms->frames() : QStringList();
    for (const auto &frame : {record_->frameId, record_->bodyFrame})
        if (!frame.isEmpty() && !frames.contains(frame)) frames.append(frame);
    frames.sort();
    return frames;
}

QVariantMap ReplayController::nativeReplaySource() const
{
    if (demo() || loading_ || path_.isEmpty() || (!record_->hasMessages && record_->frames.empty())) return {};
    // A source inventory is not a list of fixed rendering roles. Subscriptions
    // and their caches belong to each native Display, not these legacy presets.
    return {{"bag", QUrl::fromLocalFile(path_)}, {"topics", record_->topics},
            {"timeBasis", record_->timelineTimeBasis},
            {"revision", QString::number(recordingGeneration_)}};
}

QString ReplayController::experimentStampNs() const
{
    // Keep the absolute Header clock as an integer string across the QML boundary.
    // A JavaScript Number cannot preserve nanoseconds at ROS epoch magnitudes.
    return demo() || loading_ || (!record_->hasMessages && record_->frames.empty()) ? QString() : QString::number(record_->stampAt(position_));
}

QVariantMap ReplayController::nativeRvizConfig() const
{
    if (!project_) return {};
    auto result = project_->composition().value("native_rviz").toObject().toVariantMap();
    if (result.value("yaml").toString().isEmpty()) {
        const auto original = project_->topicLayout().sourceYaml;
        if (!original.isEmpty()) result["yaml"] = original;
    }
    return result;
}

void ReplayController::setNativeRvizConfig(const QString &yaml, const QVariantMap &metadata)
{
    if (loading_ || yaml.isEmpty()) return;
    auto document = projectSnapshot();
    auto composition = document.composition();
    composition["native_rviz"] = QJsonObject{{"yaml", yaml}, {"metadata", QJsonObject::fromVariantMap(metadata)}};
    document.setComposition(composition);
    project_ = std::move(document);
    // Native Property edits have their own exact host snapshots. Persisting a
    // snapshot must not reload the Bag or add a second unrelated undo command.
    emit nativeConfigChanged();
}

QString ReplayController::renderBackend() const
{
    return project_ ? project_->renderBackend() : QStringLiteral("independent");
}

void ReplayController::setRenderBackend(const QString &backend)
{
    if (loading_ || backend == renderBackend() ||
        (backend != "independent" && backend != "native-rviz")) return;
    const auto before = projectSnapshot();
    auto document = before;
    document.setRenderBackend(backend);
    project_ = std::move(document);
    recordProjectEdit(before);
    // This is the user's requested backend, independent of runtime availability.
    // Changing it must not reload data or pause the shared experiment clock.
    emit renderBackendChanged();
}

int VideoLayerListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(layers_.size());
}

QVariant VideoLayerListModel::data(const QModelIndex &index, int role) const
{
    return role == VideoLayer && index.isValid() && index.row() >= 0 && index.row() < layers_.size()
        ? layers_[index.row()] : QVariant{};
}

void VideoLayerListModel::setLayers(const QVariantList &layers)
{
    const auto id = [](const QVariant &value) { return value.toMap().value("id").toString(); };
    QSet<QString> wanted;
    for (const auto &row : layers) {
        const auto key = id(row);
        if (key.isEmpty() || wanted.contains(key)) return;
        wanted.insert(key);
    }
    for (int row = int(layers_.size()) - 1; row >= 0; --row) if (!wanted.contains(id(layers_[row]))) {
        beginRemoveRows({}, row, row); layers_.removeAt(row); endRemoveRows();
    }
    for (int row = 0; row < layers.size(); ++row) {
        const auto key = id(layers[row]);
        int existing = row;
        while (existing < layers_.size() && id(layers_[existing]) != key) ++existing;
        if (existing == layers_.size()) {
            beginInsertRows({}, row, row); layers_.insert(row, layers[row]); endInsertRows();
        } else if (existing != row) {
            beginMoveRows({}, existing, existing, {}, row); layers_.move(existing, row); endMoveRows();
        }
        if (layers_[row] != layers[row]) {
            layers_[row] = layers[row]; emit dataChanged(index(row), index(row), {VideoLayer});
        }
    }
}

ReplayController::ReplayController(QObject *parent) : QObject(parent), record_(emptyRecording()), videoLayerModel_(this)
{
    canvas_ = new CanvasLayerModel(*this);
    connect(this, &ReplayController::videoLayersChanged, this, &ReplayController::sourcesChanged);
    connect(this, &ReplayController::sceneLayersChanged, this, &ReplayController::sourcesChanged);
    connect(this, &ReplayController::recordingChanged, this, &ReplayController::sourcesChanged);
    connect(this, &ReplayController::positionChanged, this, &ReplayController::activeLayoutChanged);
    connect(this, &ReplayController::layoutsChanged, this, &ReplayController::activeLayoutChanged);
    connect(this, &ReplayController::recordingChanged, this, &ReplayController::refreshIndependentDisplayData);
    // Seeking and frame-locked TF can change availability. Coalesce rapid seeks
    // to 10 Hz and only notify the tree when its actual status changes.
    independentStatusTimer_.setSingleShot(true);
    independentStatusTimer_.setInterval(100);
    connect(this, &ReplayController::positionChanged, this, [this] {
        if (!path_.isEmpty() && !independentStatusTimer_.isActive()) independentStatusTimer_.start();
    });
    connect(&independentStatusTimer_, &QTimer::timeout, this, [this] {
        QVariantList statuses;
        for (const auto &value : independentDisplays()) {
            const auto row = value.toMap();
            statuses.append(QVariantMap{{"id", row.value("id")}, {"status", row.value("status")}, {"text", row.value("statusText")}});
        }
        if (statuses != independentTimeStatuses_) { independentTimeStatuses_ = std::move(statuses); emit independentDisplaysChanged(); }
    });
    connect(&independentWatcher_, &QFutureWatcher<IndependentDisplayLoad>::finished, this, [this] {
        const auto result = independentWatcher_.future().takeResult();
        if (result.generation == independentDataGeneration_ && result.source == independentDataSource_ && result.fixedFrame == independentDataFrame_) {
            for (const auto &key : result.keys) {
                independentPending_.remove(key);
                if (result.records.contains(key)) independentData_[key] = result.records.value(key);
                else independentErrors_[key] = result.error.isEmpty() ? tr("没有可解码数据。 / No decoded data.") : result.error;
            }
        }
        emit independentDisplaysChanged();
        refreshIndependentDisplayData();
    });
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] {
        setPosition(start_ + clock_.nsecsElapsed() / 1.e9 * moviePreferences().value("speed").toDouble());
        if (position_ >= duration())
            pause();
    });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        auto result = watcher_.result();
        loading_ = false;
        if (!cancelRequested_ && result.record) {
            const auto previousRecord = record_;
            const auto previousPath = path_;
            path_ = result.path;
            demoMode_ = false;
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
            if (replacementBefore_) {
                recordProjectEdit(*replacementBefore_, previousRecord, previousPath);
                for (const auto &entry : replacementRemovedSources_) {
                    const auto source = entry.toMap();
                    emit sourceRemoved(source.value("kind").toString(), source.value("url").toUrl());
                }
            }
        } else {
            status_ = cancelRequested_ ? tr("已取消读取") : result.issue;
            emit recordingChanged();
        }
        replacementBefore_.reset();
        replacementRemovedSources_.clear();
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
    if (independentCancel_) *independentCancel_ = true;
    independentWatcher_.waitForFinished();
    watcher_.waitForFinished();
    folderWatcher_.waitForFinished();
}

void ReplayController::install(std::shared_ptr<const replay::Recording> record, bool keepSession)
{
    pause();
    record_ = std::move(record);
    ensureInitialDisplayLayout();
    ++recordingGeneration_;
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
        emit nativeConfigChanged();
        emit nativeConfigLoaded(nativeRvizConfig());
    }
    refreshSceneLayers();
    refreshCloudViews();
    refreshMotions();
    refreshVideoLayers();
    refreshLayoutCues();
    emit transparentCanvasChanged();
    emit sceneStyleChanged();
    emit markersChanged();
    emit renderBackendChanged();
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
        result.append(QVariantMap{{"id", display.id}, {"label", label}, {"enabled", display.enabled}, {"color", display.color}});
    }
    return result;
}

replay::RvizLayout ReplayController::independentLayout() const
{
    auto layout = project_ ? project_->topicLayout() : replay::RvizLayout{};
    if (layout.useInstanceModel || !layout.instances.empty() || !layout.displays.empty() || !layout.sourceYaml.isEmpty()) {
        if (layout.instances.empty() && !layout.useInstanceModel) layout.instances = replay::rvizDisplayInstances(layout);
        return layout;
    }
    replay::TopicDisplay grid; grid.id = "initial-grid"; grid.kind = "grid"; grid.pluginClass = "rviz/Grid";
    grid.name = tr("Grid · 网格"); grid.color = QColor("#75808c");
    grid.properties = {{"Cell Size", 1.}, {"Plane Cell Count", 20.}};
    replay::TopicDisplay axes; axes.id = "initial-axes"; axes.kind = "axes"; axes.pluginClass = "rviz/Axes";
    axes.name = tr("Axes · 坐标轴"); axes.color = QColor("#ffffff"); axes.properties = {{"Length", 1.}};
    layout.instances = {grid, axes}; layout.useInstanceModel = true;
    return layout;
}

void ReplayController::ensureInitialDisplayLayout()
{
    if (demo()) return;
    const auto stored = project_ ? project_->topicLayout() : replay::RvizLayout{};
    if (stored.useInstanceModel || !stored.instances.empty() || !stored.displays.empty() || !stored.sourceYaml.isEmpty()) return;
    auto layout = independentLayout();
    if (!path_.isEmpty() && !record_->cloudTopic.isEmpty()) {
        replay::TopicDisplay cloud; cloud.id = QUuid::createUuid().toString(QUuid::Id128);
        cloud.kind = "cloud"; cloud.pluginClass = "rviz/PointCloud2"; cloud.name = tr("Point Cloud · 点云");
        cloud.topic = record_->cloudTopic; cloud.color = QColor("#64c9ee"); layout.instances.push_back(cloud);
    }
    auto document = projectSnapshot(); document.setTopicLayout(replay::withRvizDisplayInstances(layout, layout.instances));
    project_ = std::move(document);
}

bool ReplayController::independentDisplayModelActive() const
{
    if (demo() && (!project_ || !project_->topicLayout().useInstanceModel)) return false;
    return independentLayout().useInstanceModel;
}
QString ReplayController::independentFixedFrame() const
{
    const auto selected = independentLayout().fixedFrame;
    return selected.isEmpty() ? record_->frameId.isEmpty() ? QStringLiteral("map") : record_->frameId : selected;
}
std::vector<replay::TopicDisplay> ReplayController::independentDisplayStates() const
{
    const auto layout = independentLayout();
    auto instances = replay::rvizDisplayInstances(layout);
    QHash<QString, const replay::TopicDisplay *> byId;
    for (const auto &display : instances) byId[display.id] = &display;
    for (auto &display : instances) {
        auto parent = display.parentId; QSet<QString> seen{display.id};
        while (!parent.isEmpty() && byId.contains(parent) && !seen.contains(parent)) {
            seen.insert(parent); const auto *group = byId.value(parent); display.enabled &= group->enabled; parent = group->parentId;
        }
    }
    return instances;
}

QString ReplayController::independentCacheKey(const replay::TopicDisplay &display) const
{
    return display.kind + QChar(0x1f) + display.topic;
}

std::vector<std::shared_ptr<const replay::Recording>> ReplayController::independentDisplayRecordings() const
{
    std::vector<std::shared_ptr<const replay::Recording>> result;
    for (const auto &display : independentDisplayStates()) {
        const auto record = independentData_.value(independentCacheKey(display));
        if (record && std::find(result.begin(), result.end(), record) == result.end()) result.push_back(record);
    }
    return result;
}

QVariantList ReplayController::independentDisplayTypes() const
{
    QVariantList result;
    for (const auto &name : {"Grid", "Axes", "Group", "PointCloud2", "LaserScan", "Path", "Marker", "MarkerArray", "Odometry", "Pose", "PointStamped", "PoseWithCovariance", "TF"}) {
        const auto plugin = QStringLiteral("rviz/") + QLatin1StringView(name);
        result.append(QVariantMap{{"class", plugin}, {"name", QString::fromLatin1(name)}, {"kind", replay::independentDisplayKind(plugin)},
            {"messageTypes", independentMessageTypes(plugin)}, {"backend", QStringLiteral("Standalone C++ / Qt")}});
    }
    return result;
}
QVariantList ReplayController::compatibleIndependentDisplays(const QString &messageType) const
{
    QVariantList result;
    for (const auto &value : independentDisplayTypes()) if (value.toMap().value("messageTypes").toStringList().contains(messageType)) result.append(value);
    return result;
}

QVariantList ReplayController::independentDisplays() const
{
    QVariantList result;
    const auto local = replay::rvizDisplayInstances(independentLayout());
    const auto effective = independentDisplayStates();
    QHash<QString, bool> effectiveEnabled;
    for (const auto &display : effective) effectiveEnabled[display.id] = display.enabled;
    for (const auto &display : local) {
        auto row = display.toJson().toVariantMap();
        row["class"] = display.pluginClass; row["parentId"] = display.parentId;
        row["alpha"] = display.alpha; row["pointSize"] = display.pointSize; row["lineWidth"] = display.lineWidth;
        row["historySeconds"] = display.historySeconds; row["useMessageColor"] = display.useMessageColor;
        row["effectiveEnabled"] = effectiveEnabled.value(display.id, display.enabled);
        row["supported"] = display.kind != "unsupported";
        QString code, message, messageType;
        const auto *catalog = independentTopicEntry(*record_, display);
        if (catalog) messageType = catalog->type;
        row["messageType"] = messageType;
        if (display.kind == "tf") {
            QStringList topics;
            for (const auto &topic : {QStringLiteral("/tf"), QStringLiteral("/tf_static")})
                if (independentCompatible(independentTopicEntry(*record_, topic), display)) topics.append(topic);
            row["sourceTopics"] = topics;
        }
        if (display.kind == "unsupported") { code = "Unsupported"; message = tr("此 Class 没有独立渲染实现；原始属性完整保留。 / Unsupported standalone Class; configuration retained."); }
        else if (display.kind == "group") { code = "Group"; message = tr("分组管理子显示，不代表传感器数据就绪。 / Group only."); }
        else if (display.kind == "grid" || display.kind == "axes") { code = "Configured"; message = tr("静态场景显示已配置；无需传感器消息。 / Static scene display configured."); }
        else if (path_.isEmpty()) { code = "Waiting for Data"; message = tr("等待录包；Topic 和样式已保存。 / Waiting for a Bag source."); }
        else if (display.topic.isEmpty()) { code = "Waiting for Topic"; message = tr("请选择兼容 Topic。 / Select a compatible Topic."); }
        else if (!catalog) { code = "Topic Missing"; message = tr("当前录包没有此 Topic。 / Topic is absent from this Bag."); }
        else if (!independentCompatible(catalog, display)) { code = "Type Mismatch"; message = tr("Topic 类型与显示不兼容，未强制转换。 / Incompatible Topic type."); }
        else {
            const auto key = independentCacheKey(display);
            if (independentErrors_.contains(key)) { code = "Decode Error"; message = independentErrors_.value(key); }
            else if (independentPending_.contains(key)) { code = "Loading"; message = tr("后台读取此 Topic 与 TF；主数据和播放不重载。 / Loading Topic and TF in the background."); }
            else if (const auto data = independentData_.value(key)) {
                size_t samples = 0; bool readyFrame = false, hasCurrent = false;
                const auto stamp = record_->stampAt(position_);
                for (const auto &overlay : data->topicOverlays) if (overlay.display.kind == display.kind &&
                    (overlay.display.topic == display.topic || (display.kind == "tf" && overlay.display.topic == "/tf_static"))) {
                    samples += overlay.samples.size(); const auto *sample = overlay.sampleAt(stamp);
                    if (sample && sample->range.end > sample->range.first) {
                        hasCurrent = true;
                        if (!sample->frameLocked || sample->frameId.isEmpty() || sample->frameId == data->frameId ||
                            (data->transforms && data->transforms->lookup(data->frameId, sample->frameId, stamp))) readyFrame = true;
                    }
                }
                row["decodedSamples"] = qulonglong(samples);
                if (!samples) { code = "No Decoded Data"; message = tr("已检查消息但没有可绘制数据；检查类型、TF 和资源限制。 / No renderable samples; inspect type, TF and resource limits."); }
                else if (!hasCurrent) { code = "Waiting at Time"; message = tr("该播放头时间没有可用几何。 / No geometry at the current experiment time."); }
                else if (!readyFrame) { code = "Missing TF"; message = tr("缺少消息坐标系到 Fixed Frame 的 TF，未伪造变换。 / Required TF is missing."); }
                else { code = "Data Decoded"; message = tr("真实消息已解码；不等于原生 RViz 全兼容或 GUI 验收通过。 / Real data decoded; native plugin parity is not certified."); }
                if (!data->topicWarnings.isEmpty()) message += ' ' + data->topicWarnings.join("; ");
            } else { code = "Waiting for Data"; message = tr("等待读取匹配 Topic。 / Waiting to decode the matching Topic."); }
        }
        row["status"] = code; row["statusText"] = message;
        row["compatibility"] = tr("独立 C++ 显示子集；原始属性可查看，未应用属性保留。 / Standalone renderer subset; unapplied native properties are retained.");
        result.append(row);
    }
    return result;
}

QVariantMap ReplayController::selectedIndependentDisplay() const
{
    for (const auto &value : independentDisplays()) if (value.toMap().value("id").toString() == selectedIndependentDisplayId_) return value.toMap();
    return {};
}
void ReplayController::selectIndependentDisplay(const QString &id)
{
    const auto rows = independentDisplays();
    if (!id.isEmpty() && std::none_of(rows.begin(), rows.end(), [&](const auto &row) { return row.toMap().value("id").toString() == id; })) return;
    if (selectedIndependentDisplayId_ == id) return;
    selectedIndependentDisplayId_ = id; emit independentDisplaySelectionChanged(); emit independentDisplaysChanged();
}

void ReplayController::storeIndependentLayout(const replay::RvizLayout &layout, const QString &message)
{
    const auto before = projectSnapshot(); auto document = before;
    document.setTopicLayout(replay::withRvizDisplayInstances(layout, replay::rvizDisplayInstances(layout)));
    project_ = std::move(document); recordProjectEdit(before); refreshDisplayVisibility();
    if (!message.isEmpty()) status_ = message;
    if (before.sceneBackground() != project_->sceneBackground()) emit sceneStyleChanged();
    emit recordingChanged(); emit independentDisplaysChanged(); emit stateChanged();
}

QString ReplayController::addIndependentDisplay(const QString &pluginClass, const QString &topic, const QString &parentId)
{
    if (loading_) return {};
    try {
        const auto types = independentDisplayTypes();
        if (std::none_of(types.begin(), types.end(), [&](const auto &row) { return row.toMap().value("class").toString() == pluginClass; })) {
            status_ = tr("此 Class 未提供独立创建实现；可从 .rviz 保真导入未知 Class。 / Unsupported Class for standalone creation."); emit stateChanged(); return {};
        }
        auto layout = independentLayout(); auto instances = replay::rvizDisplayInstances(layout);
        replay::TopicDisplay display; display.id = QUuid::createUuid().toString(QUuid::Id128); display.pluginClass = pluginClass;
        display.kind = replay::independentDisplayKind(pluginClass); display.name = pluginClass.section('/', -1); display.parentId = parentId;
        display.topic = display.kind == "tf" ? QStringLiteral("/tf") : topic.trimmed(); display.color = QColor("#64c9ee");
        if (display.kind == "grid") display.properties = {{"Cell Size", 1.}, {"Plane Cell Count", 20.}};
        if (display.kind == "axes") display.properties = {{"Length", 1.}};
        instances.push_back(display); layout = replay::withRvizDisplayInstances(layout, instances);
        storeIndependentLayout(layout, tr("显示实例已添加；缺少数据时保留 Waiting 状态。 / Display instance added."));
        selectIndependentDisplay(display.id); return display.id;
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); emit stateChanged(); return {}; }
}

QString ReplayController::duplicateIndependentDisplay(const QString &id)
{
    if (loading_) return {};
    try {
        auto layout = independentLayout(); auto instances = replay::rvizDisplayInstances(layout);
        const auto source = std::find_if(instances.begin(), instances.end(), [&](const auto &display) { return display.id == id; });
        if (source == instances.end()) return {};
        const auto original = instances; QHash<QString, QString> mapping;
        mapping[id] = QUuid::createUuid().toString(QUuid::Id128);
        bool expanded = true;
        while (expanded) { expanded = false; for (const auto &display : original) if (!mapping.contains(display.id) && mapping.contains(display.parentId)) {
            mapping[display.id] = QUuid::createUuid().toString(QUuid::Id128); expanded = true;
        } }
        for (auto display : original) if (mapping.contains(display.id)) {
            const auto originalId = display.id; display.id = mapping.value(originalId); display.sourcePath.clear();
            if (originalId == id) display.name += tr(" Copy · 副本");
            if (mapping.contains(display.parentId)) display.parentId = mapping.value(display.parentId);
            instances.push_back(display);
        }
        storeIndependentLayout(replay::withRvizDisplayInstances(layout, instances), tr("已复制显示；数据缓存共享，样式独立。 / Display duplicated with shared data."));
        selectIndependentDisplay(mapping.value(id)); return mapping.value(id);
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); emit stateChanged(); return {}; }
}

void ReplayController::removeIndependentDisplays(const QStringList &ids)
{
    if (loading_ || ids.isEmpty()) return;
    try {
        auto layout = independentLayout(); auto instances = replay::rvizDisplayInstances(layout); QSet<QString> removed(ids.begin(), ids.end());
        bool expanded = true; while (expanded) { expanded = false; for (const auto &display : instances) if (removed.contains(display.parentId) && !removed.contains(display.id)) { removed.insert(display.id); expanded = true; } }
        std::erase_if(instances, [&](const auto &display) { return removed.contains(display.id); });
        storeIndependentLayout(replay::withRvizDisplayInstances(layout, instances), tr("显示已删除；源数据保留，可撤销。 / Displays removed; source data retained."));
        if (removed.contains(selectedIndependentDisplayId_)) selectIndependentDisplay({});
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}

bool ReplayController::editIndependentDisplay(const QString &id, const QVariantMap &changes)
{
    if (loading_ || changes.isEmpty()) return false;
    try {
        auto layout = independentLayout(); auto instances = replay::rvizDisplayInstances(layout);
        const auto found = std::find_if(instances.begin(), instances.end(), [&](const auto &display) { return display.id == id; });
        if (found == instances.end()) return false;
        auto &display = *found;
        const QSet<QString> allowed{"name", "enabled", "topic", "color", "alpha", "pointSize", "lineWidth", "historySeconds", "useMessageColor", "parentId", "Cell Size", "Plane Cell Count", "Length"};
        for (auto i = changes.begin(); i != changes.end(); ++i) {
            if (!allowed.contains(i.key())) throw std::runtime_error("该属性没有独立渲染实现，未伪造写入成功。 / Unsupported independent property.");
            if (i.key() == "name") { const auto name = i.value().toString().trimmed(); if (name.isEmpty() || name.size() > 256) throw std::runtime_error("显示名称无效。 / Invalid display name."); display.name = name; }
            else if (i.key() == "enabled") display.enabled = i.value().toBool();
            else if (i.key() == "topic") { if (display.kind == "tf" || !independentDataKind(display.kind)) throw std::runtime_error("此显示不支持独立 Topic 修改。 / This display has no editable Topic."); display.topic = i.value().toString().trimmed(); }
            else if (i.key() == "parentId") display.parentId = i.value().toString();
            else if (i.key() == "color") { const QColor color(i.value().toString()); if (!color.isValid()) throw std::runtime_error("颜色无效。 / Invalid color."); display.color = color; display.properties["_color_edited"] = true; }
            else if (i.key() == "useMessageColor") display.useMessageColor = i.value().toBool();
            else {
                bool ok = false; const auto number = i.value().toDouble(&ok);
                const auto low = i.key() == "alpha" || i.key() == "historySeconds" ? 0. : .1;
                const auto high = i.key() == "alpha" ? 1. : i.key() == "historySeconds" ? 86400. : i.key() == "Plane Cell Count" ? 1000. : i.key() == "Cell Size" || i.key() == "Length" ? 10000. : 512.;
                if (!ok || !std::isfinite(number) || number < low || number > high) throw std::runtime_error("属性数值超出有效范围。 / Property value exceeds supported limits.");
                if (i.key() == "alpha") display.alpha = number;
                else if (i.key() == "pointSize") display.pointSize = number;
                else if (i.key() == "lineWidth") display.lineWidth = number;
                else if (i.key() == "historySeconds") display.historySeconds = number;
                else display.properties[i.key()] = number;
            }
        }
        storeIndependentLayout(replay::withRvizDisplayInstances(layout, instances)); return true;
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); emit stateChanged(); return false; }
}

QVariantList ReplayController::independentDisplayProperties(const QString &id) const
{
    for (const auto &display : replay::rvizDisplayInstances(independentLayout())) if (display.id == id) {
        try { return replay::rvizDisplayPropertyTree(display); }
        catch (const std::exception &error) { return {QVariantMap{{"name", "Configuration error"}, {"value", QString::fromUtf8(error.what())}, {"readOnly", true}}}; }
    }
    return {};
}

void ReplayController::retryIndependentDisplays()
{
    if (independentWatcher_.isRunning()) return;
    independentErrors_.clear(); refreshIndependentDisplayData();
}

void ReplayController::refreshIndependentDisplayData()
{
    const auto frame = independentFixedFrame();
    if (independentDataSource_ != path_ || independentDataFrame_ != frame || independentSourceGeneration_ != recordingGeneration_) {
        if (independentCancel_) *independentCancel_ = true;
        ++independentDataGeneration_; independentDataSource_ = path_; independentDataFrame_ = frame;
        independentSourceGeneration_ = recordingGeneration_;
        independentData_.clear(); independentErrors_.clear(); independentPending_.clear();
    }
    if (loading_ || path_.isEmpty() || independentWatcher_.isRunning()) { emit independentDisplaysChanged(); return; }
    std::vector<replay::TopicDisplay> missing; QStringList keys;
    const auto states = independentDisplayStates(); QSet<QString> activeKeys;
    for (const auto &display : states) if (independentDataKind(display.kind)) activeKeys.insert(independentCacheKey(display));
    for (auto i = independentData_.begin(); i != independentData_.end();) {
        if (!activeKeys.contains(i.key())) i = independentData_.erase(i); else ++i;
    }
    for (auto display : states) {
        if (!independentDataKind(display.kind) || display.topic.isEmpty()) continue;
        const auto *catalog = independentTopicEntry(*record_, display);
        if (!independentCompatible(catalog, display)) continue;
        const auto key = independentCacheKey(display);
        if (independentData_.contains(key) || independentErrors_.contains(key) || keys.contains(key)) continue;
        display.id = "data:" + key; display.enabled = true; keys.append(key);
        if (display.kind == "tf") {
            for (const auto &topic : {QStringLiteral("/tf"), QStringLiteral("/tf_static")}) {
                if (!independentCompatible(independentTopicEntry(*record_, topic), display)) continue;
                auto source = display; source.topic = topic; source.id += QStringLiteral(":") + topic; missing.push_back(std::move(source));
            }
        } else missing.push_back(std::move(display));
    }
    if (missing.empty()) { emit independentDisplaysChanged(); return; }
    for (const auto &key : keys) independentPending_.insert(key);
    independentCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto source = path_; const auto generation = independentDataGeneration_; const auto cancel = independentCancel_;
    independentWatcher_.setFuture(QtConcurrent::run([source, frame, generation, keys, missing, cancel] {
        try {
            // The returned batch is solely owned here. Move geometry into one
            // cache per (kind, Topic), while sharing the immutable TF tree.
            // Deleting a Display can therefore release its Topic's CPU data.
            auto batch = std::const_pointer_cast<replay::Recording>(replay::loadDisplayData(source, frame, missing, [cancel] { return cancel->load(); }));
            IndependentDisplayLoad result{source, frame, {}, generation, keys, {}};
            for (const auto &key : keys) {
                auto data = std::make_shared<replay::Recording>();
                data->name = batch->name; data->frameId = batch->frameId; data->transforms = batch->transforms;
                data->maximumGap = batch->maximumGap; data->recordedOrigin = batch->recordedOrigin;
                data->timelineFirstStampNs = batch->timelineFirstStampNs; data->timelineLastStampNs = batch->timelineLastStampNs;
                data->hasMessages = batch->hasMessages; data->timelineTimeBasis = batch->timelineTimeBasis;
                data->center = batch->center; data->radius = batch->radius; data->showMainCloud = false;
                const auto kind = key.section(QChar(0x1f), 0, 0), topic = key.section(QChar(0x1f), 1);
                for (auto &overlay : batch->topicOverlays) if (overlay.display.kind == kind &&
                    (overlay.display.topic == topic || (kind == "tf" && overlay.display.topic == "/tf_static")))
                    data->topicOverlays.push_back(std::move(overlay));
                for (const auto &warning : batch->topicWarnings) if (warning.contains(topic)) data->topicWarnings.append(warning);
                result.records[key] = std::move(data);
            }
            return result;
        }
        catch (const std::exception &error) { return IndependentDisplayLoad{source, frame, QString::fromUtf8(error.what()), generation, keys, {}}; }
    }));
    emit independentDisplaysChanged();
}

void ReplayController::setTopicDisplayEnabled(int index, bool enabled)
{
    if (loading_ || !project_)
        return;
    auto layout = project_->topicLayout();
    if (index < 0 || size_t(index) >= layout.displays.size())
        return;
    const auto selected = layout.displays[size_t(index)];
    if (layout.useInstanceModel) {
        const auto instance = std::find_if(layout.instances.begin(), layout.instances.end(), [&](const auto &item) {
            return item.id == selected.id || (!selected.sourcePath.isEmpty() && item.sourcePath == selected.sourcePath);
        });
        if (instance != layout.instances.end()) editIndependentDisplay(instance->id, {{"enabled", enabled}});
        return;
    }
    if (selected.enabled == enabled)
        return;
    // A single RViz TF node produces dynamic and static render layers. Keep its
    // checkbox consistent across both layers and across project/RViz exports.
    for (auto &display : layout.displays)
        if (display.id == selected.id || (!selected.sourcePath.isEmpty() && display.sourcePath == selected.sourcePath))
            display.enabled = enabled;
    const auto before = projectSnapshot();
        auto document = projectSnapshot();
    document.setTopicLayout(layout);
    project_ = std::move(document);
        recordProjectEdit(before);
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
    if (loading_)
        return;
    auto normalized = frame.trimmed();
    while (normalized.startsWith('/'))
        normalized.remove(0, 1);
    auto layout = independentLayout();
    if (layout.fixedFrame != normalized) {
        layout.fixedFrame = normalized;
        storeIndependentLayout(layout);
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
    auto report = tr("原始分组、插件及属性保存在项目；独立 C++ 渲染仅支持明确列出的子集。 / Original RViz configuration retained; standalone C++ renderer supports an explicit subset.");
    if (!layout.skipped.isEmpty()) report += tr(" 未按原插件渲染：%1").arg(layout.skipped.join(QStringLiteral("、")));
    return report;
}

void ReplayController::exportRviz(const QUrl &url)
{
    if (loading_ || !url.isLocalFile()) return;
    try {
        if (QFileInfo(url.toLocalFile()).suffix().compare("rviz", Qt::CaseInsensitive) != 0 || referencesFile(url.toLocalFile()))
            throw std::runtime_error("请选择 .rviz 文件，不能覆盖项目素材。 / Invalid RViz destination.");
        auto layout = independentLayout();
        if (layout.fixedFrame.isEmpty()) layout.fixedFrame = independentFixedFrame();
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
    const auto instances = replay::rvizDisplayInstances(layout);
    storeIndependentLayout(replay::withRvizDisplayInstances(layout, instances),
        tr("已恢复 %1 个 RViz 显示/分组；未知 Class 和属性保留，后台仅读取匹配 Topic。 / RViz display tree restored.").arg(instances.size()));
    selectIndependentDisplay(instances.empty() ? QString() : instances.front().id);
    emit nativeConfigChanged();
    emit nativeConfigLoaded(QVariantMap{{"yaml", layout.sourceYaml}});
}

void ReplayController::openBag(const QUrl &url)
{
    if (loading_ || !url.isLocalFile())
        return;
    const auto path = url.toLocalFile();
    try {
        auto document = projectSnapshot(); document.setBagSource(path);
        load(path, {}, std::move(document), true);
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}

void ReplayController::openDemo()
{
    if (loading_)
        return;
    path_.clear();
    demoMode_ = true;
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
            empty->radius = 1.f;
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
    auto project = replay::ProjectDocument::create(demo() ? QStringLiteral("demo") : path_.isEmpty() ? QStringLiteral("none") : QStringLiteral("bag"), path_);
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
        auto document = projectSnapshot();
        if (!project_ && !demo()) document.setTopicLayout(independentLayout());
        project_ = document.saveAs(url.toLocalFile());
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
    if (independentCancel_) *independentCancel_ = true;
    ++independentDataGeneration_; independentPending_.clear();
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

QVariantList ReplayController::speedMeters() const
{
    auto result = project_ ? project_->composition().value("speed_meters").toArray().toVariantList() : QVariantList{};
    const auto legacy = speedGauge();
    const bool containsLegacy = std::any_of(result.cbegin(), result.cend(), [](const QVariant &v) { return v.toMap().value("id").toString() == "speed"; });
    if (legacy.value("visible").toBool() && !containsLegacy) {
        const QString savedUnit = QSettings().value("telemetry/speedUnit", "m/s").toString();
        const QString unit = savedUnit == "km/h" ? savedUnit : QStringLiteral("m/s");
        result.prepend(QVariantMap{{"id", "speed"}, {"label", tr("Speed Meter · 速度表")}, {"source", QVariantMap{}},
            {"settings", QVariantMap{{"unit", unit}, {"mode", "total"}, {"statisticsMode", "entire"}, {"rangeMode", "manual"},
                {"rangeMinimumSI", 0.}, {"rangeMaximumSI", legacy.value("limit", 10.)}}},
            {"style", QVariantMap{{"backgroundMode", legacy.value("transparent", true).toBool() ? "transparent" : "translucent"}}}});
    }
    return result;
}

QString ReplayController::addSpeedMeter()
{
    if (loading_) return {};
    const auto before = projectSnapshot();
    auto document = before;
    auto composition = document.composition();
    auto meters = QJsonArray::fromVariantList(speedMeters());
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    meters.append(QJsonObject{{"id", id}, {"label", tr("Speed Meter %1 · 速度表").arg(meters.size() + 1)},
        {"source", QJsonObject{}}, {"settings", QJsonObject{{"unit", QSettings().value("telemetry/speedUnit", "m/s").toString() == "km/h" ? "km/h" : "m/s"},
            {"mode", "total"}, {"statisticsMode", "entire"}, {"rangeMode", "auto"}}}, {"style", QJsonObject{}}});
    composition["speed_meters"] = meters;
    document.setComposition(composition); project_ = std::move(document);
    recordProjectEdit(before); emit recordingChanged(); canvas_->select(id);
    return id;
}

void ReplayController::editSpeedMeter(const QString &id, const QString &section, const QVariantMap &changes)
{
    if (loading_ || !QStringList{"source", "settings", "style"}.contains(section)) return;
    if (section == "settings" && changes.contains("unit") && !QStringList{"m/s", "km/h"}.contains(changes.value("unit").toString())) return;
    const auto before = projectSnapshot(); auto document = before; auto composition = document.composition();
    auto meters = QJsonArray::fromVariantList(speedMeters()); bool found = false;
    for (qsizetype i = 0; i < meters.size(); ++i) {
        auto meter = meters[i].toObject(); if (meter.value("id").toString() != id) continue;
        auto value = section == "source" ? QJsonObject{} : meter.value(section).toObject();
        const auto patch = QJsonObject::fromVariantMap(changes);
        for (auto p = patch.begin(); p != patch.end(); ++p) value[p.key()] = p.value();
        meter[section] = value; meters[i] = meter; found = true; break;
    }
    if (!found) return;
    composition["speed_meters"] = meters; document.setComposition(composition); project_ = std::move(document);
    recordProjectEdit(before);
    if (section == "settings" && changes.contains("unit")) QSettings().setValue("telemetry/speedUnit", changes.value("unit"));
    emit recordingChanged();
}

void ReplayController::setSpeedGauge(const QVariantMap &settings)
{
    if (loading_)
        return;
    try {
        const auto before = projectSnapshot();
        auto project = projectSnapshot();
        project.setSpeedGauge(settings);
        project_ = std::move(project);
        recordProjectEdit(before);
        emit recordingChanged();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
        emit stateChanged();
    }
}

QVariantMap ReplayController::moviePreferences() const
{
    auto prefs = project_ ? project_->moviePreferences() : QVariantMap{{"speed", 1.}, {"audio", QString()}};
    const auto rate = prefs.value("speed", 1.).toDouble();
    if (rate != 1. && rate != .5 && rate != .25 && rate != 2. && rate != 4.)
        prefs.insert("speed", 1.);
    const auto id = prefs.value("audio").toString();
    const auto layer = std::find_if(clips_.begin(), clips_.end(), [&](const auto &clip) {
        return !id.isEmpty() && clip.id == id;
    });
    prefs.insert("audioIndex", layer == clips_.end() ? -1 : int(layer - clips_.begin()));
    return prefs;
}

void ReplayController::setMoviePreferences(double speed, int audioIndex)
{
    if (loading_ || (speed != 1. && speed != .5 && speed != .25 && speed != 2. && speed != 4.) ||
        audioIndex < -1 || (audioIndex >= 0 && size_t(audioIndex) >= clips_.size()))
        return;
    const auto before = projectSnapshot();
    auto project = projectSnapshot();
    project.setMoviePreferences(speed, audioIndex < 0 ? QString() : clips_[size_t(audioIndex)].id);
    project_ = std::move(project);
    if (playing()) {
        // Keep the current experiment position when changing rate mid-playback.
        start_ = position_;
        clock_.restart();
    }
    recordProjectEdit(before);
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

    const auto before = projectSnapshot();
        auto project = projectSnapshot();
    project.setCameraKind(QString::fromLatin1(replay::CameraPresets[size_t(index)].id));
    project_ = std::move(project);
        recordProjectEdit(before);
    cameraIndex_ = index;
    emit cameraChanged();
}

QColor ReplayController::sceneBackground() const
{
    return QColor(project_ ? project_->sceneBackground() : QString::fromLatin1(DesignTokens::SceneBackground));
}

void ReplayController::setSceneBackground(const QColor &color)
{
    if (loading_ || !color.isValid() || color.alpha() != 255 || (color == sceneBackground() && independentLayout().backgroundColor == color)) return;
    const auto before = projectSnapshot();
    auto document = before;
    document.setSceneBackground(color.name());
    project_ = std::move(document);
    recordProjectEdit(before);
    emit sceneStyleChanged();
    emit independentDisplaysChanged();
}

void ReplayController::setTransparentCanvas(bool transparent)
{
    if (loading_ || transparentCanvas() == transparent)
        return;
    const auto before = projectSnapshot();
        auto document = projectSnapshot();
    document.setTransparentCanvas(transparent);
    project_ = std::move(document);
        recordProjectEdit(before);
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
        const auto before = projectSnapshot();
        auto document = projectSnapshot();
        document.setLayoutCue(position_, mode, videoId, transition);
        project_ = std::move(document);
        recordProjectEdit(before);
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
    const auto before = projectSnapshot();
        project_->removeLayoutCue(time);
        recordProjectEdit(before);
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
    const auto before = projectSnapshot();
        auto document = projectSnapshot();
    document.setLayerEnabled(role, enabled);
    project_ = std::move(document);
        recordProjectEdit(before);
    emit recordingChanged();
}

void ReplayController::setComparePlans(bool compare)
{
    if (loading_ || compare == comparePlans())
        return;
    const auto before = projectSnapshot();
        auto document = projectSnapshot();
    document.setComparePlans(compare);
    project_ = std::move(document);
        recordProjectEdit(before);
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
        const double time = double(stamp - record_->stampAt(0.)) / 1.e9;
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
        const auto before = projectSnapshot();
        auto document = projectSnapshot();
        document.addTimelineMarker(position_);
        project_ = std::move(document);
        recordProjectEdit(before);
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
        const auto before = projectSnapshot();
        project_->replaceTimelineMarker(time, label);
        recordProjectEdit(before);
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

void ReplayController::setMainView(const QVariantMap &settings)
{
    if (loading_) return;
    try {
        const auto before = projectSnapshot();
        auto document = projectSnapshot();
        document.setMainView(settings); project_ = std::move(document);
        recordProjectEdit(before);
        emit recordingChanged();
    } catch (const std::exception &error) { status_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}

void ReplayController::restoreMainView()
{
    if (loading_) return;
    const auto before = projectSnapshot();
    auto document = before;
    auto scene = document.composition();
    scene["has_main_view"] = true;
    document.setComposition(scene);
    project_ = std::move(document);
    recordProjectEdit(before);
    canvas_->refresh();
    emit recordingChanged();
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
        const auto before = projectSnapshot();
        auto document = projectSnapshot();
        document.addLayerMotion(kind, id, begin, position_, duration, width, x, y);
        if (kind == "main")
            document.setMainViewWidth(normalWidth);
        project_ = std::move(document);
        recordProjectEdit(before);
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
    const auto before = projectSnapshot();
        project_->removeLayerMotion(index);
        recordProjectEdit(before);
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
        const auto before = projectSnapshot();
        auto document = projectSnapshot();
        document.addCloudView(QString::fromLatin1(replay::CameraPresets[size_t(camera)].id));
        project_ = std::move(document);
        recordProjectEdit(before);
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
        const auto before = projectSnapshot();
        project_->setCloudView(id, settings);
        recordProjectEdit(before);
        refreshCloudViews();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
        emit stateChanged();
    }
}

void ReplayController::removeCloudView(const QString &id)
{
    removeLayers({id});
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
        const auto before = projectSnapshot();
        auto document = projectSnapshot();
        const double end = duration() > 0. ? std::min(duration(), position_ + 3.) : position_ + 3.;
        const double start = end <= position_ ? std::max(0., end - 3.) : position_;
        document.addSceneLayer(kind, content, start, end);
        project_ = std::move(document);
        recordProjectEdit(before);
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
        const auto before = projectSnapshot();
        project_->editSceneLayer(id, QJsonObject::fromVariantMap(changes));
        recordProjectEdit(before);
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
    removeLayers({id});
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
    videoLayerModel_.setLayers(videoLayers_);
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
        const auto before = projectSnapshot();
        auto document = projectSnapshot();
        document.addVideo(url.toLocalFile());
        project_ = std::move(document);
        recordProjectEdit(before);
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
        const auto before = *project_;
        project_->setVideoTiming(index, changes);
        if (before.json() == project_->json())
            return;
        recordProjectEdit(before);
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
    const auto previousRecord = record_;
    const auto previousPath = path_;
    project_ = redo ? edit.after : edit.before;
    record_ = redo ? edit.afterRecord : edit.beforeRecord;
    path_ = redo ? edit.afterPath : edit.beforePath;
    if (previousRecord != record_ || previousPath != path_) ++recordingGeneration_;
    destination.push_back(edit);
    history.pop_back();
    refreshProjectModels();
    status_ = redo ? tr("已重做编辑。") : tr("已撤销编辑。");
    emit editHistoryChanged();
    emit stateChanged();
}

void ReplayController::recordProjectEdit(const replay::ProjectDocument &before,
    std::shared_ptr<const replay::Recording> beforeRecord, const QString &beforePath)
{
    if (projectEditDepth_ > 0 || !project_ || before.json() == project_->json()) return;
    undo_.push_back({before, *project_, beforeRecord ? beforeRecord : record_, record_, beforeRecord ? beforePath : path_, path_});
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    emit editHistoryChanged();
}

void ReplayController::refreshProjectModels()
{
    refreshDisplayVisibility();
    refreshVideoLayers(); refreshSceneLayers(); refreshCloudViews(); refreshMotions(); refreshLayoutCues();
    emit markersChanged(); emit recordingChanged(); emit transparentCanvasChanged(); emit sceneStyleChanged();
    emit renderBackendChanged();
    emit positionChanged(); canvas_->refresh();
}
void ReplayController::removeLayers(const QStringList &ids)
{
    if (loading_ || ids.isEmpty()) return;
    const auto before=projectSnapshot(); auto document=before;
    document.removeCanvasLayers(ids); project_=std::move(document); recordProjectEdit(before);
    canvas_->clearSelection(); refreshProjectModels();
    status_=tr("已删除 %1 个图层。原始素材保留在 Sources；Ctrl+Z 可恢复。 / Layers removed; sources retained.").arg(ids.size());emit stateChanged();
}
void ReplayController::clearCanvas()
{
    QStringList ids;for(const auto &v:canvas_->layers())ids.append(v.toMap().value("id").toString());removeLayers(ids);
}
void ReplayController::addSource(const QUrl &file,const QString &kind)
{
    if(loading_||!file.isLocalFile())return;
    try{const auto before=projectSnapshot();auto document=before;document.addSource(file.toLocalFile(),kind);project_=std::move(document);recordProjectEdit(before);emit sourcesChanged();}
    catch(const std::exception &e){status_=QString::fromUtf8(e.what());emit stateChanged();}
}
void ReplayController::cancelSourceRemoval(){pendingSourceRemoval_.clear();emit sourcesChanged();}
bool ReplayController::removeSources(const QStringList &ids,bool removeReferences)
{
    if(loading_||ids.isEmpty())return false;
    const auto before=projectSnapshot();const auto previousRecord=record_;const auto previousPath=path_;
    auto document=before;QVariantList affected,removed;
    for(const auto &v:document.sources()){const auto s=v.toMap();if(!ids.contains(s.value("id").toString()))continue;
        removed.append(s);for(const auto &r:s.value("references").toList())affected.append(r);
    }
    if(!removeReferences&&!affected.isEmpty()){
        pendingSourceRemoval_={{"ids",ids},{"references",affected},{"sources",removed}};emit sourcesChanged();return false;
    }
    for(const auto &id:ids)document.removeSource(id,true);
    if (!document.bag().isEmpty() && document.extrinsics() != before.extrinsics()) {
        replacementBefore_ = before;
        replacementRemovedSources_ = removed;
        pendingSourceRemoval_.clear();
        const auto bag = document.bag();
        load(bag, {cloudTopic(), odometryTopic(), mapMode_}, std::move(document), true);
        emit sourcesChanged();
        return true;
    }
    project_=std::move(document);
    if(!previousPath.isEmpty()&&project_->bag().isEmpty()){
        auto empty=std::make_shared<replay::Recording>();empty->name=tr("No ROS data · 未载入 ROS 数据");empty->radius=1.;record_=std::move(empty);path_.clear();++recordingGeneration_;
    }
    recordProjectEdit(before,previousRecord,previousPath);pendingSourceRemoval_.clear();canvas_->clearSelection();refreshProjectModels();
    for(const auto &v:removed){const auto s=v.toMap();emit sourceRemoved(s.value("kind").toString(),s.value("url").toUrl());}
    status_=tr("已从项目移除素材及确认的引用；磁盘原文件未修改。Ctrl+Z 可恢复。");emit stateChanged();return true;
}
void ReplayController::clearUnusedSources()
{
    QStringList ids;for(const auto &v:sources()){const auto s=v.toMap();if(s.value("referenceCount").toInt()==0)ids.append(s.value("id").toString());}removeSources(ids,false);
}
void ReplayController::renameSource(const QString &id,const QString &name)
{
    if(loading_)return;const auto before=projectSnapshot();auto document=before;document.renameSource(id,name);project_=std::move(document);recordProjectEdit(before);emit sourcesChanged();
}
void ReplayController::replaceSource(const QString &id,const QUrl &file)
{
    if(loading_||!file.isLocalFile())return;
    try{
        const auto before=projectSnapshot();auto document=before;document.replaceSource(id,file.toLocalFile());
        if (!document.bag().isEmpty() && (document.bag() != before.bag() || document.extrinsics() != before.extrinsics())) {
            replacementBefore_ = before;
            const auto bag = document.bag();
            load(bag, {cloudTopic(), odometryTopic(), mapMode_}, std::move(document), true);
            return;
        }
        project_=std::move(document);recordProjectEdit(before);refreshProjectModels();
    }
    catch(const std::exception &e){status_=QString::fromUtf8(e.what());emit stateChanged();}
}
void ReplayController::addSourceLayer(const QString &id)
{
    for(const auto &v:sources()){const auto s=v.toMap();if(s.value("id").toString()!=id)continue;
        if(s.value("kind").toString()=="video")addVideo(s.value("url").toUrl());
        else if(s.value("kind").toString()=="image")addSceneImage(s.value("url").toUrl());
        else if(s.value("kind").toString()=="bag")openBag(s.value("url").toUrl());
        return;
    }
}
