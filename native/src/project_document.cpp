#include "project_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
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

void ProjectDocument::setValue(const QString &key, const QString &value)
{
    auto settings = values();
    settings.insert(key, value);
    root_.insert("values", settings);
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

void ProjectDocument::setVideoTiming(qsizetype index, const QJsonObject &changes)
{
    auto scene = root_.value("composition").toObject();
    auto layers = scene.value("videos").toArray();
    if (index < 0 || index >= layers.size())
        fail(QStringLiteral("视频图层已失效。"));
    auto layer = layers[index].toObject();
    auto edit = layer.value("edit").toObject();
    for (auto it = changes.begin(); it != changes.end(); ++it)
        edit.insert(it.key(), it.value());
    VideoTiming::fromJson(edit);
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
