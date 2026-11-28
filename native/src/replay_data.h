#pragma once

#include "rviz_layout.h"
#include "ros_topic_message.h"

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

struct TopicCatalogEntry {
    QString name, type, source;
    qint64 count = 0, firstStampNs = 0, lastStampNs = 0;
    int connectionCount = 0;
    bool definitionAvailable = false, schemaConflict = false;
    QVariantList fields;
    QStringList numericFields;
    QString schemaStatus;
    double durationSeconds() const;
    double frequencyHz() const;
    QVariantMap toVariant() const;
};

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
    QString speedTopic, bodyFrame;
    QString scanTopic;
    QStringList scanTopics;
    qint64 scanMaximumAge = 500'000'000;
    QString frameId;
    QStringList cloudTopics;
    QStringList odometryTopics;
    QStringList speedTopics;
    QStringList topics;
    std::vector<TopicCatalogEntry> topicCatalog;
    qint64 recordedOrigin = 0;
    qint64 timelineFirstStampNs = 0, timelineLastStampNs = 0;
    bool hasMessages = false, storageTimeFallback = false;
    QString timelineTimeBasis;
    std::vector<Point> points;
    std::vector<Frame> frames;
    std::vector<Pose> poses;
    std::vector<std::pair<qint64, float>> speeds;
    qint64 maximumGap = 500'000'000;
    bool tfPose = false;
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
    std::optional<float> speedAt(qint64 stamp) const;
};

struct LoadOptions {
    QString cloudTopic;
    QString odometryTopic;
    std::optional<MapMode> mapMode;
    RvizLayout topicLayout;
    QString planTopic, corridorTopic, goalTopic, cameraTopic, planFrame;
    bool showPlan = true, showCorridor = true, showGoal = true, showCamera = true;
    bool showMainCloud = true;
    QString speedTopic, world, body, extrinsics;
    bool tfPose = false;
    double maximumGap = .5;
    QString scanTopic;
    double scanMaximumAge = .5;
    bool autoScan = false;
    // Append options to preserve existing aggregate initializers.
    // Lazy Display decoding never selects the legacy global cloud/pose roles.
    bool displayDataOnly = false;
};

using Cancel = std::function<bool()>;
using Progress = std::function<void(double)>;

QJsonObject inspectBag(const QString &path, const Cancel &cancel = {});
std::vector<TopicCatalogEntry> inspectBagCatalog(const QString &path, const Cancel &cancel = {},
                                                 const Progress &progress = {});
std::optional<TopicMessage> inspectBagTopic(const QString &path, const QString &topic, qint64 stamp,
                                            const Cancel &cancel = {}, int arrayPage = 0,
                                            const QString &type = {}, const QString &selectionTimeBasis = QStringLiteral("storage"));
struct NumericTopicSeries {
    QVariantList samples;
    QVariantMap statistics;
    QString status;
};
NumericTopicSeries sampleBagTopic(const QString &path, const QString &topic, const QString &fieldPath,
                                   const QString &timeBasis = QStringLiteral("storage"),
                                   const QString &firstStampNs = {}, const QString &lastStampNs = {},
                                   int maxSamples = 20000, const QString &type = {},
                                   const Cancel &cancel = {});
// Serialized views are valid only during the callback. Every selected message
// is visited exactly once; no display decimation or implicit clock mapping.
using BagTopicVisitor = std::function<void(const QByteArray &type, const QByteArray &definition,
                                         QByteArrayView serialized, qint64 storageStampNs)>;
qint64 visitBagTopic(const QString &path, const QString &topic, const BagTopicVisitor &visitor,
                     const QString &type = {}, const Cancel &cancel = {});
std::shared_ptr<const Recording> loadBag(const QString &path, const LoadOptions &options = {},
                                         const Cancel &cancel = {}, const Progress &progress = {});
std::shared_ptr<const Recording> loadDisplayData(const QString &path, const QString &fixedFrame,
    const std::vector<TopicDisplay> &displays, const Cancel &cancel = {}, const Progress &progress = {});
std::shared_ptr<const Recording> makeDemo();
}
