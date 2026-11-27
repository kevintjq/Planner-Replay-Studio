#include "project_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace replay {
namespace {
[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toUtf8().constData());
}

QString absolutePath(const QString &path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return QDir::cleanPath(parent.isEmpty() ? info.absoluteFilePath()
                                           : QDir(parent).filePath(info.fileName()));
}

void validateRoot(const QJsonObject &root)
{
    if (root.value("version").toDouble() != 1.)
        fail(QStringLiteral("不支持此 .slamfpv 项目版本。"));
    const auto source = root.value("source").toString();
    const auto bag = root.value("bag");
    if ((source != "bag" && source != "demo" && source != "none") ||
        (source == "bag") != (bag.isString() && !bag.toString().isEmpty()))
        fail(QStringLiteral("项目的数据源无效。"));
    for (const auto &key : {QStringLiteral("values"), QStringLiteral("enabled"), QStringLiteral("composition")}) {
        if (!root.value(key).isObject())
            fail(QStringLiteral("项目缺少有效的 %1 对象。").arg(key));
    }
    const auto values = root.value("values").toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!it.value().isString())
            fail(QStringLiteral("项目参数 %1 必须为文本。").arg(it.key()));
    }
    const auto enabled = root.value("enabled").toObject();
    for (auto it = enabled.begin(); it != enabled.end(); ++it) {
        if (!it.value().isBool())
            fail(QStringLiteral("项目开关 %1 必须为布尔值。").arg(it.key()));
    }
}

void writeManifest(const QString &path, const QJsonObject &root)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        fail(QStringLiteral("无法写入项目 %1：%2").arg(path, file.errorString()));
    const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        fail(QStringLiteral("无法保存项目 %1：%2").arg(path, file.errorString()));
}

void copyMedia(const QString &source, const QString &destination, const std::function<bool()> &cancel)
{
    QFile input(source);
    QFile output(destination);
    if (!QFileInfo(source).isFile() || !input.open(QIODevice::ReadOnly))
        fail(QStringLiteral("无法读取项目素材：%1").arg(source));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        fail(QStringLiteral("无法写入项目素材：%1").arg(destination));

    QByteArray chunk;
    chunk.resize(4 * 1024 * 1024);
    for (;;) {
        if (cancel && cancel())
            fail(QStringLiteral("已取消项目打包。"));
        const auto count = input.read(chunk.data(), chunk.size());
        if (count < 0 || (count > 0 && output.write(chunk.constData(), count) != count))
            fail(QStringLiteral("无法复制项目素材：%1").arg(source));
        if (count == 0)
            break;
    }
    if (!output.flush())
        fail(QStringLiteral("无法写入项目素材：%1").arg(destination));
}

QString referenceFrom(const QString &directory, const QString &path)
{
    return QDir::fromNativeSeparators(QDir(directory).relativeFilePath(path));
}
}

ProjectDocument::ProjectDocument(QString path, QJsonObject root)
    : path_(std::move(path)), root_(std::move(root))
{
    validateRoot(root_);
    media(); // Validate asset references before changing the active project.
}

ProjectDocument ProjectDocument::load(const QString &manifest)
{
    const auto path = absolutePath(manifest);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("无法读取项目 %1：%2").arg(path, file.errorString()));
    QJsonParseError issue;
    const auto document = QJsonDocument::fromJson(file.readAll(), &issue);
    if (issue.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("项目 JSON 无效：%1").arg(issue.errorString()));
    return ProjectDocument(path, document.object());
}

ProjectDocument ProjectDocument::create(const QString &source, const QString &bag)
{
    QJsonObject scene{{"main_view", QJsonObject{{"kind", "rear"}, {"width", 1.}, {"position", QJsonArray{.5, .5}}}},
                      {"views", QJsonArray{}}, {"videos", QJsonArray{}}, {"texts", QJsonArray{}}};
    return ProjectDocument({}, QJsonObject{{"version", 1}, {"source", source},
        {"bag", bag.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(absolutePath(bag))},
        {"extrinsics", QJsonValue(QJsonValue::Null)}, {"values", QJsonObject{}},
        {"enabled", QJsonObject{}}, {"composition", scene}});
}

