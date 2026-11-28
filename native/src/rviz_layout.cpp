#include "rviz_layout.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QUuid>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
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

QColor rgbColor(const YAML::Node &node)
{
    const auto channels = scalar(node).split(';');
    if (channels.size() != 3)
        return {};
    std::array<int, 3> rgb;
    for (int i = 0; i < 3; ++i) {
        bool ok;
        rgb[i] = channels[i].trimmed().toInt(&ok);
        if (!ok || rgb[i] < 0 || rgb[i] > 255)
            return {};
    }
    return QColor(rgb[0], rgb[1], rgb[2]);
}

QColor displayColor(const YAML::Node &node, size_t index)
{
    static const std::array<QColor, 5> defaults{DefaultColor, QColor("#ffca63"), QColor("#7aec9d"),
                                                QColor("#b6a0ff"), QColor("#fb969c")};
    const auto color = rgbColor(node);
    return color.isValid() ? color : defaults[index % defaults.size()];
}

double finiteNumber(const YAML::Node &node, double fallback, double low, double high)
{
    if (!node.IsScalar()) return fallback;
    try {
        const auto value = node.as<double>();
        return std::isfinite(value) && value >= low && value <= high ? value : fallback;
    } catch (const YAML::Exception &) { return fallback; }
}

QString classForKind(const QString &kind)
{
    static const QHash<QString, QString> classes{{"cloud", "rviz/PointCloud2"}, {"laser", "rviz/LaserScan"},
        {"path", "rviz/Path"}, {"marker", "rviz/MarkerArray"}, {"position", "rviz/Pose"},
        {"tf", "rviz/TF"}, {"grid", "rviz/Grid"}, {"axes", "rviz/Axes"}, {"group", "rviz/Group"}};
    return classes.value(kind);
}

void readInstanceStyle(TopicDisplay &display, const YAML::Node &entry)
{
    if (entry.IsMap()) for (const auto &property : entry) {
        if (!property.first.IsScalar() || !property.second.IsScalar()) continue;
        const auto name = scalar(property.first), value = scalar(property.second);
        bool number = false; const auto numeric = value.toDouble(&number);
        if (value == "true" || value == "false") display.properties[name] = value == "true";
        else if (number && std::isfinite(numeric)) display.properties[name] = numeric;
        else display.properties[name] = value;
    }
    display.alpha = finiteNumber(field(entry, "Alpha"), 1., 0., 1.);
    display.pointSize = finiteNumber(field(entry, "Size (Pixels)"), 3., .1, 512.);
    display.historySeconds = finiteNumber(field(entry, "Decay Time"), 0., 0., 86400.);
    // Native Path Line Width is metres. Keep it unchanged, rather than
    // pretending that the independent renderer's pixel width is equivalent.
    const auto local = field(entry, "UAV Studio");
    display.alpha = finiteNumber(field(local, "Alpha"), display.alpha, 0., 1.);
    display.pointSize = finiteNumber(field(local, "Point Size (Pixels)"), display.pointSize, .1, 512.);
    display.lineWidth = finiteNumber(field(local, "Line Width (Pixels)"), 2., .1, 512.);
    display.historySeconds = finiteNumber(field(local, "History (Seconds)"), display.historySeconds, 0., 86400.);
    const auto messageColor = field(local, "Use Message Color");
    if (messageColor.IsScalar()) display.useMessageColor = messageColor.as<bool>(true);
    else display.useMessageColor = scalar(field(entry, "Color Transformer")) != "FlatColor";
}

