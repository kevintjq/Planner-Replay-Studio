#include "project_document.h"
#include "design_tokens.h"
#include <QColor>
#include <QCryptographicHash>

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

void updateCameraOptions(QJsonObject &view, const QVariantMap &settings)
{
    for (const auto &name : {"targetFrame", "name"}) if (settings.contains(name))
        view.insert(name, settings.value(name).toString());
    for (const auto &name : {"followLag", "smoothing", "pointSize"}) if (settings.contains(name)) {
        bool ok = false; const auto value = settings.value(name).toDouble(&ok);
        const double minimum = QString(name) == "pointSize" ? .25 : 0.;
        const double maximum = QString(name) == "pointSize" ? 20. : 10.;
        if (!ok || !std::isfinite(value) || value < minimum || value > maximum)
            fail(QStringLiteral("相机 %1 超出允许范围。").arg(name));
        view.insert(name, value);
    }
    if (settings.contains("followMode")) {
        bool ok = false; const auto value = settings.value("followMode").toDouble(&ok);
        if (!ok || value < 0 || value > 3 || std::floor(value) != value)
            fail(QStringLiteral("无效 TF 跟随模式。"));
        view.insert("followMode", int(value));
    }
    for (const auto &name : {"opticalFrame", "cloudVisible"}) if (settings.contains(name))
        view.insert(name, settings.value(name).toBool());
    for (const auto &name : {"cloudColor", "pathColor", "viewBackground"}) if (settings.contains(name)) {
        const auto value = settings.value(name).toString();
        if (!value.isEmpty() && !QColor(value).isValid()) fail(QStringLiteral("无效的机位颜色：%1").arg(name));
        view.insert(name, value);
    }
    if (settings.contains("hiddenDisplays")) view.insert("hiddenDisplays", QJsonArray::fromStringList(settings.value("hiddenDisplays").toStringList()));
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
    const auto backend = root.value("composition").toObject().value("render_backend");
    if (!backend.isUndefined() && (!backend.isString() ||
        (backend.toString() != "independent" && backend.toString() != "native-rviz")))
        fail(QStringLiteral("项目渲染后端须为 independent 或 native-rviz。"));
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

QVariantMap sourceMetadata(QVariantMap value)
{
    // These are derived from the current graph/file system, never project truth.
    for (const auto &key : {"references", "referenceCount", "url", "exists", "size"}) value.remove(key);
    return value;
}

bool sameMediaPath(const QString &a, const QString &b)
{
    return a.compare(b,
#ifdef Q_OS_WIN
                     Qt::CaseInsensitive
#else
                     Qt::CaseSensitive
#endif
                     ) == 0;
}

QVariantMap remapCanvasImages(QVariantMap canvas,const std::function<QString(const QString &)> &map)
{
    auto elements=canvas.value("elements").toList();auto items=canvas.value("items").toMap();
    for(auto &v:elements){auto row=v.toMap();if(row.value("kind")!="image")continue;
        const auto remap=[&](QVariantMap value){
            const auto source=value.value("source").toString();if(source.isEmpty())return value;
            const QUrl url(source);const auto path=QDir::isAbsolutePath(source)?source:url.isLocalFile()?url.toLocalFile():url.scheme().isEmpty()?source:QString();
            if(path.isEmpty())fail(QStringLiteral("科研图片必须引用本地文件。"));value["source"]=map(path);return value;
        };
        row=remap(row);v=row;const auto id=row.value("id").toString();if(items.value(id).toMap().contains("source"))items[id]=remap(items.value(id).toMap());
    }
    if(canvas.contains("elements"))canvas["elements"]=elements;if(canvas.contains("items"))canvas["items"]=items;return canvas;
}

QVariantMap removeCanvasEntries(QVariantMap canvas,const QStringList &ids)
{
    auto elements=canvas.value("elements").toList();const auto count=elements.size();
    elements.removeIf([&](const QVariant &v){return ids.contains(v.toMap().value("id").toString());});
    if(elements.size()!=count)canvas["elements"]=elements;
    auto items=canvas.value("items").toMap();const auto itemCount=items.size();for(const auto &id:ids)items.remove(id);
    if(items.size()!=itemCount)canvas["items"]=items;return canvas;
}
}

