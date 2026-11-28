#pragma once

#include "replay_data.h"
#include <QVariantMap>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace replay {
enum class VelocityMode { Total, Horizontal, Vertical, X, Y, Z, Custom };
VelocityMode velocityMode(const QString &name);
QString velocityModeName(VelocityMode mode);
double speedDisplayScale(const QString &unit);

struct SpeedSample {
    qint64 stampNs = 0;
    std::array<double, 3> velocity{}; // SI m/s, never display units.
    double scalar = std::numeric_limits<double>::quiet_NaN();
    bool valid = false;
    QString frame;
    bool breakBefore = false;
};
struct SpeedSeries {
    std::vector<SpeedSample> samples;
    QVariantMap provenance;
    bool vector = true, scalarIsSpeed = false;
    qint64 maximumGapNs = 500'000'000;
};
struct SpeedRange {
    qint64 begin = 0, end = 0;
};
std::vector<SpeedRange> mergeSpeedRanges(std::vector<SpeedRange> ranges);
std::vector<SpeedRange> subtractSpeedRanges(const std::vector<SpeedRange> &included,
                                           const std::vector<SpeedRange> &excluded);
std::vector<SpeedRange> speedRangesFromVariants(const QVariantList &values);
QVariantList speedRangesToVariants(const std::vector<SpeedRange> &ranges);

// One real Bag pass. Fields are inferred only for known standard message types.
// Custom fields require an explicit inputUnit and explicit time basis.
SpeedSeries loadSpeedSeries(const QString &path, const QVariantMap &source,
                            std::shared_ptr<const TransformTree> transforms = {}, const Cancel &cancel = {});
SpeedSeries speedSeriesFromRecording(const Recording &recording);
SpeedSeries filteredSpeedSeries(const SpeedSeries &source, double timeConstantSeconds);

// Immutable scalar index. Current values use binary search; integrals use prefix
// sums, extrema use blocks. Plot decimation never enters this index.
class SpeedIndex {
public:
    SpeedIndex(const SpeedSeries &series, VelocityMode mode);
    std::optional<double> valueAt(qint64 stampNs) const;
    QVariantMap statistics(const std::vector<SpeedRange> &included = {},
                           const std::vector<SpeedRange> &excluded = {}) const;
    QVariantList plot(const std::vector<SpeedRange> &included = {}, int maximumPoints = 2000) const;
    qint64 firstStamp() const { return stamps_.empty() ? 0 : stamps_.front(); }
    qint64 lastStamp() const { return stamps_.empty() ? 0 : stamps_.back(); }
    qsizetype size() const { return qsizetype(stamps_.size()); }
    bool distanceAvailable() const { return distanceAvailable_; }
    const std::vector<qint64> &stamps() const { return stamps_; }
    const std::vector<double> &values() const { return values_; }
private:
    struct Primitive { double duration = 0., integral = 0., distance = 0.; };
    struct Block { double low = std::numeric_limits<double>::infinity(), high = -std::numeric_limits<double>::infinity(); qint64 count = 0; };
    Primitive primitiveAt(qint64 stampNs) const;
    std::vector<qint64> stamps_;
    std::vector<double> values_, speeds_, duration_, integral_, distance_;
    std::vector<bool> validEdges_;
    std::vector<Block> blocks_;
    qint64 maximumGap_ = 500'000'000;
    bool distanceAvailable_ = false;
    VelocityMode mode_;
};
}
