#pragma once

#include "replay_data.h"

#include <QMatrix4x4>
#include <QSize>

namespace replay {
// This is shared camera math, not a second scene renderer. It has no frame-history
// state, so seeking, playback and offscreen output resolve the same experiment time.
struct CameraFollowSettings {
    int camera = 0;
    float yaw = 0, pitch = 0, scale = 1, panX = 0, panY = 0;
    QString targetFrame;
    int followMode = 0; // 0 preset, 1 position, 2 position+yaw, 3 full pose
    double followLag = 0, smoothing = 0; // experiment seconds, not wall-clock time
    bool opticalFrame = false; // ROS optical axes: +Z forward, -Y up
};

struct CameraFollowFrame {
    QMatrix4x4 matrix;
    QVector3D eye, target, up;
    std::optional<Pose> targetPose;
    qint64 targetStamp = 0;
    QString status;
    bool following = false;
};

CameraFollowFrame cameraFollowFrame(const Recording &recording, qint64 stamp,
                                   const QSize &size, const CameraFollowSettings &settings);
}