ProjectDocument::ProjectDocument(QString path, QJsonObject root)
    : path_(std::move(path)), root_(std::move(root))
{
    validateRoot(root_);
    media(); // Validate asset references before changing the active project.
    // Migrate the previously separate scene/RViz backgrounds once on load.
    // An explicit project scene choice wins over an older imported setting.
    auto composition = root_.value("composition").toObject();
    if (!composition.contains("display_background_color")) {
        auto color = QColor(composition.value("scene_background").toString());
        if (!color.isValid()) color = rvizBackgroundColor(composition.value("rviz_source_yaml").toString());
        if (color.isValid() && color.alpha() == 255) {
            composition["display_background_color"] = color.name();
            composition["scene_background"] = color.name();
            root_["composition"] = composition;
        }
    }
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

ProjectDocument ProjectDocument::create(const QString &source, const QString &bag, const QString &extrinsics)
{
    QJsonObject scene{{"main_view", QJsonObject{{"kind", "rear"}, {"width", 1.}, {"position", QJsonArray{.5, .5}}}},
                      {"views", QJsonArray{}}, {"videos", QJsonArray{}}, {"texts", QJsonArray{}}};
    return ProjectDocument({}, QJsonObject{{"version", 1}, {"source", source},
        {"bag", bag.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(absolutePath(bag))},
        {"extrinsics", extrinsics.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(absolutePath(extrinsics))}, {"values", QJsonObject{}},
        {"enabled", QJsonObject{}}, {"composition", scene}});
}

QString ProjectDocument::source() const { return root_.value("source").toString(); }
QVariantMap ProjectDocument::canvasState() const
{
    return remapCanvasImages(composition().value("canvas_layers").toObject().toVariantMap(),[this](const QString &path){return absoluteMedia(path);});
}
void ProjectDocument::setCanvasState(const QVariantMap &state)
{
    const auto existing=composition().value("canvas_layers").toObject().toVariantMap();
    if(!existing.value("elements").toList().isEmpty())retainSourceCatalog();
    const auto normalized=remapCanvasImages(state,[this](const QString &path){const auto absolute=absoluteMedia(path);return path_.isEmpty()?absolute:referenceFrom(QFileInfo(path_).absolutePath(),absolute);});
    auto scene=composition();scene["canvas_layers"]=QJsonObject::fromVariantMap(normalized);setComposition(scene);
}
QString ProjectDocument::bag() const { return absoluteMedia(root_.value("bag").toString()); }
void ProjectDocument::setBagSource(const QString &file)
{
    if (!QFileInfo(file).isFile()) fail(QStringLiteral("录包文件不存在。 / Bag file does not exist."));
    const auto previous = bag();
    if (!previous.isEmpty()) {
        for (const auto &value : sources()) {
            const auto source = value.toMap();
            if (source.value("kind") == "bag" && sameMediaPath(source.value("path").toString(), previous)) {
                replaceSource(source.value("id").toString(), file); root_["source"] = "bag"; return;
            }
        }
    }
    const auto absolute = absolutePath(file);
    root_["bag"] = path_.isEmpty() ? absolute : referenceFrom(QFileInfo(path_).absolutePath(), absolute);
    root_["source"] = "bag";
}
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

QString ProjectDocument::sceneBackground() const
{
    const auto value = root_.value("composition").toObject().value("scene_background").toString();
    return QColor::isValidColorName(value) ? value : QString::fromLatin1(DesignTokens::SceneBackground);
}

void ProjectDocument::setSceneBackground(const QString &color)
{
    const QColor value(color);
    if (!value.isValid() || value.alpha() != 255)
        fail(QStringLiteral("场景背景需要有效的不透明颜色。"));
    auto scene = root_.value("composition").toObject();
    scene.insert("scene_background", value.name());
    scene.insert("display_background_color", value.name());
    root_.insert("composition", scene);
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
            {"transparent", scene.value("speed_transparent").toBool(true)},
            {"scheme", scene.value("speed_scheme").toString("dark")}};
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
    scene.insert("speed_scheme", setup.value("scheme").toString() == "light" ? "light" : "dark");
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

QString ProjectDocument::renderBackend() const
{
    return composition().value("render_backend").toString(QStringLiteral("independent"));
}

void ProjectDocument::setRenderBackend(const QString &backend)
{
    if (backend != "independent" && backend != "native-rviz")
        fail(QStringLiteral("渲染后端须为 independent 或 native-rviz。"));
    auto scene = composition();
    scene.insert("render_backend", backend);
    setComposition(scene);
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

QVariantMap ProjectDocument::mainView() const
{
    const auto view = root_.value("composition").toObject().value("main_view").toObject();
    const auto position = view.value("position").toArray();
    auto fields = view.toVariantMap();
    fields.insert("width", view.value("width").toDouble(1.));
    fields.insert("x", position.size() == 2 ? position[0].toDouble(.5) : .5);
    fields.insert("y", position.size() == 2 ? position[1].toDouble(.5) : .5);
    fields.insert("zoom", view.value("zoom").toDouble(1.));
    return fields;
}

void ProjectDocument::setMainView(const QVariantMap &settings)
{
    auto scene = root_.value("composition").toObject();
    auto view = scene.value("main_view").toObject();
    updateCameraOptions(view, settings);
    scene.insert("main_view", view); root_.insert("composition", scene);
}

void ProjectDocument::setMainViewWidth(double width)
{
    if (!std::isfinite(width) || width < .12 || width > 1.)
        fail(QStringLiteral("请检查主点云常态宽度。"));
    auto scene = root_.value("composition").toObject();
    auto view = scene.value("main_view").toObject();
    view.insert("width", width);
    if (!view.contains("kind"))
        view.insert("kind", QStringLiteral("rear"));
    scene.insert("main_view", view);
    root_.insert("composition", scene);
}

QJsonArray ProjectDocument::layerMotions() const
{
    return root_.value("composition").toObject().value("motions").toArray();
}

void ProjectDocument::addLayerMotion(const QString &kind, const QString &id, double begin, double time, double duration,
                                    double width, double x, double y)
{
    if (!std::isfinite(begin) || !std::isfinite(time) || !std::isfinite(duration) || !std::isfinite(width) ||
        !std::isfinite(x) || !std::isfinite(y) || begin < 0. || time < begin || duration < .1 || duration > 5. ||
        width < .05 || width > 1. || x < 0. || x > 1. || y < 0. || y > 1.)
        fail(QStringLiteral("请检查缩窗时间、位置与尺寸。"));

    const bool independent = kind == "main" || kind == "speed";
    auto scene = root_.value("composition").toObject();
    if (!independent) {
        const QString listName = kind == "video" ? QStringLiteral("videos") : kind == "view" ? QStringLiteral("views")
            : kind == "image" ? QStringLiteral("images") : kind == "text" ? QStringLiteral("texts") : QString();
        const auto layers = scene.value(listName).toArray();
        if (id.isEmpty() || std::none_of(layers.begin(), layers.end(), [&](const auto &entry) {
                return entry.toObject().value("id").toString() == id;
            }))
            fail(QStringLiteral("找不到缩窗动画指定的图层。"));
    }
    auto motions = layerMotions();
    for (const auto &entry : motions) {
        const auto old = entry.toObject();
        if (old.value("kind").toString() != kind || old.value("layer_id").toString() != id)
            continue;
        if (old.value("start").toDouble() < time + duration - 1e-8 &&
            begin < old.value("time").toDouble() + old.value("duration").toDouble(.4) - 1e-8)
            fail(QStringLiteral("同一图层的缩窗动画不能重叠。"));
    }
    motions.append(QJsonObject{{"kind", kind}, {"layer_id", independent ? QJsonValue(QJsonValue::Null) : QJsonValue(id)},
        {"start", begin}, {"time", time}, {"duration", duration}, {"large_width", width}, {"large_position", QJsonArray{x,y}}});
    scene.insert("motions", motions);
    root_.insert("composition", scene);
}

void ProjectDocument::removeLayerMotion(qsizetype index)
{
    auto motions = layerMotions();
    if (index < 0 || index >= motions.size())
        return;
    motions.removeAt(index);
    auto scene = root_.value("composition").toObject();
    scene.insert("motions", motions);
    root_.insert("composition", scene);
}

QVariantList ProjectDocument::cloudViews() const
{
    const auto views = root_.value("composition").toObject().value("views").toArray();
    QVariantList result;
    for (const auto &entry : views) {
        const auto view = entry.toObject();
        const auto position = view.value("position").toArray();
        auto fields = view.toVariantMap();
        fields.insert("x", position.size() == 2 ? position[0].toDouble(.97) : .97);
        fields.insert("y", position.size() == 2 ? position[1].toDouble(.05) : .05);
        fields.insert("zoom", view.value("zoom").toDouble(1.));
        fields.insert("width", view.value("width").toDouble(.32));
        fields.insert("transparent", view.value("transparent").toBool(true));
        if (!view.value("overlays").isArray()) {
            const auto kind = view.value("kind").toString();
            fields.insert("overlays", kind == "fpv" || kind == "fpv_raw" ? QStringList{"scan"}
                         : QStringList{"actual", "plan", "goal", "corridor", "scan", "camera"});
        }
        result.append(fields);
    }
    return result;
}

void ProjectDocument::addCloudView(const QString &kind)
{
    auto scene = root_.value("composition").toObject();
    auto views = scene.value("views").toArray();
    const auto row = views.size();
    views.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"kind", kind},
                             {"width", .32}, {"zoom", 1.}, {"transparent", true},
                             {"position", QJsonArray{row % 2 ? .03 : .97, row % 3 == 0 ? .05 : row % 3 == 1 ? .94 : .52}}});
    scene.insert("views", views);
    root_.insert("composition", scene);
}