void appendInstances(RvizLayout &layout, const YAML::Node &entries, const QString &parentId,
                     std::vector<YAML::Node> &parents, size_t &visited, const QList<int> &parentPath = {})
{
    if (!entries.IsSequence()) return;
    int index = -1;
    for (const auto entry : entries) {
        auto path = parentPath; path.append(++index);
        if (++visited > MaxEntries || parents.size() >= 64)
            throw std::runtime_error("RViz 显示实例或分组过多。 / Too many RViz display instances.");
        if (std::any_of(parents.begin(), parents.end(), [&](const auto &ancestor) { return entry.is(ancestor); }))
            throw std::runtime_error("RViz 显示分组包含循环引用。 / Cyclic RViz display group.");
        TopicDisplay display;
        const auto legacy = std::find_if(layout.displays.begin(), layout.displays.end(), [&](const auto &item) { return item.sourcePath == path; });
        display.id = legacy == layout.displays.end() ? QUuid::createUuid().toString(QUuid::Id128) : legacy->id;
        display.sourcePath = path; display.parentId = parentId;
        display.pluginClass = scalar(field(entry, "Class"));
        display.kind = independentDisplayKind(display.pluginClass);
        display.name = scalar(field(entry, "Name"));
        if (display.name.isEmpty()) display.name = display.pluginClass.isEmpty() ? QStringLiteral("Unrecognized Display · 未识别显示") : display.pluginClass.section('/', -1);
        const auto enabled = field(entry, "Enabled");
        display.enabled = enabled.IsNull() ? field(entry, "Value").as<bool>(true) : enabled.as<bool>(true);
        auto topic = field(entry, "Topic");
        if (topic.IsNull() || (topic.IsScalar() && scalar(topic).isEmpty())) topic.reset(field(entry, "Marker Topic"));
        display.topic = scalar(topic.IsMap() ? field(topic, "Value") : topic).trimmed();
        if (display.kind == "tf") display.topic = "/tf";
        display.color = displayColor(field(entry, "Color"), layout.instances.size());
        readInstanceStyle(display, entry);
        auto raw = YAML::Clone(entry);
        if (raw.IsMap()) raw.remove("Displays");
        display.rawConfigYaml = QString::fromStdString(YAML::Dump(raw));
        layout.instances.push_back(display);
        const auto children = field(entry, "Displays");
        if (children.IsSequence()) {
            parents.push_back(entry);
            appendInstances(layout, children, display.id, parents, visited, path);
            parents.pop_back();
        }
    }
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
    layout.backgroundColor = rgbColor(field(field(manager, "Global Options"), "Background Color"));
    std::vector<YAML::Node> parents; size_t visited = 0;
    appendDisplays(layout, displays, true, parents, visited);
    parents.clear(); visited = 0;
    appendInstances(layout, displays, {}, parents, visited);
    return layout;
}
}

QColor rvizBackgroundColor(const QString &sourceYaml)
{
    auto bytes = sourceYaml.toUtf8();
    if (bytes.isEmpty() || bytes.size() > MaxConfigBytes) return {};
    if (bytes.startsWith("\xef\xbb\xbf")) bytes.remove(0, 3);
    try {
        return rgbColor(field(field(field(YAML::Load(bytes.toStdString()), "Visualization Manager"), "Global Options"), "Background Color"));
    } catch (const YAML::Exception &) { return {}; }
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
    display.alpha = value.value("alpha").toDouble(1.);
    display.pointSize = value.value("point_size").toDouble(3.);
    display.lineWidth = value.value("line_width").toDouble(2.);
    display.historySeconds = value.value("history_seconds").toDouble(0.);
    display.useMessageColor = value.value("use_message_color").toBool(true);
    display.parentId = value.value("parent_id").toString();
    display.rawConfigYaml = value.value("raw_config_yaml").toString();
    display.properties = value.value("properties").toObject();
    return display;
}

QJsonObject TopicDisplay::toJson() const
{
    QJsonArray path; for (int index : sourcePath) path.append(index);
    return {{"id", id}, {"topic", topic}, {"kind", kind}, {"name", name},
            {"color", color.name()}, {"enabled", enabled}, {"main_only", mainOnly},
            {"source_path", path}, {"plugin_class", pluginClass}, {"alpha", alpha},
            {"point_size", pointSize}, {"line_width", lineWidth}, {"history_seconds", historySeconds},
            {"use_message_color", useMessageColor}, {"parent_id", parentId},
            {"raw_config_yaml", rawConfigYaml}, {"properties", properties}};
}

