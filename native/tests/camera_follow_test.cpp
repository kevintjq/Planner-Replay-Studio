#include "camera_follow.h"
#include "transform_tree.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
constexpr qint64 Second = 1'000'000'000;
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
bool near(const QVector3D &a, const QVector3D &b, float tolerance = .0002f)
{
    return (a - b).length() <= tolerance;
}
bool near(const QMatrix4x4 &a, const QMatrix4x4 &b, float tolerance = .00001f)
{
    for (int i = 0; i < 16; ++i)
        if (std::abs(a.constData()[i] - b.constData()[i]) > tolerance) return false;
    return true;
}
bool finite(const QMatrix4x4 &matrix)
{
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(matrix.constData()[i])) return false;
    return true;
}

replay::Recording recording()
{
    replay::Recording result;
    result.frameId = "map";
    result.bodyFrame = "body";
    result.maximumGap = 11 * Second;
    result.radius = 10;
    result.frames = {{0, 0, 0}, {10 * Second, 0, 0}};
    result.poses = {{0, {100, 0, 0}, {}, {}}, {10 * Second, {110, 0, 0}, {}, {}}};
    auto transforms = std::make_shared<replay::TransformTree>(11 * Second);
    transforms->add("map", "vehicle", 0, {{0, 0, 0}, {}}, false);
    transforms->add("map", "vehicle", 10 * Second, {{10, 0, 0}, {}}, false);
    transforms->add("map", "body", 0, {{20, 0, 0}, {}}, false);
    transforms->add("map", "body", 10 * Second, {{30, 0, 0}, {}}, false);
    transforms->add("vehicle", "gimbal", 0, {{0, 2, 1}, QQuaternion::fromEulerAngles(25, 35, 80)}, true);
    transforms->add("map", "optical", 0, {{0, 0, 5}, {}}, true);
    transforms->add("other_root", "disconnected", 0, {{3, 4, 5}, {}}, true);
    transforms->add("map", "wrap", 0, {{}, QQuaternion::fromAxisAndAngle(0, 0, 1, 179)}, false);
    transforms->add("map", "wrap", 10 * Second, {{}, QQuaternion::fromAxisAndAngle(0, 0, 1, -179)}, false);
    transforms->prepare();
    result.transforms = transforms;
    return result;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        auto data = recording();
        replay::CameraFollowSettings settings;
        settings.camera = 1;
        const QSize size(1920, 1080);
        const auto resolve = [&](qint64 stamp) { return replay::cameraFollowFrame(data, stamp, size, settings); };
        const auto body = resolve(5 * Second);
        require(body.following && near(body.targetPose->position, {105, 0, 0}), "default body pose");
        settings.targetFrame = "body";
        const auto explicitBody = resolve(5 * Second);
        require(explicitBody.following && near(explicitBody.targetPose->position, {25, 0, 0}),
                "explicit body frame must use real TF instead of different odometry pose");
        settings.targetFrame = "/gimbal";
        const auto mount = resolve(5 * Second);
        require(mount.following && near(mount.targetPose->position, {5, 2, 1}), "arbitrary mounted TF target");
        require(!near(body.eye, mount.eye) && !near(body.matrix, mount.matrix), "target must change rendered camera");

        settings.targetFrame = "vehicle";
        settings.followLag = 1;
        const auto lag = resolve(5 * Second);
        require(lag.targetStamp == 4 * Second && near(lag.targetPose->position, {4, 0, 0}), "lag uses experiment timestamp");
        settings.smoothing = 2;
        const auto smooth = resolve(5 * Second);
        require(near(smooth.targetPose->position, {3, 0, 0}), "uniform trailing smoothing window");
        for (const qint64 stamp : {9 * Second, Second, 7 * Second, 2 * Second, 8 * Second, 0LL})
            (void)resolve(stamp);
        const auto sought = resolve(5 * Second);
        require(near(smooth.eye, sought.eye) && near(smooth.matrix, sought.matrix), "random seeks must not change camera");

        settings.smoothing = settings.followLag = 0;
        for (const auto &target : {QString("missing"), QString("disconnected")}) {
            settings.targetFrame = target;
            const auto missing = resolve(5 * Second);
            require(!missing.following && !missing.targetPose && !missing.status.isEmpty()
                    && finite(missing.matrix), "unavailable target reports overview fallback");
        }
        settings.targetFrame = "vehicle";
        require(!resolve(11 * Second).following, "no TF extrapolation beyond recorded coverage");
        settings.followLag = 6;
        require(!resolve(5 * Second).following, "lag before coverage must not reuse latest TF");
        settings.followLag = 0;
        settings.targetFrame = "map";
        require(resolve(5 * Second).following && near(resolve(5 * Second).targetPose->position, {}), "known fixed frame identity");

        settings.targetFrame = "wrap";
        const auto wrap = resolve(5 * Second);
        require(wrap.following && wrap.targetPose->rotation.rotatedVector({1, 0, 0}).x() < -.999f,
                "quaternion wrap must take shortest arc");
        settings.smoothing = 2;
        require(resolve(5 * Second).targetPose->rotation.rotatedVector({1, 0, 0}).x() < -.999f,
                "quaternion smoothing must preserve hemisphere");

        settings.smoothing = 0;
        settings.camera = 2;
        settings.targetFrame = "gimbal";
        settings.followMode = 1;
        const auto position = resolve(5 * Second);
        settings.followMode = 2;
        const auto yaw = resolve(5 * Second);
        settings.followMode = 3;
        const auto full = resolve(5 * Second);
        require(near(position.target, yaw.target) && near(yaw.target, full.target), "modes retain target position");
        require(!near(position.eye, yaw.eye) && !near(yaw.eye, full.eye), "position, yaw and full pose differ");
        require(near(position.up, {0, 0, 1}) && near(yaw.up, {0, 0, 1}) && !near(full.up, yaw.up), "full pose follows roll/pitch");

        settings.camera = 4;
        settings.targetFrame = "optical";
        settings.opticalFrame = true;
        const auto optical = resolve(5 * Second);
        require(near(optical.target - optical.eye, {0, 0, 1}) && near(optical.up, {0, -1, 0}), "optical Z-forward Y-down");
        settings.opticalFrame = false;
        const auto standard = resolve(5 * Second);
        require(near(standard.target - standard.eye, {1, 0, 0}) && near(standard.up, {0, 0, 1}), "standard X-forward Z-up");

        settings.camera = 0;
        settings.targetFrame = "missing";
        const auto overview = resolve(5 * Second);
        settings.targetFrame = "gimbal";
        require(!overview.following && near(overview.matrix, resolve(5 * Second).matrix), "overview independent of target");
        settings.camera = 1;
        settings.scale = std::numeric_limits<float>::quiet_NaN();
        const auto invalid = resolve(5 * Second);
        require(!invalid.following && finite(invalid.matrix) && invalid.status.contains("Invalid"), "nonfinite config diagnosed");

        data.transforms.reset();
        settings = {};
        settings.camera = 1;
        require(resolve(5 * Second).following, "empty target preserves odometry-only selected pose source");
        settings.targetFrame = "body";
        require(!resolve(5 * Second).following, "explicit body without a TF chain must not reuse odometry source");
        settings.targetFrame = "gimbal";
        require(!resolve(5 * Second).following, "no TF must not pretend target exists");

        std::cout << "Camera follow: arbitrary/static TF, lag/smoothing, missing TF, quaternion wrap, modes, optical axes, random seeks and invalid input passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
