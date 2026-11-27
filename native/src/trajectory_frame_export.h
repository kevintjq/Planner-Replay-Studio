#pragma once

#include "trajectory_image.h"
#include <QVariantMap>

namespace replay {
struct TrajectoryBatchResult {
    QString error, directory;
    int completed = 0;
    bool cancelled = false;
};

// 后台使用不可变编辑快照，不改变预览或界面状态。
TrajectoryBatchResult exportTrajectoryBatch(const QString &source, const QString &parentDirectory,
    std::shared_ptr<const FixedCameraTrajectory> trajectory, const std::vector<size_t> &samples,
    const TrajectoryEdits &edits, double opacity, double currentTime, const QVariantMap &options,
    const TrackingCancelled &cancel = {}, const TrackingProgress &progress = {});
}
