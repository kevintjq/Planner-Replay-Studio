#pragma once

#include "rviz_layout.h"

#include <QStringList>
#include <QQuaternion>
#include <QVector3D>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace replay {

class TransformTree;
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

struct TopicOverlay {
    enum class Primitive { Default, Points, Lines, LineStrip, Triangles };

    struct Sample {
        Frame range;
        QString frameId;
        Primitive primitive = Primitive::Default;
        QColor color;
        qint64 expires = 0;
        quint64 order = 0;
        bool frameLocked = false;
        float size = 0.f;
    };
    TopicDisplay display;
    std::vector<Point> points;
    std::vector<Sample> samples;
    std::shared_ptr<const std::vector<std::pair<qint64, quint64>>> clears;
    const Sample *sampleAt(qint64 stamp) const;
};

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
    std::vector<TopicOverlay> topicOverlays;
    QStringList topicWarnings;
    std::shared_ptr<const TransformTree> transforms;
    std::vector<std::pair<qint64, QString>> planningEvents;
    bool showMainCloud = true;
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
    RvizLayout topicLayout;
    QString planTopic, corridorTopic, goalTopic, cameraTopic, planFrame;
    bool showPlan = true, showCorridor = true, showGoal = true, showCamera = true;
    bool showMainCloud = true;
};

using Cancel = std::function<bool()>;
using Progress = std::function<void(double)>;

std::shared_ptr<const Recording> loadBag(const QString &path, const LoadOptions &options = {},
                                         const Cancel &cancel = {}, const Progress &progress = {});
std::shared_ptr<const Recording> makeDemo();
}