void ProjectDocument::setCloudView(const QString &id, const QVariantMap &settings)
{
    auto scene = root_.value("composition").toObject();
    auto views = scene.value("views").toArray();
    for (qsizetype i = 0; i < views.size(); ++i) {
        auto view = views[i].toObject();
        if (view.value("id").toString() != id)
            continue;
        const auto oldPosition = view.value("position").toArray();
        const auto number = [&](const char *name, double fallback, double low, double high) {
            bool ok = true;
            const double value = settings.value(QLatin1StringView(name), fallback).toDouble(&ok);
            if (!ok || !std::isfinite(value) || value < low || value > high)
                fail(QStringLiteral("机位位置、宽度或相机缩放超出可用范围。"));
            return value;
        };
        const double x = number("x", oldPosition.size() == 2 ? oldPosition[0].toDouble(.97) : .97, 0., 1.);
        const double y = number("y", oldPosition.size() == 2 ? oldPosition[1].toDouble(.05) : .05, 0., 1.);
        view.insert("position", QJsonArray{x,y});
        view.insert("width", number("width", view.value("width").toDouble(.32), .12, 1.5));
        view.insert("zoom", number("zoom", view.value("zoom").toDouble(1.), .5, 4.));
        if (settings.contains("transparent"))
            view.insert("transparent", settings.value("transparent").toBool());
        if (settings.contains("overlays"))
            view.insert("overlays", QJsonArray::fromStringList(settings.value("overlays").toStringList()));
        updateCameraOptions(view, settings);
        views.replace(i, view);
        scene.insert("views", views);
        root_.insert("composition", scene);
        return;
    }
}

