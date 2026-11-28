#pragma once

#include "trajectory_edits.h"

#include <QImage>
#include <QPoint>
#include <QPainterPath>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QTemporaryDir;
class QPainter;

namespace replay {

struct TrajectoryFrame {
    double seconds = 0.;
    QRect bounds;
    QImage sprite;
    QString cachedSprite;
    QString foregroundError;
    double foregroundFraction = 0.;
    qint64 alphaPixels = -1;
    QImage sourceCrop;
    QString cachedSource;
};

struct FixedCameraTrajectory {
    QImage background;
    std::vector<TrajectoryFrame> frames;
    std::optional<double> lostAt;
    std::shared_ptr<QTemporaryDir> cache;
    QString backgroundWarning;
    int registeredFrames = 0, registrationRejected = 0;
};

struct ForegroundExtraction {
    QImage sprite;
    QString error;
    double foregroundFraction = 0.;
};

// A white coverage pixel means the reference contains an observed, unoccluded
// background pixel. Failed extraction returns a transparent sprite and a reason.
// Optional target seeds are actual observed target-appearance pixels. A supplied
// but unsupported seed mask fails; it never falls back to the complete ROI.
ForegroundExtraction extractTrajectoryForeground(const QImage &source, const QImage &background,
                                                 const QImage &backgroundCoverage = {},
                                                 const QImage &targetSeeds = {});

using TrackingProgress = std::function<void(double)>;
using TrackingCancelled = std::function<bool()>;

struct TrajectoryTrackingOptions {
    QString backgroundFile;
    std::optional<double> backgroundSeconds;
    bool registerCamera = false;
};

struct TrajectoryCompositionStats {
    int tracked = 0, sampled = 0, successful = 0, failed = 0, filtered = 0, composed = 0, lineSegments = 0, paintedLayers = 0;
    QString exportError(const TrajectoryEdits &edits) const;
};

struct TrajectoryInstance {
    size_t frameIndex = 0;
    qint64 timeNs = 0;
    QRectF placement;
    double opacity = 1.;
};
struct TrajectoryEndpoint {
    QPointF center;
    qint64 timeNs = 0;
};
QString trajectoryEndpointCaption(const TrajectoryEndpoint &point, const TrajectoryEndpointStyle &style);
QRectF trajectoryEndpointBounds(const TrajectoryEndpoint &point, const TrajectoryEndpointStyle &style);
// Source-pixel coordinates and actual source timestamps; no UI decoration.
// Descriptors reference immutable tracking caches rather than decoding video or
// retaining a second full set of source/sprite images.
struct TrajectoryLayerData {
    QSize canvasSize;
    double opacity = 1.;
    std::vector<TrajectoryInstance> instances;
    QPainterPath linePath;
    std::optional<TrajectoryEndpoint> start, goal;
    TrajectoryCompositionStats counts;
};
enum TrajectoryPaintPart { TrajectoryBackground = 1, TrajectoryInstances = 2, TrajectoryPath = 4,
    TrajectoryStart = 8, TrajectoryGoal = 16, TrajectoryAll = 31 };

TrajectoryLayerData trajectoryLayers(const FixedCameraTrajectory &trajectory, const std::vector<size_t> &sampled,
    double opacity, const QSet<int> &excluded = {}, const TrajectoryEdits &edits = {}, const TrackingCancelled &cancel = {});
QVariantList trajectoryCanvasLayerRows(const FixedCameraTrajectory &trajectory, const TrajectoryLayerData &layers,
    const TrajectoryEdits &edits, double opacity, const QVariantMap &canvasState = {});
// The path, circles and captions stay as painter primitives in PDF/SVG; only
// observed background and real video foreground instances are raster images.
void paintTrajectoryLayers(QPainter &painter, const FixedCameraTrajectory &trajectory, const TrajectoryLayerData &layers,
    const QRectF &target, const TrajectoryEdits &edits = {}, const TrackingCancelled &cancel = {}, int parts = TrajectoryAll,
    const QVariantMap &canvasState = {}, double time = -1., TrajectoryCompositionStats *paintedStats = nullptr,
    const QStringList &selectedLayers = {});
void paintTrajectoryLayer(QPainter &painter, const FixedCameraTrajectory &trajectory, const TrajectoryLayerData &layers,
    const QString &layerId, const QRectF &target, const TrajectoryEdits &edits = {}, const QVariantMap &canvasState = {},
    const TrackingCancelled &cancel = {});

qint64 trajectoryAlphaPixels(const QImage &image);
QImage trajectorySprite(const TrajectoryFrame &frame, const TrajectoryFrameEdit &edit = {});
QImage trajectorySourceCrop(const TrajectoryFrame &frame);

QImage readTrajectoryFrame(const QString &file, double seconds);
FixedCameraTrajectory trackTrajectory(const QString &file, double start, std::optional<double> end,
                                      const QRectF &selection, const TrackingCancelled &cancel = {},
                                      const TrackingProgress &progress = {}, const TrajectoryTrackingOptions &options = {});
std::vector<size_t> sampleTrajectory(const FixedCameraTrajectory &trajectory, double interval);
QImage renderTrajectory(const FixedCameraTrajectory &trajectory, const std::vector<size_t> &sampled,
                        double opacity, const QSet<int> &excluded, QSize limit = {},
                        const TrajectoryEdits &edits = {}, const TrackingCancelled &cancel = {}, bool exactOutputSize = false,
                        TrajectoryCompositionStats *stats = nullptr, bool requireForeground = false, const QVariantMap &canvasState = {}, double time = -1.);

std::vector<size_t> editedTrajectorySamples(const FixedCameraTrajectory &trajectory, double interval,
                                          int mode, int count, const TrajectoryEdits &edits);

}
