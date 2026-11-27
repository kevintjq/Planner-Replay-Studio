#pragma once

#include <QColor>
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
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

    static TopicDisplay fromJson(const QJsonObject &value);
    QJsonObject toJson() const;
};

struct RvizLayout {
    QString fixedFrame;
    std::vector<TopicDisplay> displays;
    QStringList skipped;
    QString sourceYaml;
};

RvizLayout readRviz(const QString &path);
QByteArray serializeRviz(const RvizLayout &layout);
}
