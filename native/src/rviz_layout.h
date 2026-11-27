#pragma once

#include <QColor>
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

    static TopicDisplay fromJson(const QJsonObject &value);
    QJsonObject toJson() const;
};

struct RvizLayout {
    QString fixedFrame;
    std::vector<TopicDisplay> displays;
    QStringList skipped;
};

RvizLayout readRviz(const QString &path);
}