QString independentDisplayKind(const QString &pluginClass)
{
    // Explicitly describes the standalone renderer, not pluginlib discovery.
    static const QHash<QString, QString> kinds{{"PointCloud2", "cloud"}, {"LaserScan", "laser"},
        {"Path", "path"}, {"Marker", "marker"}, {"MarkerArray", "marker"},
        {"Pose", "position"}, {"PointStamped", "position"}, {"Odometry", "position"},
        {"PoseWithCovariance", "position"}, {"TF", "tf"}, {"Grid", "grid"},
        {"Axes", "axes"}, {"Group", "group"}};
    // A third-party class with a matching suffix is not the built-in plugin.
    const auto package = pluginClass.section('/', 0, -2);
    if (package != "rviz" && package != "rviz_default_plugins") return "unsupported";
    return kinds.value(pluginClass.section('/', -1), "unsupported");
}

std::vector<TopicDisplay> rvizDisplayInstances(const RvizLayout &layout)
{
    if (!layout.instances.empty() || layout.useInstanceModel) return layout.instances;
    if (!layout.sourceYaml.isEmpty()) {
        try {
            return parseRviz(layout.sourceYaml.toUtf8(), YAML::Load(layout.sourceYaml.toStdString())).instances;
        } catch (const YAML::Exception &) { throw std::runtime_error("无法恢复原始 RViz 显示树。 / Cannot restore RViz display tree."); }
    }
    auto result = layout.displays;
    for (auto &display : result) {
        if (display.id.isEmpty()) display.id = QUuid::createUuid().toString(QUuid::Id128);
        if (display.pluginClass.isEmpty()) display.pluginClass = classForKind(display.kind);
    }
    return result;
}

RvizLayout withRvizDisplayInstances(const RvizLayout &layout, const std::vector<TopicDisplay> &instances)
{
    if (instances.size() > MaxEntries) throw std::runtime_error("显示实例超过 10000 个。 / Display instance limit exceeded.");
    auto result = layout; result.instances = instances; result.useInstanceModel = true; result.displays.clear();
    QHash<QString, const TopicDisplay *> byId;
    for (const auto &display : instances) {
        if (display.id.isEmpty() || byId.contains(display.id)) throw std::runtime_error("显示实例 ID 无效或重复。 / Invalid display identity.");
        byId.insert(display.id, &display);
    }
    for (const auto &display : instances) {
        auto effective = display; QSet<QString> visited{display.id}; auto parent = display.parentId;
        while (!parent.isEmpty()) {
            if (visited.contains(parent) || !byId.contains(parent)) throw std::runtime_error("显示分组关系无效。 / Invalid display group hierarchy.");
            visited.insert(parent); const auto *group = byId.value(parent);
            if (group->kind != "group") throw std::runtime_error("显示的父节点不是分组。 / Display parent is not a Group.");
            effective.enabled = effective.enabled && group->enabled; parent = group->parentId;
        }
        if (display.kind == "group" || display.kind == "grid" || display.kind == "axes" || display.topic.isEmpty()) continue;
        result.displays.push_back(effective);
        if (display.kind == "tf") {
            auto staticTf = effective; staticTf.id += ":static"; staticTf.topic = "/tf_static";
            result.displays.push_back(std::move(staticTf));
        }
    }
    return result;
}

