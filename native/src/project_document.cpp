#include "project_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
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