QString ProjectDocument::source() const { return root_.value("source").toString(); }
QString ProjectDocument::bag() const { return absoluteMedia(root_.value("bag").toString()); }
bool ProjectDocument::isPortable() const { return root_.value("portable").toBool(); }
QJsonObject ProjectDocument::values() const { return root_.value("values").toObject(); }

QString ProjectDocument::extrinsics() const
{
    return absoluteMedia(root_.value("extrinsics").toString());
}

bool ProjectDocument::isLayerEnabled(const QString &layer) const
{
    return root_.value("enabled").toObject().value(layer).toBool(true);
}

void ProjectDocument::setLayerEnabled(const QString &layer, bool isEnabled)
{
    auto enabled = root_.value("enabled").toObject();
    enabled.insert(layer, isEnabled);
    root_.insert("enabled", enabled);
}

QVariantMap ProjectDocument::speedGauge() const
{
    const auto scene = root_.value("composition").toObject();
    const auto placement = scene.value("speed_position").toArray();
    const double end = scene.value("speed_end").toDouble(-1.);
    return {{"visible", scene.value("show_speed").toBool(false)},
            {"start", scene.value("speed_start").toDouble(0.)}, {"end", end},
            {"limit", scene.value("speed_limit").toDouble(10.)},
            {"scale", scene.value("speed_scale").toDouble(1.)},
            {"x", placement.size() == 2 ? placement[0].toDouble(.05) : .05},
            {"y", placement.size() == 2 ? placement[1].toDouble(.32) : .32},
            {"transparent", scene.value("speed_transparent").toBool(true)}};
}

void ProjectDocument::setSpeedGauge(const QVariantMap &changed)
{
    auto setup = speedGauge();
    for (auto entry = changed.cbegin(); entry != changed.cend(); ++entry)
        setup.insert(entry.key(), entry.value());
    const auto validNumber = [&](const char *name) {
        bool ok = false;
        const double value = setup.value(QLatin1StringView(name)).toDouble(&ok);
        if (!ok || !std::isfinite(value))
            fail(QStringLiteral("速度表参数无效。"));
        return value;
    };
    const double begin = validNumber("start"), end = validNumber("end");
    const double limit = validNumber("limit"), scale = validNumber("scale");
    const double x = validNumber("x"), y = validNumber("y");
    if (begin < 0. || (end >= 0. && end <= begin) || limit <= 0. || scale < .35 || scale > 2.5 ||
        x < 0. || x > 1. || y < 0. || y > 1.)
        fail(QStringLiteral("请检查速度表时间区间、量程和缩放。"));
    auto scene = root_.value("composition").toObject();
    scene.insert("show_speed", setup.value("visible").toBool());
    scene.insert("speed_start", begin);
    scene.insert("speed_end", end < 0. ? QJsonValue(QJsonValue::Null) : QJsonValue(end));
    scene.insert("speed_limit", limit);
    scene.insert("speed_scale", scale);
    scene.insert("speed_position", QJsonArray{x,y});
    scene.insert("speed_transparent", setup.value("transparent").toBool());
    root_.insert("composition", scene);
}

QVariantMap ProjectDocument::moviePreferences() const
{
    const auto scene = root_.value("composition").toObject();
    return {{"speed", scene.value("play_rate").toDouble(1.)}, {"audio", scene.value("audio_id").toString()}};
}

void ProjectDocument::setMoviePreferences(double rate, const QString &audioId)
{
    auto scene = root_.value("composition").toObject();
    scene.insert("play_rate", rate);
    scene.insert("audio_id", audioId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(audioId));
    root_.insert("composition", scene);
}

bool ProjectDocument::comparePlans() const
{
    return root_.value("composition").toObject().value("compare_plans").toBool(false);
}

