#include "rviz_layout.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QUuid>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace replay {
namespace {
constexpr qsizetype MaxConfigBytes = 8 * 1024 * 1024;
constexpr size_t MaxEntries = 10'000;
const QColor DefaultColor("#4dddf3");

YAML::Node field(const YAML::Node &mapping, const char *key)
{
    if (!mapping.IsDefined() || !mapping.IsMap())
        return {};
    const auto node = mapping[key];
    return node.IsDefined() ? node : YAML::Node();
}

QString scalar(const YAML::Node &node)
{
    return node.IsScalar() ? QString::fromStdString(node.Scalar()) : QString();
}

QString normalizedFrame(QString frame)
{
    frame = frame.trimmed();
    while (frame.startsWith('/'))
        frame.remove(0, 1);
    while (frame.endsWith('/'))
        frame.chop(1);
    return frame;
}

QColor displayColor(const YAML::Node &node, size_t index)
{
    static const std::array<QColor, 5> defaults{DefaultColor, QColor("#ffca63"), QColor("#7aec9d"),
                                                QColor("#b6a0ff"), QColor("#fb969c")};
    const auto channels = scalar(node).split(';');
    if (channels.size() != 3)
        return defaults[index % defaults.size()];
    std::array<int, 3> rgb;
    for (int i = 0; i < 3; ++i) {
        bool ok;
        rgb[i] = channels[i].trimmed().toInt(&ok);
        if (!ok || rgb[i] < 0 || rgb[i] > 255)
            return defaults[index % defaults.size()];
    }
    return QColor(rgb[0], rgb[1], rgb[2]);
}

void appendDisplays(RvizLayout &layout, const YAML::Node &entries, bool active,
                    std::vector<YAML::Node> &parents, size_t &visited, const QList<int> &parentPath = {})
{
    static const QHash<QString, QString> kinds{
        {"PointCloud2", "cloud"}, {"LaserScan", "laser"}, {"Path", "path"},
        {"Marker", "marker"}, {"MarkerArray", "marker"}, {"Pose", "position"},
        {"PointStamped", "position"}, {"Odometry", "position"}, {"TF", "tf"},
        {"Image", "image"}, {"Camera", "image"}
    };
    int entryIndex = -1;
    for (const YAML::Node entry : entries) {
        auto path = parentPath; path.append(++entryIndex);
        if (++visited > MaxEntries || parents.size() >= 64)
            throw std::runtime_error("RViz 显示配置的分组过多。");
        if (!entry.IsMap() || std::any_of(parents.begin(), parents.end(), [&](const auto &parent) {
                return entry.is(parent);
            }))
            continue;

        const auto plugin = scalar(field(entry, "Class")).section('/', -1);
        auto name = scalar(field(entry, "Name"));
        if (name.isEmpty())
            name = plugin.isEmpty() ? QStringLiteral("未命名显示") : plugin;
        const auto enabledNode = field(entry, "Enabled");
        const bool enabled = active && (enabledNode.IsNull() ? field(entry, "Value").as<bool>(true)
                                                              : enabledNode.as<bool>(true));
        const auto children = field(entry, "Displays");
        if (plugin == "Group" && children.IsSequence()) {
            parents.push_back(entry);
            appendDisplays(layout, children, enabled, parents, visited, path);
            parents.pop_back();
            continue;
        }

        auto kind = kinds.value(plugin);
        auto topicNode = field(entry, "Topic");
        if (topicNode.IsNull() || (topicNode.IsScalar() && scalar(topicNode).isEmpty()))
            topicNode.reset(field(entry, "Marker Topic"));
        const auto topic = scalar(topicNode.IsMap() ? field(topicNode, "Value") : topicNode).trimmed();
        QStringList topics;
        if (kind == "tf") {
            topics = {"/tf", "/tf_static"};
        } else if (!topic.isEmpty()) {
            topics = {topic};
            if (kind.isEmpty()) {
                kind = QStringLiteral("message");
                layout.skipped.append(name + QStringLiteral("（导入为消息字段）"));
            }
        } else {
            layout.skipped.append(name);
            continue;
        }

        const auto color = displayColor(field(entry, "Color"), layout.displays.size());
        for (const auto &topicName : topics) {
            layout.displays.push_back({QUuid::createUuid().toString(QUuid::Id128), topicName, kind,
                                       name, color, enabled});
            layout.displays.back().sourcePath = path;
            layout.displays.back().pluginClass = scalar(field(entry, "Class"));
        }
    }
}
RvizLayout parseRviz(const QByteArray &yaml, const YAML::Node &root)
{
    if (!root.IsMap()) throw std::runtime_error("缺少 Visualization Manager / Displays。");
    const auto manager = field(root, "Visualization Manager"), displays = field(manager, "Displays");
    if (!displays.IsSequence()) throw std::runtime_error("缺少 Visualization Manager / Displays。");
    RvizLayout layout;
    layout.sourceYaml = QString::fromUtf8(yaml);
    if (yaml.startsWith("\xef\xbb\xbf") && !layout.sourceYaml.startsWith(QChar(0xfeff))) layout.sourceYaml.prepend(QChar(0xfeff));
    layout.fixedFrame = normalizedFrame(scalar(field(field(manager, "Global Options"), "Fixed Frame")));
    std::vector<YAML::Node> parents; size_t visited = 0;
    appendDisplays(layout, displays, true, parents, visited);
    return layout;
}
}

