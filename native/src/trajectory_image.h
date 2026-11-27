#pragma once

#include "trajectory_edits.h"

#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QSet>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QTemporaryDir;

namespace replay {

struct TrajectoryFrame {
    double seconds = 0.;
    QRect bounds;
    QImage sprite;
    QString cachedSprite;
};

struct FixedCameraTrajectory {
    QImage background;
    std::vector<TrajectoryFrame> frames;
    std::optional<double> lostAt;
    std::shared_ptr<QTemporaryDir> cache;
};

using TrackingProgress = std::function<void(double)>;
using TrackingCancelled = std::function<bool()>;

QImage readTrajectoryFrame(const QString &file, double seconds);
FixedCameraTrajectory trackTrajectory(const QString &file, double start, std::optional<double> end,
                                      const QRectF &selection, const TrackingCancelled &cancel = {},
                                      const TrackingProgress &progress = {});
std::vector<size_t> sampleTrajectory(const FixedCameraTrajectory &trajectory, double interval);
QImage renderTrajectory(const FixedCameraTrajectory &trajectory, const std::vector<size_t> &sampled,
                        double opacity, const QSet<int> &excluded, QSize limit = {},
                        const TrajectoryEdits &edits = {}, const TrackingCancelled &cancel = {}, bool exactOutputSize = false);

std::vector<size_t> editedTrajectorySamples(const FixedCameraTrajectory &trajectory, double interval,
                                          int mode, int count, const TrajectoryEdits &edits);

}