void ProjectDocument::setComparePlans(bool compare)
{
    auto composition = root_.value("composition").toObject();
    composition.insert("compare_plans", compare);
    root_.insert("composition", composition);
}

void ProjectDocument::setValue(const QString &key, const QString &value)
{
    auto settings = values();
    settings.insert(key, value);
    root_.insert("values", settings);
}

namespace {
void validateSceneLayer(const QJsonObject &layer, const QString &kind)
{
    const auto start = layer.value("start").toDouble(-1.);
    const auto end = layer.value("end").toDouble(-1.);
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0. || end <= start)
        fail(QStringLiteral("图层结束时间须晚于非负的开始时间。"));
    if (kind == "texts") {
        const auto text = layer.value("text").toString();
        const int size = layer.value("size").toInt(0);
        if (text.trimmed().isEmpty() || text.toUcs4().size() > 160)
            fail(QStringLiteral("请输入 1–160 个字符。"));
        if (size < 12 || size > 120 || !QColor(layer.value("color").toString()).isValid())
            fail(QStringLiteral("字号应为 12–120，并填写有效颜色。"));
    } else {
        const auto width = layer.value("width").toDouble();
        if (width < .05 || width > 1.)
            fail(QStringLiteral("图片宽度须为 5–100%。"));
    }

    const auto position = layer.value("position").toArray();
    if (position.size() != 2 || std::any_of(position.begin(), position.end(), [](const auto &coordinate) {
            const double value = coordinate.toDouble(-1.);
            return !std::isfinite(value) || value < 0. || value > 1.;
        }))
        fail(QStringLiteral("图层位置须为 0–100%。"));
}
}

QString ProjectDocument::cameraKind() const
{
    const auto nativeCamera = values().value("native_camera").toString();
    if (nativeCamera == "overview")
        return nativeCamera;
    return root_.value("composition").toObject().value("main_view").toObject().value("kind").toString();
}

void ProjectDocument::setCameraKind(const QString &kind)
{
    setValue(QStringLiteral("native_camera"), kind == "overview" ? kind : QString());
    if (kind != "overview") {
        auto composition = root_.value("composition").toObject();
        auto mainView = composition.value("main_view").toObject();
        mainView.insert("kind", kind);
        composition.insert("main_view", mainView);
        root_.insert("composition", composition);
    }
}

bool ProjectDocument::transparentCanvas() const
{
    return root_.value("composition").toObject().value("transparent_canvas").toBool(false);
}

void ProjectDocument::setTransparentCanvas(bool transparent)
{
    auto composition = root_.value("composition").toObject();
    composition.insert("transparent_canvas", transparent);
    root_.insert("composition", composition);
}

QJsonArray ProjectDocument::layoutCues() const
{
    return root_.value("composition").toObject().value("layouts").toArray();
}

void ProjectDocument::setLayoutCue(double time, const QString &mode, const QString &videoId, double transition)
{
    const bool videoMode = mode == "video" || mode == "backdrop" || mode == "inset";
    const bool validMode = videoMode || mode == "mix" || mode == "cloud";
    if (!std::isfinite(time) || time < 0. || !std::isfinite(transition) ||
        transition < 0. || transition > 5. || !validMode)
        fail(QStringLiteral("布局时间或过渡设置无效。"));
    if (videoMode && videoId.isEmpty())
        fail(QStringLiteral("请先选择一路视频。"));

    auto layouts = layoutCues();
    qsizetype index = 0;
    while (index < layouts.size() && layouts.at(index).toObject().value("time").toDouble() < time - 1e-7)
        ++index;
    const auto previous = index < layouts.size() ? layouts.at(index).toObject() : QJsonObject();
    const bool replace = !previous.isEmpty() && std::abs(previous.value("time").toDouble() - time) < 1e-7;
    auto cue = replace && previous.value("mode").toString() == mode && previous.value("video_id").toString() == videoId
        ? previous : QJsonObject();
    cue.insert("time", time);
    cue.insert("mode", mode);
    cue.insert("transition", transition);
    cue.insert("video_id", videoMode ? QJsonValue(videoId) : QJsonValue(QJsonValue::Null));
    if (replace)
        layouts.replace(index, cue);
    else
        layouts.insert(index, cue);
    auto composition = root_.value("composition").toObject();
    composition.insert("layouts", layouts);
    root_.insert("composition", composition);
}