void ProjectDocument::removeCloudView(const QString &id)
{
    const auto views = composition().value("views").toArray();
    if (!id.isEmpty() && std::any_of(views.begin(), views.end(), [&](const auto &v) { return v.toObject().value("id").toString() == id; }))
        removeCanvasLayers({id});
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
    if (id.isEmpty()) return;
    for (const auto &kind : {QStringLiteral("images"), QStringLiteral("texts")}) {
        const auto layers = composition().value(kind).toArray();
        if (std::any_of(layers.begin(), layers.end(), [&](const auto &v) { return v.toObject().value("id").toString() == id; })) {
            removeCanvasLayers({id}); return;
        }
    }
}

RvizLayout ProjectDocument::topicLayout() const
{
    const auto composition = root_.value("composition").toObject();
    RvizLayout layout;
    layout.fixedFrame = composition.value("display_fixed_frame").toString();
    layout.sourceYaml = composition.value("rviz_source_yaml").toString();
    layout.backgroundColor = QColor(composition.value("display_background_color").toString());
    for (const auto &name : composition.value("rviz_unrendered_plugins").toArray()) layout.skipped.append(name.toString());
    const auto displays = composition.value("topic_displays").toArray();
    for (const auto &display : displays)
        layout.displays.push_back(TopicDisplay::fromJson(display.toObject()));
    layout.useInstanceModel = composition.value("independent_display_model").toBool(false);
    for (const auto &display : composition.value("independent_displays").toArray())
        layout.instances.push_back(TopicDisplay::fromJson(display.toObject()));
    return layout;
}