TopicDisplay TopicDisplay::fromJson(const QJsonObject &value)
{
    TopicDisplay display{value.value("id").toString(), value.value("topic").toString(),
                         value.value("kind").toString(), value.value("name").toString(),
                         QColor(value.value("color").toString()), value.value("enabled").toBool(true),
                         value.value("main_only").toBool(false)};
    if (!display.color.isValid())
        display.color = DefaultColor;
    for (const auto &index : value.value("source_path").toArray()) display.sourcePath.append(index.toInt(-1));
    display.pluginClass = value.value("plugin_class").toString();
    return display;
}

QJsonObject TopicDisplay::toJson() const
{
    QJsonArray path; for (int index : sourcePath) path.append(index);
    return {{"id", id}, {"topic", topic}, {"kind", kind}, {"name", name},
            {"color", color.name()}, {"enabled", enabled}, {"main_only", mainOnly},
            {"source_path", path}, {"plugin_class", pluginClass}};
}

RvizLayout readRviz(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("无法读取 RViz 配置文件。");
    if (file.size() > MaxConfigBytes)
        throw std::runtime_error("RViz 配置文件超过 8 MiB。");

    try {
        const auto originalYaml = file.readAll();
        auto yaml = originalYaml;
        if (yaml.startsWith("\xef\xbb\xbf"))
            yaml.remove(0, 3);
        const YAML::Node root = YAML::Load(yaml.toStdString());
        return parseRviz(originalYaml, root);
    } catch (const YAML::Exception &) {
        throw std::runtime_error("无法解析 RViz YAML 配置。");
    }
}