void ProjectDocument::removeLayoutCue(double time)
{
    auto layouts = layoutCues();
    for (qsizetype index = 0; index < layouts.size(); ++index) {
        if (layouts.at(index).toObject().value("time").toDouble() == time) {
            layouts.removeAt(index);
            auto composition = root_.value("composition").toObject();
            composition.insert("layouts", layouts);
            root_.insert("composition", composition);
            return;
        }
    }
}

QJsonArray ProjectDocument::timelineMarkers() const
{
    return root_.value("composition").toObject().value("markers").toArray();
}

void ProjectDocument::addTimelineMarker(double time)
{
    if (!std::isfinite(time) || time < 0.)
        fail(QStringLiteral("标记时间无效。"));

    auto markers = timelineMarkers();
    QStringList labels;
    qsizetype index = markers.size();
    for (qsizetype i = 0; i < markers.size(); ++i) {
        const auto marker = markers.at(i).toObject();
        const auto stamp = marker.value("time").toDouble(-1.);
        if (std::abs(stamp - time) < 1e-6)
            fail(QStringLiteral("当前位置已有标记。"));
        if (stamp > time && index == markers.size())
            index = i;
        labels.append(marker.value("label").toString());
    }

    int number = 1;
    while (labels.contains(QStringLiteral("标记 %1").arg(number)))
        ++number;
    markers.insert(index, QJsonObject{{"time", time}, {"label", QStringLiteral("标记 %1").arg(number)}});
    auto composition = root_.value("composition").toObject();
    composition.insert("markers", markers);
    root_.insert("composition", composition);
}

void ProjectDocument::replaceTimelineMarker(double time, const std::optional<QString> &label)
{
    auto markers = timelineMarkers();
    for (qsizetype index = 0; index < markers.size(); ++index) {
        auto marker = markers.at(index).toObject();
        if (marker.value("time").toDouble(-1.) != time)
            continue;

        if (label) {
            const auto name = label->trimmed();
            if (name.isEmpty() || name.toUcs4().size() > 48)
                fail(QStringLiteral("标记名称须为 1–48 个字符。"));
            marker.insert("label", name);
            markers.replace(index, marker);
        } else {
            markers.removeAt(index);
        }
        auto composition = root_.value("composition").toObject();
        composition.insert("markers", markers);
        root_.insert("composition", composition);
        return;
    }
    fail(QStringLiteral("找不到所选标记。"));
}

QVariantList ProjectDocument::sceneLayers() const
{
    const auto composition = root_.value("composition").toObject();
    QVariantList result;
    for (const auto &kind : {QStringLiteral("images"), QStringLiteral("texts")}) {
        const auto layers = composition.value(kind).toArray();
        for (const auto &value : layers) {
            const auto layer = value.toObject();
            auto view = layer.toVariantMap();
            const auto source = absoluteMedia(layer.value("path").toString());
            const auto position = layer.value("position").toArray();
            view.insert("kind", kind);
            view.insert("url", source.isEmpty() ? QUrl() : QUrl::fromLocalFile(source));
            view.insert("label", kind == "images" ? QFileInfo(source).fileName() : layer.value("text").toString().simplified());
            view.insert("horizontal", position.size() == 2 ? position[0].toDouble() : .05);
            view.insert("vertical", position.size() == 2 ? position[1].toDouble() : .08);
            view.insert("size", layer.value("size").toInt(32));
            view.insert("color", layer.value("color").toString("#f7faff"));
            view.insert("width", layer.value("width").toDouble(.28));
            view.insert("transparent", layer.value("transparent").toBool(true));
            result.append(view);
        }
    }
    return result;
}

