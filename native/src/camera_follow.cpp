#include "camera_follow.h"

#include "camera_presets.h"
#include "transform_tree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace replay {
namespace {
QString normalizedFrame(QString frame)
{
    frame = frame.trimmed();
    while (frame.startsWith('/')) frame.remove(0, 1);
    return frame;
}

bool finite(const QVector3D &v)
{
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

bool validPose(const Pose &pose)
{
    const auto &q = pose.rotation;
    const auto norm = q.lengthSquared();
    return finite(pose.position) && std::isfinite(q.scalar()) && std::isfinite(q.x())
        && std::isfinite(q.y()) && std::isfinite(q.z()) && std::isfinite(norm) && norm > 1e-12f;
}

qint64 offsetStamp(qint64 stamp, qint64 offset)
{
    if (offset < 0 && stamp < std::numeric_limits<qint64>::min() - offset)
        return std::numeric_limits<qint64>::min();
    if (offset > 0 && stamp > std::numeric_limits<qint64>::max() - offset)
        return std::numeric_limits<qint64>::max();
    return stamp + offset;
}

std::optional<Pose> targetAt(const Recording &recording, const QString &requested, qint64 stamp)
{
    std::optional<Pose> pose;
    const auto target = normalizedFrame(requested);
    // Empty means the user's selected pose source. An explicit frame must follow
    // that actual TF chain, even when it has the same name as the odometry child.
    if (target.isEmpty())
        pose = recording.poseAt(stamp);
    if (!pose && !target.isEmpty()) {
        if (target == normalizedFrame(recording.frameId)) {
            pose = Pose{stamp, {}, QQuaternion(), {}};
        } else if (recording.transforms) {
            // Match canonical frame names without changing the immutable TF tree.
            auto source = target;
            auto fixed = recording.frameId;
            for (const auto &frame : recording.transforms->frames()) {
                if (normalizedFrame(frame) == target) { source = frame; break; }
            }
            for (const auto &frame : recording.transforms->frames())
                if (normalizedFrame(frame) == normalizedFrame(fixed)) { fixed = frame; break; }
            const auto transform = recording.transforms->lookup(fixed, source, stamp);
            if (transform)
                pose = Pose{stamp, transform->translation, transform->rotation, {}};
        }
    }
    if (!pose || !validPose(*pose)) return {};
    pose->rotation.normalize();
    return pose;
}

Pose smoothedTarget(const Recording &recording, const QString &frame, qint64 stamp,
                    double seconds, const Pose &current)
{
    if (seconds <= 0) return current;
    // Uniform trailing experiment-time samples: deterministic after random seeks.
    // Missing older samples truncate the window; the current target must exist.
    QVector3D position;
    QQuaternion reference = current.rotation;
    double w = 0, x = 0, y = 0, z = 0;
    int count = 0;
    for (int i = 0; i < 17; ++i) {
        const auto sample = targetAt(recording, frame,
            offsetStamp(stamp, -qint64(std::llround(seconds * 1.e9 * i / 16.))));
        if (!sample) continue;
        const auto q = sample->rotation;
        const double sign = QQuaternion::dotProduct(reference, q) < 0 ? -1. : 1.;
        position += sample->position;
        w += sign * q.scalar(); x += sign * q.x(); y += sign * q.y(); z += sign * q.z();
        ++count;
    }
    if (!count) return current;
    auto result = current;
    result.position = position / float(count);
    const QQuaternion average{float(w), float(x), float(y), float(z)};
    if (average.lengthSquared() > 1e-12f) result.rotation = average.normalized();
    return result;
}
}

CameraFollowFrame cameraFollowFrame(const Recording &recording, qint64 stamp,
                                   const QSize &size, const CameraFollowSettings &settings)
{
    CameraFollowFrame result;
    const bool valid = settings.camera >= 0 && size_t(settings.camera) < CameraPresets.size()
        && settings.followMode >= 0 && settings.followMode <= 3
        && std::isfinite(settings.yaw) && std::isfinite(settings.pitch)
        && std::isfinite(settings.scale) && settings.scale > 0
        && std::isfinite(settings.panX) && std::isfinite(settings.panY)
        && std::isfinite(settings.followLag) && settings.followLag >= 0 && settings.followLag <= 10
        && std::isfinite(settings.smoothing) && settings.smoothing >= 0 && settings.smoothing <= 10;
    const auto config = valid ? settings : CameraFollowSettings{};
    const auto &preset = CameraPresets[size_t(config.camera)];
    auto mode = preset.mode;
    result.targetStamp = offsetStamp(stamp, -qint64(std::llround(config.followLag * 1.e9)));
    if (!valid) {
        result.status = QStringLiteral("相机参数无效 · Invalid camera settings; overview");
    } else if (mode == CameraPreset::OrbitMap) {
        result.status = QStringLiteral("地图总览 · Map overview");
    } else {
        auto pose = targetAt(recording, config.targetFrame, result.targetStamp);
        if (pose) {
            result.targetPose = smoothedTarget(recording, config.targetFrame, result.targetStamp,
                                                config.smoothing, *pose);
            result.following = true;
            const auto label = config.targetFrame.isEmpty()
                ? (recording.bodyFrame.isEmpty() ? QStringLiteral("Odometry") : recording.bodyFrame)
                : config.targetFrame;
            result.status = QStringLiteral("跟随 · Following %1").arg(label);
        } else {
            mode = CameraPreset::OrbitMap;
            result.status = QStringLiteral("目标无有效位姿 / TF · No valid target pose/TF: %1; overview")
                .arg(config.targetFrame.isEmpty() ? QStringLiteral("current pose") : config.targetFrame);
        }
    }
    const auto &geometry = mode == CameraPreset::OrbitMap ? CameraPresets.front() : preset;
    const float ratio = float(std::max(1, size.width())) / std::max(1, size.height());
    const float radius = std::isfinite(recording.radius) ? std::max(.01f, recording.radius) : 1.f;
    const float scale = std::clamp(config.scale, .02f, 16.f);
    QMatrix4x4 projection, view;
    QQuaternion orientation;
    if (result.targetPose) orientation = result.targetPose->rotation;
    const QVector3D localForward = config.opticalFrame ? QVector3D(0, 0, 1) : QVector3D(1, 0, 0);
    const QVector3D localUp = config.opticalFrame ? QVector3D(0, -1, 0) : QVector3D(0, 0, 1);
    const auto targetForward = orientation.rotatedVector(localForward);
    if (config.followMode == 1) {
        orientation = QQuaternion();
    } else if (config.followMode == 2) {
        const float heading = std::atan2(targetForward.y(), targetForward.x()) * 57.2957795131f;
        orientation = QQuaternion::fromAxisAndAngle(0, 0, 1, heading);
    }
    auto forwardAxis = config.followMode == 1 ? QVector3D(1, 0, 0)
        : config.followMode == 2 ? orientation.rotatedVector({1, 0, 0}) : orientation.rotatedVector(localForward);
    auto upAxis = config.followMode == 1 || config.followMode == 2 ? QVector3D(0, 0, 1)
        : orientation.rotatedVector(localUp);
    if (mode == CameraPreset::FirstPerson || mode == CameraPreset::FirstPersonRaw) {
        if (mode == CameraPreset::FirstPerson && config.followMode == 0) {
            if (config.smoothing == 0) {
                const auto before = targetAt(recording, config.targetFrame, offsetStamp(result.targetStamp, -120'000'000));
                const auto after = targetAt(recording, config.targetFrame, offsetStamp(result.targetStamp, 120'000'000));
                if (before && after) orientation = QQuaternion::slerp(before->rotation, after->rotation, .5f);
                forwardAxis = orientation.rotatedVector(localForward);
                upAxis = orientation.rotatedVector(localUp);
            }
            const QVector3D tangent(-forwardAxis.y(), forwardAxis.x(), 0);
            if (tangent.lengthSquared() > 1e-6f)
                upAxis = QVector3D::crossProduct(forwardAxis, tangent).normalized();
        }
        auto forward = QQuaternion::fromAxisAndAngle(upAxis, config.yaw).rotatedVector(forwardAxis);
        const auto side = QVector3D::crossProduct(upAxis, forward).normalized();
        const auto tilt = QQuaternion::fromAxisAndAngle(side, -config.pitch);
        forward = tilt.rotatedVector(forward);
        result.up = tilt.rotatedVector(upAxis);
        result.eye = result.targetPose->position + .12f * forwardAxis
            + .4f * (-side * config.panX + result.up * config.panY);
        result.target = result.eye + forward;
        projection.perspective(std::clamp(65.f * scale, 12.f, 120.f), ratio, .035f, std::max(70.f, radius * 8.f));
    } else {
        constexpr float Radians = .01745329252f;
        float yaw = (config.yaw + geometry.yaw) * Radians;
        const float pitch = std::clamp(geometry.pitch + config.pitch, -75.f, 86.f) * Radians;
        result.target = finite(recording.center) ? recording.center : QVector3D();
        result.up = {0, 0, 1};
        float distance = radius * 2.65f * scale;
        const bool fullPose = mode != CameraPreset::OrbitMap && config.followMode == 3;
        if (mode != CameraPreset::OrbitMap) {
            if (!fullPose) yaw += std::atan2(forwardAxis.y(), forwardAxis.x());
            if (mode == CameraPreset::OrbitAircraft) {
                constexpr double Circle = 6.283185307179586;
                const qint64 origin = recording.frames.empty() ? stamp : recording.frames.front().stamp;
                const double elapsed = double((static_cast<long double>(stamp) - origin) / 1.e9L);
                yaw += float(std::fmod(elapsed, 9.) * Circle / 9.);
            }
            result.target = result.targetPose->position
                + forwardAxis * (geometry.id == std::string_view("rear") ? 3.f : 0.f);
            distance = std::clamp(radius * .5f, 6.f, 20.f) * scale;
        }
        QVector3D right(-std::sin(yaw), std::cos(yaw), 0);
        QVector3D direction(std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch));
        if (fullPose) {
            const auto side = QVector3D::crossProduct(upAxis, forwardAxis).normalized();
            right = -forwardAxis * std::sin(yaw) + side * std::cos(yaw);
            direction = forwardAxis * direction.x() + side * direction.y() + upAxis * direction.z();
            result.up = upAxis;
        }
        result.target += distance * (-right * config.panX + result.up * config.panY);
        result.eye = result.target + direction * distance;
        projection.perspective(46.f, ratio, std::max(.01f, distance * .002f),
                               std::max(50.f, distance + radius * 5.f));
    }
    view.lookAt(result.eye, result.target, result.up);
    result.matrix = projection * view;
    return result;
}
}
