#include "transform_tree.h"

#include <QSet>
#include <algorithm>

namespace replay {
QVector3D RigidTransform::map(const QVector3D &point) const
{
    return rotation.rotatedVector(point) + translation;
}

RigidTransform RigidTransform::inverted() const
{
    const auto inverse = rotation.conjugated();
    return {inverse.rotatedVector(-translation), inverse};
}

RigidTransform RigidTransform::operator*(const RigidTransform &source) const
{
    return {map(source.translation), (rotation * source.rotation).normalized()};
}

void TransformTree::add(const QString &parent, const QString &child, qint64 stamp,
                        const RigidTransform &transform, bool isStatic)
{
    if (parent.isEmpty() || child.isEmpty() || parent == child)
        return;
    auto entry = edges_.find(child);
    if (entry == edges_.end()) {
        auto ancestor = edges_.constFind(parent);
        while (ancestor != edges_.cend()) {
            if (ancestor->parent == child)
                return;
            ancestor = edges_.constFind(ancestor->parent);
        }
        entry = edges_.insert(child, Edge{parent, {}, {}});
    }
    if (entry->parent != parent)
        return;
    if (isStatic) {
        if (!entry->fixed && entry->samples.empty())
            entry->fixed = transform;
    } else if (!entry->fixed) {
        entry->samples.push_back({stamp, transform});
    }
}

void TransformTree::prepare()
{
    for (auto &edge : edges_) {
        std::stable_sort(edge.samples.begin(), edge.samples.end(),
            [](const Sample &a, const Sample &b) { return a.stamp < b.stamp; });
    }
}

std::optional<RigidTransform> TransformTree::Edge::at(qint64 stamp) const
{
    if (fixed)
        return fixed;
    constexpr qint64 MaxGap = 500'000'000;
    const auto next = std::upper_bound(samples.begin(), samples.end(), stamp,
        [](qint64 time, const Sample &sample) { return time < sample.stamp; });
    if (next == samples.begin())
        return {};
    const auto &before = *std::prev(next);
    if (before.stamp == stamp)
        return before.transform;
    if (next == samples.end() || next->stamp - before.stamp > MaxGap)
        return {};
    const float fraction = float(double(stamp - before.stamp) / double(next->stamp - before.stamp));
    return RigidTransform{before.transform.translation * (1.f - fraction) + next->transform.translation * fraction,
                          QQuaternion::slerp(before.transform.rotation, next->transform.rotation, fraction)};
}

std::optional<RigidTransform> TransformTree::lookup(const QString &target, const QString &source, qint64 stamp) const
{
    if (target == source)
        return RigidTransform{};

    QHash<QString, RigidTransform> sourceChain;
    QString frame = source;
    RigidTransform worldFromSource;
    while (!sourceChain.contains(frame)) {
        sourceChain.insert(frame, worldFromSource);
        const auto edge = edges_.constFind(frame);
        const auto transform = edge == edges_.cend() ? std::nullopt : edge->at(stamp);
        if (!transform)
            break;
        worldFromSource = *transform * worldFromSource;
        frame = edge->parent;
    }

    frame = target;
    RigidTransform worldFromTarget;
    QSet<QString> seen;
    while (!seen.contains(frame)) {
        const auto item = sourceChain.constFind(frame);
        if (item != sourceChain.cend())
            return worldFromTarget.inverted() * item.value();
        seen.insert(frame);
        const auto edge = edges_.constFind(frame);
        const auto transform = edge == edges_.cend() ? std::nullopt : edge->at(stamp);
        if (!transform)
            break;
        worldFromTarget = *transform * worldFromTarget;
        frame = edge->parent;
    }
    return {};
}
}