void ProjectDocument::addSceneLayer(const QString &kind, const QString &content, double start, double end)
{
    QJsonObject layer{{"id", QUuid::createUuid().toString(QUuid::Id128)},
                      {"start", start}, {"end", end}, {"transparent", true}};
    if (kind == "images") {
        if (!QFileInfo(content).isFile())
            fail(QStringLiteral("找不到图片：%1").arg(content));
        layer.insert("path", path_.isEmpty() ? absolutePath(content)
                                             : referenceFrom(QFileInfo(path_).absolutePath(), absolutePath(content)));
        layer.insert("position", QJsonArray{.94, .08});
        layer.insert("width", .28);
    } else if (kind == "texts") {
        layer.insert("text", content);
        layer.insert("position", QJsonArray{.05, .08});
        layer.insert("size", 32);
        layer.insert("color", "#f7faff");
    } else {
        fail(QStringLiteral("未知的合成图层类型。"));
    }
    validateSceneLayer(layer, kind);
    auto composition = root_.value("composition").toObject();
    auto layers = composition.value(kind).toArray();
    layers.append(layer);
    composition.insert(kind, layers);
    root_.insert("composition", composition);
}

void ProjectDocument::editSceneLayer(const QString &id, const QJsonObject &changes)
{
    auto composition = root_.value("composition").toObject();
    for (const auto &kind : {QStringLiteral("images"), QStringLiteral("texts")}) {
        auto layers = composition.value(kind).toArray();
        for (qsizetype index = 0; index < layers.size(); ++index) {
            auto layer = layers.at(index).toObject();
            if (layer.value("id").toString() != id)
                continue;
            for (auto change = changes.begin(); change != changes.end(); ++change)
                layer.insert(change.key(), change.value());
            validateSceneLayer(layer, kind);
            layers.replace(index, layer);
            composition.insert(kind, layers);
            root_.insert("composition", composition);
            return;
        }
    }
    fail(QStringLiteral("找不到所选图层。"));
}

void ProjectDocument::removeSceneLayer(const QString &id)
{
    auto composition = root_.value("composition").toObject();
    for (const auto &kind : {QStringLiteral("images"), QStringLiteral("texts")}) {
        auto layers = composition.value(kind).toArray();
        for (qsizetype index = 0; index < layers.size(); ++index) {
            if (layers.at(index).toObject().value("id").toString() == id) {
                layers.removeAt(index);
                composition.insert(kind, layers);
                root_.insert("composition", composition);
                return;
            }
        }
    }
}

RvizLayout ProjectDocument::topicLayout() const
{
    const auto composition = root_.value("composition").toObject();
    RvizLayout layout;
    layout.fixedFrame = composition.value("display_fixed_frame").toString();
    const auto displays = composition.value("topic_displays").toArray();
    for (const auto &display : displays)
        layout.displays.push_back(TopicDisplay::fromJson(display.toObject()));
    return layout;
}

void ProjectDocument::setTopicLayout(const RvizLayout &layout)
{
    auto composition = root_.value("composition").toObject();
    QJsonArray displays;
    bool hasCloud = false;
    for (const auto &display : layout.displays) {
        displays.append(display.toJson());
        hasCloud = hasCloud || (display.enabled && display.kind == "cloud");
    }
    composition.insert("topic_displays", displays);
    composition.insert("display_fixed_frame", layout.fixedFrame);
    composition.insert("show_main_cloud", !hasCloud);
    root_.insert("composition", composition);
}