QByteArray serializeRviz(const RvizLayout &layout)
{
    if (layout.sourceYaml.toUtf8().size() > MaxConfigBytes || layout.displays.size() > MaxEntries)
        throw std::runtime_error("RViz 配置过大。 / RViz configuration exceeds limits.");
    try {
        YAML::Node root; RvizLayout baseline;
        if (!layout.sourceYaml.isEmpty()) {
            root = YAML::Load(layout.sourceYaml.toStdString());
            baseline = parseRviz(layout.sourceYaml.toUtf8(), root);
        } else {
            root["Visualization Manager"]["Displays"] = YAML::Node(YAML::NodeType::Sequence);
        }
        bool changed = layout.sourceYaml.isEmpty();
        for (const auto &original : baseline.displays)
            if (std::none_of(layout.displays.begin(), layout.displays.end(), [&](const auto &display) {
                    return display.sourcePath == original.sourcePath && display.kind == original.kind &&
                        (original.kind != "tf" || display.topic == original.topic);
                }))
                throw std::runtime_error("原始显示节点缺失；当前导出不能静默保留已删除显示。 / Missing source display.");
        auto manager = root["Visualization Manager"];
        if (layout.fixedFrame != baseline.fixedFrame) {
            manager["Global Options"]["Fixed Frame"] = layout.fixedFrame.toStdString(); changed = true;
        }
        const auto colorText = [](const QColor &color) { return QString("%1; %2; %3").arg(color.red()).arg(color.green()).arg(color.blue()).toStdString(); };
        const QHash<QString, QString> classes{{"cloud", "rviz/PointCloud2"}, {"laser", "rviz/LaserScan"}, {"path", "rviz/Path"},
            {"marker", "rviz/MarkerArray"}, {"position", "rviz/Pose"}, {"tf", "rviz/TF"}, {"image", "rviz/Image"}};
        for (const auto &display : layout.displays) {
            const auto original = std::find_if(baseline.displays.begin(), baseline.displays.end(), [&](const auto &item) {
                return item.sourcePath == display.sourcePath && item.kind == display.kind &&
                    (item.kind != "tf" || item.topic == display.topic);
            });
            if (display.sourcePath.isEmpty()) {
                const auto plugin = display.pluginClass.isEmpty() ? classes.value(display.kind) : display.pluginClass;
                if (plugin.isEmpty()) throw std::runtime_error("该消息显示没有可导出的 RViz 插件。 / No corresponding RViz plugin.");
                YAML::Node entry; entry["Class"] = plugin.toStdString(); entry["Name"] = display.name.toStdString();
                entry["Enabled"] = display.enabled; entry["Color"] = colorText(display.color);
                if (display.kind != "tf") entry["Topic"] = display.topic.toStdString();
                manager["Displays"].push_back(entry); changed = true; continue;
            }
            if (original == baseline.displays.end()) throw std::runtime_error("RViz 源属性路径失效，已停止导出。 / Stale RViz source path.");
            if (display.pluginClass != original->pluginClass)
                throw std::runtime_error("不能在保真导出时静默更改插件类型。 / Changing a source plugin class is unsupported.");
            if (std::count_if(layout.displays.begin(), layout.displays.end(), [&](const auto &other) {
                    return other.sourcePath == display.sourcePath && other.kind == display.kind && other.topic == display.topic;
                }) > 1)
                throw std::runtime_error("多个显示映射到同一源属性，已停止导出。 / Duplicate RViz source mapping.");
            if (display.kind == "tf") {
                // 原生模型暂将 TF 的动态/静态话题拆开；单个 RViz TF 插件无法表达互相冲突的设置。
                for (const auto &other : layout.displays) if (other.sourcePath == display.sourcePath &&
                    (other.enabled != display.enabled || other.name != display.name || other.color != display.color))
                    throw std::runtime_error("TF 子显示设置冲突；单个 RViz TF 插件无法无损表示。 / Conflicting TF display settings.");
            }
            auto node = manager;
            for (int depth = 0; depth < display.sourcePath.size(); ++depth) {
                const int index = display.sourcePath[depth];
                const auto entries = field(node, "Displays");
                if (!entries.IsSequence() || index < 0 || size_t(index) >= entries.size()) throw std::runtime_error("Invalid RViz source path.");
                node.reset(entries[size_t(index)]);
                if (depth + 1 < display.sourcePath.size() && display.enabled && !original->enabled) {
                    const auto enabled = field(node, "Enabled");
                    const bool active = enabled.IsNull() ? field(node, "Value").as<bool>(true) : enabled.as<bool>(true);
                    if (!active) throw std::runtime_error("父分组已禁用，不能无损表达单独启用的子显示；请先在 RViz 启用分组。 / Parent RViz group is disabled.");
                }
            }
            if (display.name != original->name) { node["Name"] = display.name.toStdString(); changed = true; }
            if (display.enabled != original->enabled) { node["Enabled"] = display.enabled; changed = true; }
            if (display.color != original->color) { node["Color"] = colorText(display.color); changed = true; }
            if (display.kind != "tf" && display.topic != original->topic) {
                const char *key = field(node, "Topic").IsNull() && !field(node, "Marker Topic").IsNull() ? "Marker Topic" : "Topic";
                if (node[key].IsMap()) node[key]["Value"] = display.topic.toStdString();
                else node[key] = display.topic.toStdString();
                changed = true;
            }
        }
        // 未编辑时逐字返回原配置；有编辑时只改对应节点，保留未知插件、分组、相机、工具及自定义属性。
        if (!changed) return layout.sourceYaml.toUtf8();
        YAML::Emitter emitter; emitter << root;
        if (!emitter.good()) throw std::runtime_error("Cannot serialize RViz configuration.");
        const auto bytes = QByteArray(emitter.c_str()) + '\n';
        if (bytes.size() > MaxConfigBytes) throw std::runtime_error("RViz output exceeds 8 MiB.");
        return bytes;
    } catch (const YAML::Exception &) { throw std::runtime_error("无法序列化 RViz YAML。 / Cannot serialize RViz YAML."); }
}
}