void ProjectDocument::setTopicLayout(const RvizLayout &layout)
{
    auto composition = root_.value("composition").toObject();
    QJsonArray displays;
    bool hasCloud = false;
    for (const auto &display : layout.displays) {
        displays.append(display.toJson());
        hasCloud = hasCloud || display.kind == "cloud";
    }
    composition.insert("topic_displays", displays);
    composition.insert("display_fixed_frame", layout.fixedFrame);
    composition.insert("rviz_source_yaml", layout.sourceYaml);
    if (layout.backgroundColor.isValid() && layout.backgroundColor.alpha() == 255) {
        composition.insert("display_background_color", layout.backgroundColor.name());
        composition.insert("scene_background", layout.backgroundColor.name());
    } else {
        const QColor sceneColor(composition.value("scene_background").toString());
        if (sceneColor.isValid() && sceneColor.alpha() == 255) composition.insert("display_background_color", sceneColor.name());
        else composition.remove("display_background_color");
    }
    composition.insert("rviz_unrendered_plugins", QJsonArray::fromStringList(layout.skipped));
    composition.insert("independent_display_model", layout.useInstanceModel);
    QJsonArray instances;
    for (const auto &display : layout.instances) instances.append(display.toJson());
    composition.insert("independent_displays", instances);
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

QJsonObject ProjectDocument::trajectorySettings() const
{
    auto settings = root_.value("trajectory_studio").toObject();
    if (!settings.isEmpty()) {
        settings.insert("path", absoluteMedia(settings.value("path").toString()));
        if(!settings.value("background_file").toString().isEmpty())settings["background_file"]=absoluteMedia(settings.value("background_file").toString());
        if(settings.contains("canvas"))settings["canvas"]=QJsonObject::fromVariantMap(remapCanvasImages(settings.value("canvas").toObject().toVariantMap(),[this](const QString &path){return absoluteMedia(path);}));
    }
    return settings;
}

void ProjectDocument::setTrajectorySettings(const QJsonObject &settings)
{
    if (settings.isEmpty()) {
        root_.remove("trajectory_studio");
        return;
    }
    auto saved = settings;
    const auto source = settings.value("path").toString();
    if (source.isEmpty())
        fail(QStringLiteral("轨迹图设置缺少源视频路径。"));
    saved.insert("path", path_.isEmpty() ? absolutePath(source)
        : referenceFrom(QFileInfo(path_).absolutePath(), absolutePath(source)));
    const auto reference=[this](const QString &path){const auto absolute=absoluteMedia(path);return path_.isEmpty()?absolute:referenceFrom(QFileInfo(path_).absolutePath(),absolute);};
    if(!saved.value("background_file").toString().isEmpty())saved["background_file"]=reference(saved.value("background_file").toString());
    if(saved.contains("canvas"))saved["canvas"]=QJsonObject::fromVariantMap(remapCanvasImages(saved.value("canvas").toObject().toVariantMap(),reference));
    if(!root_.value("trajectory_studio").toObject().value("canvas").toObject().value("elements").toArray().isEmpty())retainSourceCatalog();
    root_.insert("trajectory_studio", saved);
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
    for (const auto &kind : {QStringLiteral("videos"), QStringLiteral("images"), QStringLiteral("audio")}) {
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
    if(scene.contains("canvas_layers"))scene["canvas_layers"]=QJsonObject::fromVariantMap(remapCanvasImages(scene.value("canvas_layers").toObject().toVariantMap(),[&](const QString &path){return map(absoluteMedia(path));}));
    root.insert("composition", scene);
    if (root.contains("sources")) {
        QJsonArray sources;
        for (const auto &value : root.value("sources").toArray()) {
            auto source = QJsonObject::fromVariantMap(sourceMetadata(value.toObject().toVariantMap()));
            if (!source.value("path").toString().isEmpty()) source["path"] = map(absoluteMedia(source.value("path").toString()));
            sources.append(source);
        }
        root["sources"] = sources;
    }
    if (root.contains("trajectory_studio")) {
        auto trajectory = root.value("trajectory_studio").toObject();
        if (!trajectory.value("path").isString() || trajectory.value("path").toString().isEmpty())
            fail(QStringLiteral("轨迹图项目源视频路径无效。"));
        trajectory.insert("path", map(absoluteMedia(trajectory.value("path").toString())));
        if(!trajectory.value("background_file").toString().isEmpty())trajectory["background_file"]=map(absoluteMedia(trajectory.value("background_file").toString()));
        if(trajectory.contains("canvas"))trajectory["canvas"]=QJsonObject::fromVariantMap(remapCanvasImages(trajectory.value("canvas").toObject().toVariantMap(),[&](const QString &path){return map(absoluteMedia(path));}));
        root.insert("trajectory_studio", trajectory);
    }
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

QVariantList ProjectDocument::sources() const
{
    QVariantList catalog;
    auto add = [&](const QString &path, const QString &kind, const QVariantMap &reference, const QVariantMap &saved = {}) {
        if (path.isEmpty()) return;
        const QString absolute = absoluteMedia(path);
        auto match = std::find_if(catalog.begin(), catalog.end(), [&](const QVariant &v) {
            return v.toMap().value("path").toString().compare(absolute,
#ifdef Q_OS_WIN
                Qt::CaseInsensitive
#else
                Qt::CaseSensitive
#endif
            ) == 0 && v.toMap().value("kind").toString() == kind;
        });
        QVariantMap item = match == catalog.end() ? saved : match->toMap();
        const QString identity = kind + "\n" + QDir::fromNativeSeparators(absolute)
#ifdef Q_OS_WIN
            .toLower()
#endif
            ;
        if (!item.contains("id")) item["id"] = QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(),QCryptographicHash::Sha256).toHex().left(24));
        item["path"] = absolute; item["url"] = QUrl::fromLocalFile(absolute); item["kind"] = kind;
        if (!item.contains("name")) item["name"] = QFileInfo(absolute).fileName();
        auto references = item.value("references").toList();
        if (!reference.isEmpty()) references.append(reference);
        item["references"] = references; item["referenceCount"] = references.size();
        item["exists"] = QFileInfo(absolute).isFile(); item["size"] = QString::number(QFileInfo(absolute).size());
        if (match == catalog.end()) catalog.append(item); else *match = item;
    };
    for (const auto &v : root_.value("sources").toArray()) {
        const auto s=sourceMetadata(v.toObject().toVariantMap());
        add(s.value("path").toString(),s.value("kind").toString(),{},s);
    }
    if (!bag().isEmpty()) add(bag(),"bag",{{"id","bag-data"},{"name","ROS Bag"},{"kind","bag"}});
    if (!extrinsics().isEmpty()) add(extrinsics(),"calibration",{{"id","extrinsics"},{"name","Calibration"},{"kind","calibration"}});
    const auto scene=composition();
    for (const auto &key : {"videos","images","audio"}) for (const auto &v : scene.value(key).toArray()) {
        const auto item=v.toObject();
        add(item.value("path").toString(),QString(key)=="videos"?"video":QString(key)=="images"?"image":"audio",
            {{"id",item.value("id").toString()},{"name",item.value("label").toString(QFileInfo(item.value("path").toString()).fileName())},{"kind",key}});
    }
    const auto trajectory=root_.value("trajectory_studio").toObject();
    if(!trajectory.isEmpty())add(trajectory.value("path").toString(),"video",{{"id","trajectory-source"},{"name","Trajectory Studio"},{"kind","trajectory"}});
    const auto addCanvas=[&](const QVariantMap &canvas,const QString &workspace){
        const auto items=canvas.value("items").toMap();
        for(const auto &v:canvas.value("elements").toList()){auto row=v.toMap();if(row.value("kind")!="image")continue;
            const auto saved=items.value(row.value("id").toString()).toMap();const auto source=saved.value("source",row.value("source")).toString();
            const QUrl url(source);const auto path=url.isLocalFile()?url.toLocalFile():source;
            add(path,"image",{{"id",row.value("id")},{"name",row.value("name",QFileInfo(path).fileName())},{"kind","image"},{"workspace",workspace}});
        }
    };
    addCanvas(scene.value("canvas_layers").toObject().toVariantMap(),"editor");
    addCanvas(trajectory.value("canvas").toObject().toVariantMap(),"trajectory");
    if(!trajectory.value("background_file").toString().isEmpty())add(trajectory.value("background_file").toString(),"image",{{"id","trajectory-background-file"},{"name","Trajectory background"},{"kind","image"}});
    return catalog;
}

void ProjectDocument::retainSourceCatalog()
{
    QJsonArray catalog;
    for(const auto &v:sources()) {
        auto s=sourceMetadata(v.toMap());const auto path=s.value("path").toString();
        s["path"]=path_.isEmpty()?path:referenceFrom(QFileInfo(path_).absolutePath(),path);
        catalog.append(QJsonObject::fromVariantMap(s));
    }
    root_["sources"]=catalog;
}
QString ProjectDocument::addSource(const QString &file,const QString &kind)
{
    if(!QFileInfo(file).isFile())fail(QStringLiteral("素材不存在：%1").arg(file));
    retainSourceCatalog();
    const auto absolute=absolutePath(file);
    for(const auto &v:sources()){const auto s=v.toMap();if(sameMediaPath(s.value("path").toString(),absolute)&&s.value("kind").toString()==kind)return s.value("id").toString();}
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);auto all=root_.value("sources").toArray();
    all.append(QJsonObject{{"id",id},{"kind",kind},{"name",QFileInfo(file).fileName()},
        {"path",path_.isEmpty()?absolute:referenceFrom(QFileInfo(path_).absolutePath(),absolute)}});root_["sources"]=all;return id;
}
void ProjectDocument::removeCanvasLayers(const QStringList &ids)
{
    auto targets=ids;targets.removeAll(QString());targets.removeDuplicates();if(targets.isEmpty())return;
    const auto before=composition();auto scene=before;
    for(const auto &key:{"videos","views","images","texts","audio","speed_meters","plots","annotations","legends","colorbars"}){
        auto array=scene.value(key).toArray();const auto count=array.size();
        for(qsizetype i=array.size();i-->0;)if(targets.contains(array[i].toObject().value("id").toString()))array.removeAt(i);
        if(array.size()!=count)scene[key]=array;
    }
    if(targets.contains("main")&&(scene.contains("main_view")||scene.value("has_main_view").toBool(true))){scene.remove("main_view");scene["has_main_view"]=false;}
    if(targets.contains("speed")&&scene.value("show_speed").toBool())scene["show_speed"]=false;
    auto cues=scene.value("layouts").toArray();
    const auto cueCount=cues.size();
    for(qsizetype i=cues.size();i-->0;)if(targets.contains(cues[i].toObject().value("video_id").toString()))cues.removeAt(i);
    if(cues.size()!=cueCount)scene["layouts"]=cues;
    auto motions=scene.value("motions").toArray();
    const auto motionCount=motions.size();
    for(qsizetype i=motions.size();i-->0;){const auto motion=motions[i].toObject();
        if(targets.contains(motion.value("layer_id").toString())||
           ((motion.value("kind")=="main"||motion.value("kind")=="speed")&&targets.contains(motion.value("kind").toString())))motions.removeAt(i);
    }
    if(motions.size()!=motionCount)scene["motions"]=motions;
    if(targets.contains(scene.value("audio_id").toString()))scene["audio_id"]=QJsonValue(QJsonValue::Null);
    const auto canvas=scene.value("canvas_layers").toObject().toVariantMap(),removed=removeCanvasEntries(canvas,targets);
    if(canvas!=removed)scene["canvas_layers"]=QJsonObject::fromVariantMap(removed);
    const auto trajectoryBefore=root_.value("trajectory_studio").toObject();auto trajectory=trajectoryBefore;
    const auto trajectoryCanvas=trajectory.value("canvas").toObject().toVariantMap(),trajectoryRemoved=removeCanvasEntries(trajectoryCanvas,targets);
    if(trajectoryCanvas!=trajectoryRemoved)trajectory["canvas"]=QJsonObject::fromVariantMap(trajectoryRemoved);
    if(scene==before&&trajectory==trajectoryBefore)return;
    retainSourceCatalog();setComposition(scene);if(trajectory!=trajectoryBefore)root_["trajectory_studio"]=trajectory;
}
bool ProjectDocument::removeSource(const QString &id,bool removeReferences)
{
    QVariantMap target;
    for(const auto &v:sources())if(v.toMap().value("id").toString()==id)target=v.toMap();
    if(target.isEmpty())return false;
    const auto references=target.value("references").toList();if(!references.isEmpty()&&!removeReferences)return false;
    retainSourceCatalog();
    QStringList layers;
    for(const auto &v:references){const auto r=v.toMap();const auto key=r.value("id").toString();
        if(key=="bag-data"){root_["bag"]=QJsonValue(QJsonValue::Null);root_["source"]="none";}
        else if(key=="trajectory-source")root_.remove("trajectory_studio");
        else if(key=="trajectory-background-file"){auto trajectory=root_.value("trajectory_studio").toObject();trajectory.remove("background_file");root_["trajectory_studio"]=trajectory;}
        else if(key=="extrinsics")root_["extrinsics"]=QJsonValue(QJsonValue::Null);
        else layers.append(key);
    }
    removeCanvasLayers(layers);
    auto all=root_.value("sources").toArray();for(qsizetype i=all.size();i-->0;)if(all[i].toObject().value("id").toString()==id)all.removeAt(i);root_["sources"]=all;return true;
}
void ProjectDocument::renameSource(const QString &id,const QString &name)
{
    if(name.trimmed().isEmpty())return;retainSourceCatalog();auto all=root_.value("sources").toArray();
    for(qsizetype i=0;i<all.size();++i){auto s=all[i].toObject();if(s.value("id").toString()==id){s["name"]=name.trimmed();all[i]=s;}}root_["sources"]=all;
}
void ProjectDocument::replaceSource(const QString &id,const QString &file)
{
    if(!QFileInfo(file).isFile())fail(QStringLiteral("替换素材不存在。"));
    QVariantMap target;for(const auto &v:sources())if(v.toMap().value("id").toString()==id)target=v.toMap();
    if(target.isEmpty())return;
    const auto source=target.value("path").toString(),kind=target.value("kind").toString(),absolute=absolutePath(file);
    if(sameMediaPath(source,absolute))return;
    retainSourceCatalog();
    const auto reference=path_.isEmpty()?absolute:referenceFrom(QFileInfo(path_).absolutePath(),absolute);
    const auto replacePath=[&](QJsonObject object){
        if(sameMediaPath(absoluteMedia(object.value("path").toString()),source))object["path"]=reference;
        return object;
    };
    // Path+kind is the source identity: replacing a video's image/audio use of
    // the same file would otherwise change an independent source silently.
    auto scene=composition();const QString key=kind=="video"?"videos":kind=="image"?"images":kind=="audio"?"audio":"";
    if(!key.isEmpty()){
        QJsonArray array;for(const auto &v:scene.value(key).toArray())array.append(replacePath(v.toObject()));
        if(scene.contains(key))scene[key]=array;setComposition(scene);
    }
    if(kind=="bag"&&sameMediaPath(bag(),source))root_["bag"]=reference;
    if(kind=="calibration"&&sameMediaPath(extrinsics(),source))root_["extrinsics"]=reference;
    if(kind=="video"&&root_.contains("trajectory_studio"))root_["trajectory_studio"]=replacePath(root_.value("trajectory_studio").toObject());
    if(kind=="image"){
        const auto replaceCanvas=[&](const QVariantMap &canvas){return remapCanvasImages(canvas,[&](const QString &path){const auto previous=absoluteMedia(path);return sameMediaPath(previous,source)?reference:path;});};
        scene=composition();if(scene.contains("canvas_layers")){scene["canvas_layers"]=QJsonObject::fromVariantMap(replaceCanvas(scene.value("canvas_layers").toObject().toVariantMap()));setComposition(scene);}
        auto trajectory=root_.value("trajectory_studio").toObject();
        if(trajectory.contains("canvas"))trajectory["canvas"]=QJsonObject::fromVariantMap(replaceCanvas(trajectory.value("canvas").toObject().toVariantMap()));
        if(sameMediaPath(absoluteMedia(trajectory.value("background_file").toString()),source))trajectory["background_file"]=reference;
        if(root_.contains("trajectory_studio"))root_["trajectory_studio"]=trajectory;
    }
    QJsonArray catalog;
    for(const auto &v:root_.value("sources").toArray()){
        auto item=v.toObject();
        if(item.value("id").toString()==id)item["path"]=reference;
        else if(item.value("kind").toString()==kind&&sameMediaPath(absoluteMedia(item.value("path").toString()),absolute))continue;
        catalog.append(item);
    }
    root_["sources"]=catalog;
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