std::vector<VideoClip> ProjectDocument::videos() const
{
    std::vector<VideoClip> result;
    const auto layers = root_.value("composition").toObject().value("videos").toArray();
    result.reserve(layers.size());
    for (const auto &entry : layers) {
        const auto item = entry.toObject();
        const auto path = absoluteMedia(item.value("path").toString());
        const auto position = item.value("position").toArray();
        VideoClip clip;
        clip.path = path;
        clip.id = item.value("id").toString();
        clip.label = QFileInfo(path).fileName();
        clip.timing = VideoTiming::fromJson(item.value("edit").toObject());
        clip.width = std::clamp(item.value("width").toDouble(clip.width), .05, .90);
        clip.opacity = std::clamp(item.value("opacity").toDouble(clip.opacity), 0., 1.);
        clip.backdrop = item.value("backdrop").toBool(false);
        clip.backdropScale = std::clamp(item.value("backdrop_scale").toDouble(1.), .2, 2.);
        const auto backdropPosition = item.value("backdrop_position").toArray();
        if (backdropPosition.size() == 2) {
            clip.backdropX = std::clamp(backdropPosition[0].toDouble(.5), 0., 1.);
            clip.backdropY = std::clamp(backdropPosition[1].toDouble(.5), 0., 1.);
        }
        if (position.size() == 2) {
            clip.horizontal = std::clamp(position[0].toDouble(clip.horizontal), 0., 1.);
            clip.vertical = std::clamp(position[1].toDouble(clip.vertical), 0., 1.);
        }
        result.push_back(std::move(clip));
    }
    return result;
}

void ProjectDocument::addVideo(const QString &file)
{
    if (!QFileInfo(file).isFile())
        fail(QStringLiteral("找不到实拍视频：%1").arg(file));
    auto scene = root_.value("composition").toObject();
    auto layers = scene.value("videos").toArray();
    QJsonObject edit{{"offset", 0.}, {"trim_in", 0.}, {"trim_out", QJsonValue(QJsonValue::Null)},
                     {"crop", QJsonArray{0., 0., 1., 1.}}, {"anchors", QJsonArray{}}};
    const auto anchor = layers.size() % 4;
    layers.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::Id128)},
                              {"path", path_.isEmpty() ? absolutePath(file)
                                                       : referenceFrom(QFileInfo(path_).absolutePath(), absolutePath(file))},
                              {"edit", edit}, {"width", .28}, {"opacity", .96},
                              {"position", QJsonArray{anchor % 2 ? .94 : .055, anchor / 2 ? .52 : .84}}});
    scene.insert("videos", layers);
    root_.insert("composition", scene);
}

QJsonObject ProjectDocument::videoTiming(qsizetype index) const
{
    const auto layers = root_.value("composition").toObject().value("videos").toArray();
    if (index < 0 || index >= layers.size())
        fail(QStringLiteral("视频图层已失效。"));
    return layers[index].toObject().value("edit").toObject();
}

void ProjectDocument::setVideoTiming(qsizetype index, const QJsonObject &changes)
{
    auto edit = videoTiming(index);
    for (auto it = changes.begin(); it != changes.end(); ++it)
        edit.insert(it.key(), it.value());
    replaceVideoTiming(index, edit);
}

void ProjectDocument::replaceVideoTiming(qsizetype index, const QJsonObject &edit)
{
    auto scene = root_.value("composition").toObject();
    auto layers = scene.value("videos").toArray();
    if (index < 0 || index >= layers.size())
        fail(QStringLiteral("视频图层已失效。"));
    VideoTiming::fromJson(edit);
    auto layer = layers[index].toObject();
    layer.insert("edit", edit);
    layers.replace(index, layer);
    scene.insert("videos", layers);
    root_.insert("composition", scene);
}

QString ProjectDocument::absoluteMedia(const QString &reference) const
{
    if (reference.isEmpty())
        return {};
    const auto directory = path_.isEmpty() ? QDir::currentPath() : QFileInfo(path_).absolutePath();
    return absolutePath(QDir(directory).absoluteFilePath(QDir::fromNativeSeparators(reference)));
}

