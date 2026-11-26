#pragma once

#include <QStringList>
#include <QQuaternion>
#include <QVector3D>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace replay {
struct Point {
    float x, y, z;
};

struct Frame {
    qint64 stamp;
    int first;
    int end;
};

struct Pose {
    qint64 stamp;
    QVector3D position;
    QQuaternion rotation;
    std::optional<float> speed;
};

enum class MapMode { Accumulate, Replace };

struct Recording {
    QString name;
    QString cloudTopic;
    QString odometryTopic;
    QString frameId;
    QStringList cloudTopics;
    QStringList odometryTopics;
    std::vector<Point> points;
    std::vector<Frame> frames;
    std::vector<Pose> poses;
    QVector3D center;
    float radius = 1.f;
    bool replace = false;

    qint64 stampAt(double seconds) const;
    double duration() const;
    const Frame &frameAt(qint64 stamp) const;
    std::optional<Pose> poseAt(qint64 stamp) const;
};

struct LoadOptions {
    QString cloudTopic;
    QString odometryTopic;
    std::optional<MapMode> mapMode;
};

using Cancel = std::function<bool()>;
using Progress = std::function<void(double)>;

std::shared_ptr<const Recording> loadBag(const QString &path, const LoadOptions &options = {},
                                         const Cancel &cancel = {}, const Progress &progress = {});
std::shared_ptr<const Recording> makeDemo();
}
