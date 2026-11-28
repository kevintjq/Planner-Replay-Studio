#pragma once

#include <QColor>
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <QVariantList>
#include <vector>

namespace replay {
struct TopicDisplay {
    QString id;
    QString topic;
    QString kind;
    QString name;
    QColor color;
    bool enabled = true;
    bool mainOnly = false;
    QString plannerRole;
    QList<int> sourcePath;
    QString pluginClass;

    // Independent C++ renderer settings. These are not a claim that this
    // process instantiates the corresponding native RViz Property classes.
    double alpha = 1.;
    double pointSize = 3.;
    double lineWidth = 2.;
    double historySeconds = 0.;
    bool useMessageColor = true;
    QString parentId;
    QString rawConfigYaml;
    QJsonObject properties;

    static TopicDisplay fromJson(const QJsonObject &value);
    QJsonObject toJson() const;
};

struct RvizLayout {
    QString fixedFrame;
    std::vector<TopicDisplay> displays;
    QStringList skipped;
    QString sourceYaml;
    // One entry per actual Display/Group, including unsupported plugins.
    // The legacy displays above may split a TF entry into two subscriptions.
    std::vector<TopicDisplay> instances;
    bool useInstanceModel = false;
    // Invalid means absent/unrecognized: preserve the source YAML untouched.
    QColor backgroundColor;
};

RvizLayout readRviz(const QString &path);
QByteArray serializeRviz(const RvizLayout &layout);
std::vector<TopicDisplay> rvizDisplayInstances(const RvizLayout &layout);
RvizLayout withRvizDisplayInstances(const RvizLayout &layout, const std::vector<TopicDisplay> &instances);
QString independentDisplayKind(const QString &pluginClass);
QVariantList rvizDisplayPropertyTree(const TopicDisplay &display);
QColor rvizBackgroundColor(const QString &sourceYaml);
}