QJsonObject ProjectDocument::remapMedia(const std::function<QString(const QString &)> &map) const
{
    auto root = root_;
    for (const auto &key : {QStringLiteral("bag"), QStringLiteral("extrinsics")}) {
        const auto item = root.value(key);
        if (item.isUndefined() || item.isNull())
            continue;
        if (!item.isString())
            fail(QStringLiteral("项目素材路径必须为文本：%1").arg(key));
        if (!item.toString().isEmpty())
            root.insert(key, map(absoluteMedia(item.toString())));
    }

    auto scene = root.value("composition").toObject();
    for (const auto &kind : {QStringLiteral("videos"), QStringLiteral("images")}) {
        if (!scene.contains(kind))
            continue;
        if (!scene.value(kind).isArray())
            fail(QStringLiteral("项目素材列表无效：%1").arg(kind));
        QJsonArray layers;
        for (const auto &entry : scene.value(kind).toArray()) {
            auto layer = entry.toObject();
            const auto path = layer.value("path").toString();
            if (path.isEmpty())
                fail(QStringLiteral("项目图层缺少路径：%1").arg(kind));
            layer.insert("path", map(absoluteMedia(path)));
            layers.append(layer);
        }
        scene.insert(kind, layers);
    }
    root.insert("composition", scene);
    return root;
}

QStringList ProjectDocument::media() const
{
    QStringList paths;
    remapMedia([&paths](const QString &path) {
        paths.append(path);
        return path;
    });
    paths.removeDuplicates();
    return paths;
}

ProjectDocument ProjectDocument::saveAs(const QString &manifest) const
{
    const auto target = absolutePath(manifest);
    if (media().contains(target,
#ifdef Q_OS_WIN
                         Qt::CaseInsensitive
#else
                         Qt::CaseSensitive
#endif
                         ))
        fail(QStringLiteral("项目文件不能覆盖素材。"));
    const auto directory = QFileInfo(target).absolutePath();
    auto root = remapMedia([&directory](const QString &path) { return referenceFrom(directory, path); });
    if (!isPortable() || QFileInfo(path_).absolutePath() != directory ||
        QFileInfo(target).fileName() != QStringLiteral("project.slamfpv"))
        root.remove("portable");
    ProjectDocument saved(target, root);
    writeManifest(target, root);
    return saved;
}

QString ProjectDocument::exportFolder(const QString &folder, const std::function<bool()> &cancel) const
{
    const QFileInfo target(folder);
    const auto parentPath = QFileInfo(target.absolutePath()).canonicalFilePath();
    if (parentPath.isEmpty() || target.fileName().isEmpty() || target.exists() || target.isSymLink())
        fail(QStringLiteral("请为打包项目选择一个尚不存在的文件夹。"));
    QDir parent(parentPath);
    QTemporaryDir stage(parent.filePath(".planner-replay-XXXXXX"));
    if (!stage.isValid())
        fail(QStringLiteral("无法在目标位置创建项目临时目录。"));
    QDir root(stage.path());
    for (const auto &directory : {QStringLiteral("media"), QStringLiteral("cache"), QStringLiteral("exports")}) {
        if (!root.mkdir(directory))
            fail(QStringLiteral("无法创建项目目录：%1").arg(directory));
    }
    QHash<QString, QString> copied;
    auto project = remapMedia([&](const QString &path) {
        if (copied.contains(path))
            return copied.value(path);
        const auto slot = QStringLiteral("media/asset-%1").arg(copied.size() + 1);
        const auto relative = slot + "/" + QFileInfo(path).fileName();
        if (!root.mkdir(slot))
            fail(QStringLiteral("无法创建素材目录：%1").arg(slot));
        copyMedia(path, root.filePath(relative), cancel);
        copied.insert(path, relative);
        return relative;
    });
    project.insert("portable", true);
    writeManifest(root.filePath("project.slamfpv"), project);
    if (cancel && cancel())
        fail(QStringLiteral("已取消项目打包。"));
    if (!parent.rename(QFileInfo(stage.path()).fileName(), target.fileName()))
        fail(QStringLiteral("无法发布项目文件夹：%1").arg(folder));
    stage.setAutoRemove(false);
    return parent.filePath(target.fileName() + "/project.slamfpv");
}
}
