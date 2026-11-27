#include "rviz_layout.h"

#include <QFile>
#include <QHash>
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
                    std::vector<YAML::Node> &parents, size_t &visited)
{
    static const QHash<QString, QString> kinds{
        {"PointCloud2", "cloud"}, {"LaserScan", "laser"}, {"Path", "path"},
        {"Marker", "marker"}, {"MarkerArray", "marker"}, {"Pose", "position"},
        {"PointStamped", "position"}, {"Odometry", "position"}, {"TF", "tf"},
        {"Image", "image"}, {"Camera", "image"}
    };
    for (const YAML::Node entry : entries) {
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
            appendDisplays(layout, children, enabled, parents, visited);
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
        }
    }
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
    return display;
}

QJsonObject TopicDisplay::toJson() const
{
    return {{"id", id}, {"topic", topic}, {"kind", kind}, {"name", name},
            {"color", color.name()}, {"enabled", enabled}, {"main_only", mainOnly}};
}

RvizLayout readRviz(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("无法读取 RViz 配置文件。");
    if (file.size() > MaxConfigBytes)
        throw std::runtime_error("RViz 配置文件超过 8 MiB。");

    try {
        auto yaml = file.readAll();
        if (yaml.startsWith("\xef\xbb\xbf"))
            yaml.remove(0, 3);
        const YAML::Node root = YAML::Load(yaml.toStdString());
        if (!root.IsMap())
            throw std::runtime_error("缺少 Visualization Manager / Displays。");
        const auto manager = field(root, "Visualization Manager");
        const auto displays = field(manager, "Displays");
        if (!displays.IsSequence())
            throw std::runtime_error("缺少 Visualization Manager / Displays。");

        RvizLayout layout;
        layout.fixedFrame = normalizedFrame(scalar(field(field(manager, "Global Options"), "Fixed Frame")));
        std::vector<YAML::Node> parents;
        size_t visited = 0;
        appendDisplays(layout, displays, true, parents, visited);
        return layout;
    } catch (const YAML::Exception &) {
        throw std::runtime_error("无法解析 RViz YAML 配置。");
    }
}
}
