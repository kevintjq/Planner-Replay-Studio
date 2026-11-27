#pragma once

#include <QHash>
#include <QQuaternion>
#include <QString>
#include <QStringList>
#include <QVector3D>
#include <optional>
#include <vector>

namespace replay {
struct RigidTransform {
    QVector3D translation;
    QQuaternion rotation;

    QVector3D map(const QVector3D &point) const;
    RigidTransform inverted() const;
    RigidTransform operator*(const RigidTransform &source) const;
};

class TransformTree {
public:
    explicit TransformTree(qint64 maximumGap = 500'000'000) : maximumGap_(maximumGap) {}
    void add(const QString &parent, const QString &child, qint64 stamp,
             const RigidTransform &transform, bool isStatic);
    void prepare();
    QString rootOf(const QString &frame) const;
    QStringList frames() const;
    QStringList children() const { return edges_.keys(); }
    std::vector<qint64> sampleTimes(const QString &frame) const;
    std::optional<RigidTransform> lookup(const QString &target, const QString &source, qint64 stamp) const;

private:
    struct Sample {
        qint64 stamp;
        RigidTransform transform;
    };
    struct Edge {
        QString parent;
        std::optional<RigidTransform> fixed;
        std::vector<Sample> samples;
        std::optional<RigidTransform> at(qint64 stamp, qint64 maximumGap) const;
    };
    QHash<QString, Edge> edges_;
    qint64 maximumGap_;
};
}