QVariantList rvizDisplayPropertyTree(const TopicDisplay &display)
{
    if (display.rawConfigYaml.isEmpty()) return {};
    QVariantList result; size_t visited = 0;
    std::function<void(const YAML::Node &, const QStringList &, int)> append;
    append = [&](const YAML::Node &node, const QStringList &path, int depth) {
        if (++visited > MaxEntries || depth > 64) throw std::runtime_error("RViz 属性树超过安全限制。 / RViz property tree exceeds limits.");
        if (node.IsMap()) {
            for (const auto &entry : node) {
                if (!entry.first.IsScalar()) continue;
                auto next = path; next.append(scalar(entry.first)); append(entry.second, next, depth + 1);
            }
        } else if (node.IsSequence()) {
            for (size_t i = 0; i < node.size(); ++i) { auto next = path; next.append(QStringLiteral("[%1]").arg(i)); append(node[i], next, depth + 1); }
        } else {
            result.append(QVariantMap{{"path", path}, {"name", path.join(" / ")}, {"value", scalar(node)},
                {"readOnly", true}, {"status", QStringLiteral("Preserved configuration · 原始配置保留；不代表原生 Property")}});
        }
    };
    try { append(YAML::Load(display.rawConfigYaml.toStdString()), {}, 0); }
    catch (const YAML::Exception &) { return {QVariantMap{{"name", "Raw configuration"}, {"value", display.rawConfigYaml}, {"readOnly", true}}}; }
    return result;
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

namespace {
QByteArray serializeInstanceModel(const RvizLayout &layout)
{
    // Validate identities and hierarchy even for disabled/unsupported nodes.
    withRvizDisplayInstances(layout, layout.instances);
    YAML::Node root = layout.sourceYaml.isEmpty() ? YAML::Node(YAML::NodeType::Map) : YAML::Load(layout.sourceYaml.toStdString());
    if (!root.IsMap()) throw std::runtime_error("RViz 配置根节点无效。 / Invalid RViz root.");
    const auto originalStructure = YAML::Dump(root);
    auto manager = root["Visualization Manager"];
    if (!manager.IsDefined() || manager.IsNull()) manager = root["Visualization Manager"] = YAML::Node(YAML::NodeType::Map);
    const auto priorFrame = normalizedFrame(scalar(field(field(manager, "Global Options"), "Fixed Frame")));
    if (layout.fixedFrame != priorFrame) manager["Global Options"]["Fixed Frame"] = layout.fixedFrame.toStdString();
    QHash<QString, std::vector<const TopicDisplay *>> children;
    for (const auto &display : layout.instances) children[display.parentId].push_back(&display);
    size_t visited = 0;
    const auto colorText = [](const QColor &color) { return QString("%1; %2; %3").arg(color.red()).arg(color.green()).arg(color.blue()).toStdString(); };
    const auto priorBackground = rgbColor(field(field(manager, "Global Options"), "Background Color"));
    if (layout.backgroundColor.isValid() && layout.backgroundColor.alpha() == 255 && layout.backgroundColor != priorBackground)
        manager["Global Options"]["Background Color"] = colorText(layout.backgroundColor);
    std::function<YAML::Node(const TopicDisplay &, int)> emitDisplay;
    emitDisplay = [&](const TopicDisplay &display, int depth) -> YAML::Node {
        if (++visited > MaxEntries || depth >= 64) throw std::runtime_error("显示分组超过安全限制。 / Display hierarchy exceeds limits.");
        const bool fresh = display.rawConfigYaml.isEmpty();
        auto node = fresh ? YAML::Node(YAML::NodeType::Map) : YAML::Load(display.rawConfigYaml.toStdString());
        if (!node.IsMap()) {
            if (!children.value(display.id).empty()) throw std::runtime_error("非映射 Display 无法包含子显示。 / Invalid display mapping.");
            return node;
        }
        TopicDisplay original = display; original.properties = {}; readInstanceStyle(original, node);
        if (fresh || display.pluginClass != scalar(field(node, "Class"))) node["Class"] = display.pluginClass.toStdString();
        auto originalName = scalar(field(node, "Name"));
        if (originalName.isEmpty()) originalName = display.pluginClass.isEmpty() ? QStringLiteral("Unrecognized Display · 未识别显示") : display.pluginClass.section('/', -1);
        if (fresh || display.name != originalName) node["Name"] = display.name.toStdString();
        const auto enabled = field(node, "Enabled");
        const bool originalEnabled = enabled.IsNull() ? field(node, "Value").as<bool>(true) : enabled.as<bool>(true);
        if (fresh || display.enabled != originalEnabled) {
            node["Enabled"] = display.enabled;
            const auto value = scalar(field(node, "Value")).toLower();
            if (value == "true" || value == "false") node["Value"] = display.enabled;
        }
        if (display.kind != "tf" && display.kind != "group" && display.kind != "grid" && display.kind != "axes") {
            const char *key = field(node, "Topic").IsNull() && !field(node, "Marker Topic").IsNull() ? "Marker Topic" : "Topic";
            const auto topic = field(node, key);
            const auto oldTopic = scalar(topic.IsMap() ? field(topic, "Value") : topic).trimmed();
            if ((fresh && !display.topic.isEmpty()) || display.topic != oldTopic) {
                if (topic.IsMap()) node[key]["Value"] = display.topic.toStdString(); else node[key] = display.topic.toStdString();
            }
        }
        if (fresh || (!field(node, "Color").IsNull() && display.color != displayColor(field(node, "Color"), 0)) ||
            display.properties.value("_color_edited").toBool()) node["Color"] = colorText(display.color);
        if (fresh || display.alpha != original.alpha || display.pointSize != original.pointSize || display.lineWidth != original.lineWidth ||
            display.historySeconds != original.historySeconds || display.useMessageColor != original.useMessageColor) {
            auto local = node["UAV Studio"];
            local["Alpha"] = display.alpha; local["Point Size (Pixels)"] = display.pointSize;
            local["Line Width (Pixels)"] = display.lineWidth; local["History (Seconds)"] = display.historySeconds;
            local["Use Message Color"] = display.useMessageColor;
            if (display.kind == "cloud" || display.kind == "laser") {
                node["Alpha"] = display.alpha; node["Size (Pixels)"] = display.pointSize; node["Decay Time"] = display.historySeconds;
            }
        }
        for (const auto &name : {QStringLiteral("Cell Size"), QStringLiteral("Plane Cell Count"), QStringLiteral("Length")}) {
            if (display.properties.contains(name) && (fresh || display.properties.value(name) != original.properties.value(name)))
                node[name.toStdString()] = display.properties.value(name).toDouble();
        }
        if (display.kind == "group" || !children.value(display.id).empty()) {
            auto sequence = YAML::Node(YAML::NodeType::Sequence);
            auto sourceNode = manager;
            for (const auto sourceIndex : display.sourcePath) {
                const auto sourceEntries = field(sourceNode, "Displays");
                if (!sourceEntries.IsSequence() || sourceIndex < 0 || size_t(sourceIndex) >= sourceEntries.size()) { sourceNode.reset(); break; }
                sourceNode.reset(sourceEntries[size_t(sourceIndex)]);
            }
            const auto sourceChildren = field(sourceNode, "Displays");
            if (sourceChildren.IsSequence()) sequence.SetStyle(sourceChildren.Style());
            for (const auto *child : children.value(display.id)) sequence.push_back(emitDisplay(*child, depth + 1));
            node["Displays"] = sequence;
        }
        return node;
    };
    auto entries = YAML::Node(YAML::NodeType::Sequence);
    const auto sourceEntries = field(manager, "Displays");
    if (sourceEntries.IsSequence()) entries.SetStyle(sourceEntries.Style());
    for (const auto *display : children.value(QString())) entries.push_back(emitDisplay(*display, 0));
    if (visited != layout.instances.size()) throw std::runtime_error("存在无法导出的孤立显示。 / Orphan display nodes.");
    manager["Displays"] = entries;
    if (!layout.sourceYaml.isEmpty() && YAML::Dump(root) == originalStructure) return layout.sourceYaml.toUtf8();
    YAML::Emitter emitter; emitter << root;
    if (!emitter.good()) throw std::runtime_error("Cannot serialize RViz display instances.");
    const auto output = QByteArray(emitter.c_str()) + '\n';
    if (output.size() > MaxConfigBytes) throw std::runtime_error("RViz output exceeds 8 MiB.");
    return output;
}
}

QByteArray serializeRviz(const RvizLayout &layout)
{
    if (layout.sourceYaml.toUtf8().size() > MaxConfigBytes || layout.displays.size() > MaxEntries)
        throw std::runtime_error("RViz 配置过大。 / RViz configuration exceeds limits.");
    try {
        if (layout.useInstanceModel) return serializeInstanceModel(layout);
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
        if (layout.backgroundColor.isValid() && layout.backgroundColor.alpha() == 255 && layout.backgroundColor != baseline.backgroundColor) {
            manager["Global Options"]["Background Color"] = colorText(layout.backgroundColor); changed = true;
        }
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
                }) > 1 || std::count_if(layout.displays.begin(), layout.displays.end(), [&](const auto &other) {
                    return other.sourcePath == display.sourcePath;
                }) != std::count_if(baseline.displays.begin(), baseline.displays.end(), [&](const auto &other) {
                    return other.sourcePath == display.sourcePath;
                }))
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
            if (display.enabled != original->enabled) {
                node["Enabled"] = display.enabled;
                const auto value = scalar(field(node, "Value")).toLower();
                if (value == "true" || value == "false") node["Value"] = display.enabled;
                changed = true;
            }
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
