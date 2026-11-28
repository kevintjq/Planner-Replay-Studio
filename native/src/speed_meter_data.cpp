#include "speed_meter_data.h"
#include "transform_tree.h"
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace replay {
namespace {
constexpr double Second = 1'000'000'000.;
constexpr size_t BlockSize = 256;
constexpr double Missing = std::numeric_limits<double>::quiet_NaN();
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
QString frameName(QString value) { value = value.trimmed(); while (value.startsWith('/')) value.remove(0, 1); return value; }
bool contains(const std::vector<SpeedRange> &ranges, qint64 stamp)
{
    const auto after = std::upper_bound(ranges.begin(), ranges.end(), stamp,
        [](qint64 t, const SpeedRange &range) { return t < range.begin; });
    return after != ranges.begin() && stamp <= std::prev(after)->end;
}
double component(const SpeedSeries &series, const SpeedSample &sample, VelocityMode mode)
{
    if (!sample.valid) return Missing;
    if (!series.vector) return mode == VelocityMode::Custom || (mode == VelocityMode::Total && series.scalarIsSpeed) ? sample.scalar : Missing;
    const auto &v = sample.velocity;
    switch (mode) {
    case VelocityMode::Total: return std::hypot(v[0], v[1], v[2]);
    case VelocityMode::Horizontal: return std::hypot(v[0], v[1]);
    case VelocityMode::Vertical: case VelocityMode::Z: return v[2];
    case VelocityMode::X: return v[0];
    case VelocityMode::Y: return v[1];
    case VelocityMode::Custom: return Missing;
    }
    return Missing;
}
QStringList standardFields(const QString &type, bool positions)
{
    QString prefix;
    if (positions) {
        if (type == "nav_msgs/Odometry" || type == "geometry_msgs/PoseWithCovarianceStamped") prefix = "pose.pose.position";
        else if (type == "geometry_msgs/PoseStamped") prefix = "pose.position";
        else return {};
    } else {
        if (type == "nav_msgs/Odometry" || type == "geometry_msgs/TwistWithCovarianceStamped") prefix = "twist.twist.linear";
        else if (type == "geometry_msgs/TwistStamped" || type == "geometry_msgs/TwistWithCovariance") prefix = "twist.linear";
        else if (type == "geometry_msgs/Twist") prefix = "linear";
        else if (type == "geometry_msgs/Vector3Stamped") prefix = "vector";
        else if (type == "geometry_msgs/Vector3") prefix.clear();
        else return {};
    }
    return {prefix.isEmpty() ? "x" : prefix + ".x", prefix.isEmpty() ? "y" : prefix + ".y", prefix.isEmpty() ? "z" : prefix + ".z"};
}
std::array<double, 3> rotate(const QQuaternion &rotation, const std::array<double, 3> &v)
{
    // Compute in float64 so raw velocity precision is not first truncated to QVector3D.
    const auto q = rotation.normalized();
    const std::array<double, 3> u{q.x(), q.y(), q.z()}; const double w = q.scalar();
    const std::array<double, 3> cross{u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
    const std::array<double, 3> cross2{u[1]*cross[2]-u[2]*cross[1], u[2]*cross[0]-u[0]*cross[2], u[0]*cross[1]-u[1]*cross[0]};
    return {v[0]+2.*(w*cross[0]+cross2[0]), v[1]+2.*(w*cross[1]+cross2[1]), v[2]+2.*(w*cross[2]+cross2[2])};
}
}

VelocityMode velocityMode(const QString &name)
{
    if (name.isEmpty() || name == "total" || name == "3d") return VelocityMode::Total;
    if (name == "horizontal") return VelocityMode::Horizontal;
    if (name == "vertical") return VelocityMode::Vertical;
    if (name == "x") return VelocityMode::X;
    if (name == "y") return VelocityMode::Y;
    if (name == "z") return VelocityMode::Z;
    if (name == "custom") return VelocityMode::Custom;
    throw std::runtime_error("Unknown velocity mode.");
}
QString velocityModeName(VelocityMode mode)
{
    switch (mode) {
    case VelocityMode::Total: return "total";
    case VelocityMode::Horizontal: return "horizontal";
    case VelocityMode::Vertical: return "vertical";
    case VelocityMode::X: return "x";
    case VelocityMode::Y: return "y";
    case VelocityMode::Z: return "z";
    case VelocityMode::Custom: return "custom";
    }
    return {};
}
double speedDisplayScale(const QString &unit)
{
    if (unit.isEmpty() || unit == "m/s") return 1.;
    if (unit == "km/h") return 3.6;
    throw std::runtime_error("Speed display unit must be m/s or km/h.");
}
std::vector<SpeedRange> mergeSpeedRanges(std::vector<SpeedRange> ranges)
{
    for (const auto &range : ranges) check(range.begin >= 0 && range.end >= range.begin, "Invalid exact nanosecond statistics interval.");
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.begin < b.begin; });
    std::vector<SpeedRange> result;
    for (auto range : ranges) {
        if (result.empty() || result.back().end < range.begin) result.push_back(range);
        else result.back().end = std::max(result.back().end, range.end);
    }
    return result;
}
std::vector<SpeedRange> subtractSpeedRanges(const std::vector<SpeedRange> &included, const std::vector<SpeedRange> &excluded)
{
    const auto a = mergeSpeedRanges(included), b = mergeSpeedRanges(excluded);
    std::vector<SpeedRange> result;
    for (const auto &range : a) {
        auto cursor = range.begin;
        for (const auto &remove : b) {
            if (remove.end < cursor || remove.begin > range.end) continue;
            if (remove.begin > cursor) result.push_back({cursor, std::min(remove.begin, range.end)});
            cursor = std::max(cursor, remove.end);
            if (cursor >= range.end) break;
        }
        if (cursor < range.end || (range.begin == range.end && !contains(b, cursor))) result.push_back({cursor, range.end});
    }
    return result;
}
std::vector<SpeedRange> speedRangesFromVariants(const QVariantList &values)
{
    std::vector<SpeedRange> ranges;
    for (const auto &value : values) {
        const auto row = value.toMap(); bool firstOk = false, lastOk = false;
        const auto first = row.value("beginNs").toString().toLongLong(&firstOk);
        const auto last = row.value("endNs").toString().toLongLong(&lastOk);
        check(firstOk && lastOk, "Statistics ranges require exact beginNs/endNs strings.");
        ranges.push_back({first, last});
    }
    return mergeSpeedRanges(std::move(ranges));
}
QVariantList speedRangesToVariants(const std::vector<SpeedRange> &ranges)
{
    QVariantList result;
    for (const auto &range : ranges) result.append(QVariantMap{{"beginNs", QString::number(range.begin)}, {"endNs", QString::number(range.end)}});
    return result;
}

SpeedSeries loadSpeedSeries(const QString &path, const QVariantMap &source,
                            std::shared_ptr<const TransformTree> transforms, const Cancel &cancel)
{
    const auto topic = source.value("topic").toString(), requestedType = source.value("type").toString();
    const auto basis = source.value("timeBasis").toString();
    check(basis == "header" || basis == "storage" || basis == "bag-storage", "Choose an explicit header or storage speed time basis.");
    const auto method = source.value("method", "direct").toString();
    check(method == "direct" || method == "position-difference", "Unsupported speed data method.");
    const bool positions = method == "position-difference";
    const auto target = frameName(source.value("targetFrame").toString());
    SpeedSeries result;
    const double gap = source.value("maximumGapSeconds", .5).toDouble();
    check(std::isfinite(gap) && gap > 0. && gap <= 3600., "Speed maximumGapSeconds must be positive and at most 3600.");
    result.maximumGapNs = qint64(std::llround(gap * Second));
    qint64 messages = 0, missingHeader = 0, invalid = 0, missingTf = 0, duplicates = 0;
    QString firstError, actualType; QStringList actualFields;
    bool configured = false, pendingGap = false; double scale = 1.;
    const auto explicitlyNamedFrame = frameName(source.value("sourceFrame").toString());
    messages = visitBagTopic(path, topic, [&](const QByteArray &type, const QByteArray &definition, QByteArrayView bytes, qint64 stored) {
        if (!configured) {
            actualType = QString::fromUtf8(type); actualFields = source.value("fields").toStringList();
            if (actualFields.isEmpty() && !source.value("fieldPath").toString().isEmpty()) actualFields = {source.value("fieldPath").toString()};
            if (actualFields.isEmpty()) actualFields = standardFields(actualType, positions);
            check(actualFields.size() == 1 || actualFields.size() == 3, "Choose one scalar field or three actual vector fields.");
            result.vector = actualFields.size() == 3;
            result.scalarIsSpeed = source.value("scalarIsSpeed", false).toBool();
            check(!positions || result.vector, "Position differences require three position fields.");
            const auto unit = source.value("inputUnit").toString();
            if (positions) {
                check(unit == "m" || (unit.isEmpty() && actualFields == standardFields(actualType, true)), "Custom position fields require explicit inputUnit m.");
            } else if (unit == "m/s" || (unit.isEmpty() && actualFields == standardFields(actualType, false)
                && actualType != "geometry_msgs/Vector3" && actualType != "geometry_msgs/Vector3Stamped")) scale = 1.;
            else if (unit == "km/h") scale = 1. / 3.6;
            else check(false, "Custom velocity fields require explicit inputUnit m/s or km/h.");
            configured = true;
        }
        check(result.samples.size() < 5'000'000, "Speed full-resolution cache exceeds the five million sample resource limit; data were not decimated.");
        const auto headerText = topicMessageHeaderStamp(type, definition, bytes);
        bool headerOk = false; const auto header = headerText.toLongLong(&headerOk);
        if (basis == "header" && (!headerOk || header <= 0)) { ++missingHeader; pendingGap = true; return; }
        SpeedSample sample; sample.stampNs = basis == "header" ? header : stored;
        sample.breakBefore = pendingGap; pendingGap = false;
        try {
            std::array<double, 3> values{};
            for (qsizetype axis = 0; axis < actualFields.size(); ++axis) {
                const auto value = decodeTopicNumeric(type, definition, bytes, actualFields[axis]);
                check(bool(value), "Velocity field is non-finite or unavailable.");
                values[size_t(axis)] = value->value * scale;
                check(std::isfinite(values[size_t(axis)]), "Velocity unit conversion overflowed.");
            }
            QString frame = explicitlyNamedFrame;
            if (frame.isEmpty()) {
                if (actualType == "nav_msgs/Odometry" && !positions) frame = frameName(decodeTopicString(type, definition, bytes, "child_frame_id"));
                else if (actualType.endsWith("Stamped") || actualType == "nav_msgs/Odometry") frame = frameName(decodeTopicString(type, definition, bytes, "header.frame_id"));
            }
            if (!target.isEmpty() && target != frame) {
                check(result.vector, "A scalar field cannot be converted between directional frames.");
                if (!transforms || frame.isEmpty() || !headerOk || header <= 0) {
                    ++missingTf; throw std::runtime_error("Velocity TF conversion needs a known source frame, real Header timestamp and available TF.");
                }
                const auto transform = transforms->lookup(target, frame, header);
                if (!transform) { ++missingTf; throw std::runtime_error("Velocity TF is unavailable at the actual message Header timestamp."); }
                values = rotate(transform->rotation, values);
                if (positions) {
                    values[0] += transform->translation.x(); values[1] += transform->translation.y(); values[2] += transform->translation.z();
                }
                frame = target;
            }
            sample.velocity = values; sample.scalar = values[0]; sample.frame = frame; sample.valid = true;
            if (!result.vector && result.scalarIsSpeed && sample.scalar < 0.) throw std::runtime_error("A scalar speed magnitude cannot be negative.");
        } catch (const std::exception &error) {
            sample.valid = false; ++invalid; if (firstError.isEmpty()) firstError = QString::fromUtf8(error.what());
        }
        result.samples.push_back(sample); // Invalid samples remain interpolation barriers.
    }, requestedType, cancel);
    std::stable_sort(result.samples.begin(), result.samples.end(), [](const auto &a, const auto &b) { return a.stampNs < b.stampNs; });
    std::vector<SpeedSample> unique;
    unique.reserve(result.samples.size());
    for (const auto &sample : result.samples) {
        if (cancel && cancel()) throw std::runtime_error("Speed data loading cancelled.");
        if (!unique.empty() && unique.back().stampNs == sample.stampNs) {
            ++duplicates;
            auto &previous = unique.back();
            if (previous.valid != sample.valid || previous.velocity != sample.velocity || previous.frame != sample.frame) previous.valid = false;
        } else unique.push_back(sample);
    }
    result.samples = std::move(unique);
    if (positions) {
        const auto positionsData = result.samples;
        for (size_t i = 0; i < positionsData.size(); ++i) {
            auto &sample = result.samples[i]; sample.valid = false;
            if (!positionsData[i].valid) continue;
            size_t left = i, right = i;
            if (i && !positionsData[i].breakBefore && positionsData[i - 1].valid && positionsData[i].stampNs - positionsData[i - 1].stampNs <= result.maximumGapNs
                && positionsData[i - 1].frame == positionsData[i].frame) left = i - 1;
            if (i + 1 < positionsData.size() && !positionsData[i + 1].breakBefore && positionsData[i + 1].valid && positionsData[i + 1].stampNs - positionsData[i].stampNs <= result.maximumGapNs
                && positionsData[i + 1].frame == positionsData[i].frame) right = i + 1;
            if (left == right) continue;
            const double dt = double(positionsData[right].stampNs - positionsData[left].stampNs) / Second;
            for (size_t axis = 0; axis < 3; ++axis) sample.velocity[axis] = (positionsData[right].velocity[axis] - positionsData[left].velocity[axis]) / dt;
            sample.valid = std::all_of(sample.velocity.begin(), sample.velocity.end(), [](double v) { return std::isfinite(v); });
        }
    }
    const QFileInfo info(path);
    result.provenance = {{"source", QUrl::fromLocalFile(info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath()).toString()},
        {"topic", topic}, {"type", actualType}, {"fields", actualFields}, {"timeBasis", basis == "header" ? "header" : "bag-storage"},
        {"canonicalUnit", "m/s"}, {"inputUnit", source.value("inputUnit", positions ? "m" : "m/s")}, {"method", method},
        {"estimated", positions}, {"filtered", false}, {"targetFrame", target}, {"sourceFrameOverride", explicitlyNamedFrame},
        {"matchedMessages", messages}, {"sampleCount", qlonglong(result.samples.size())}, {"missingHeaderCount", missingHeader},
        {"invalidCount", invalid}, {"missingTfCount", missingTf}, {"duplicateTimestampCount", duplicates}, {"firstError", firstError},
        {"maximumGapSeconds", gap}, {"sampling", "all selected messages; no display decimation"},
        {"duplicateRule", "Equal timestamps are merged; conflicting values become invalid interpolation barriers."},
        {"frameConversion", positions ? "Full position TF before finite differences; moving-frame result is relative position derivative."
            : "Rotate the linear velocity vector at its original reference point; no reference-point translation or moving-frame velocity subtraction."},
        {"differenceDefinition", positions ? "Centred secant with valid adjacent samples; one-sided at valid segment endpoints." : "not estimated"}};
    check(configured && !result.samples.empty(), "No speed samples with the selected explicit time basis.");
    return result;
}

SpeedSeries speedSeriesFromRecording(const Recording &recording)
{
    SpeedSeries result; result.vector = false; result.scalarIsSpeed = true; result.maximumGapNs = recording.maximumGap;
    for (const auto &[stamp, value] : recording.speeds)
        result.samples.push_back({stamp, {}, double(value), std::isfinite(value) && value >= 0.f, {}});
    result.provenance = {{"topic", recording.speedTopic}, {"method", "recording-direct-magnitude"}, {"canonicalUnit", "m/s"},
        {"timeBasis", recording.timelineTimeBasis}, {"filtered", false}, {"estimated", false},
        {"precision", "Existing Recording speed cache uses float32; select the real Topic for full float64 fields."}};
    return result;
}
SpeedSeries filteredSpeedSeries(const SpeedSeries &source, double tau)
{
    check(std::isfinite(tau) && tau > 0., "Filter time constant must be positive.");
    auto result = source;
    for (size_t i = 1; i < result.samples.size(); ++i) {
        auto &sample = result.samples[i]; const auto &previous = result.samples[i - 1];
        const auto dt = sample.stampNs - previous.stampNs;
        if (!sample.valid || !previous.valid || sample.breakBefore || sample.frame != previous.frame || dt <= 0 || dt > source.maximumGapNs) continue;
        const double alpha = -std::expm1(-double(dt) / Second / tau);
        for (size_t axis = 0; axis < 3; ++axis) sample.velocity[axis] = previous.velocity[axis] + alpha * (sample.velocity[axis] - previous.velocity[axis]);
        sample.scalar = previous.scalar + alpha * (sample.scalar - previous.scalar);
    }
    result.provenance.insert("filtered", true); result.provenance.insert("filterTimeConstantSeconds", tau);
    result.provenance.insert("filterDefinition", "Causal exponential low-pass; alpha=1-exp(-dt/tau); reset at invalid samples, frame changes and long gaps.");
    return result;
}

SpeedIndex::SpeedIndex(const SpeedSeries &series, VelocityMode mode) : maximumGap_(series.maximumGapNs),
    distanceAvailable_(series.vector || series.scalarIsSpeed), mode_(mode)
{
    stamps_.reserve(series.samples.size()); values_.reserve(series.samples.size()); speeds_.reserve(series.samples.size());
    for (const auto &sample : series.samples) {
        check(sample.stampNs >= 0 && (stamps_.empty() || sample.stampNs > stamps_.back()), "Speed index requires unique, ascending nonnegative timestamps.");
        stamps_.push_back(sample.stampNs); values_.push_back(component(series, sample, mode));
        speeds_.push_back(component(series, sample, series.vector ? VelocityMode::Total : VelocityMode::Custom));
    }
    const auto n = stamps_.size(); duration_.resize(n); integral_.resize(n); distance_.resize(n);
    validEdges_.resize(n); blocks_.resize((n + BlockSize - 1) / BlockSize);
    for (size_t i = 0; i < n; ++i) {
        if (std::isfinite(values_[i])) {
            auto &block = blocks_[i / BlockSize]; block.low = std::min(block.low, values_[i]); block.high = std::max(block.high, values_[i]); ++block.count;
        }
        if (!i) continue;
        duration_[i] = duration_[i - 1]; integral_[i] = integral_[i - 1]; distance_[i] = distance_[i - 1];
        const auto dt = stamps_[i] - stamps_[i - 1];
        validEdges_[i] = dt <= maximumGap_ && !series.samples[i].breakBefore && std::isfinite(values_[i]) && std::isfinite(values_[i - 1])
            && series.samples[i].frame == series.samples[i - 1].frame;
        if (!validEdges_[i]) continue;
        const double seconds = double(dt) / Second;
        duration_[i] += seconds; integral_[i] += .5 * (values_[i] + values_[i - 1]) * seconds;
        if (distanceAvailable_) distance_[i] += .5 * (speeds_[i] + speeds_[i - 1]) * seconds;
    }
}
std::optional<double> SpeedIndex::valueAt(qint64 stamp) const
{
    const auto next = std::lower_bound(stamps_.begin(), stamps_.end(), stamp);
    if (next == stamps_.end()) return {};
    const auto index = size_t(next - stamps_.begin());
    if (*next == stamp) return std::isfinite(values_[index]) ? std::optional(values_[index]) : std::nullopt;
    if (!index || !validEdges_[index]) return {};
    const double weight = double(stamp - stamps_[index - 1]) / double(stamps_[index] - stamps_[index - 1]);
    return std::lerp(values_[index - 1], values_[index], weight);
}
SpeedIndex::Primitive SpeedIndex::primitiveAt(qint64 stamp) const
{
    if (stamps_.empty() || stamp <= stamps_.front()) return {};
    const auto after = std::upper_bound(stamps_.begin(), stamps_.end(), stamp);
    const auto before = size_t(after - stamps_.begin() - 1);
    Primitive result{duration_[before], integral_[before], distance_[before]};
    if (before + 1 >= stamps_.size() || !validEdges_[before + 1]) return result;
    const double seconds = double(stamp - stamps_[before]) / Second;
    const double fraction = double(stamp - stamps_[before]) / double(stamps_[before + 1] - stamps_[before]);
    result.duration += seconds;
    result.integral += seconds * (values_[before] + .5 * fraction * (values_[before + 1] - values_[before]));
    if (distanceAvailable_) result.distance += seconds * (speeds_[before] + .5 * fraction * (speeds_[before + 1] - speeds_[before]));
    return result;
}
QVariantMap SpeedIndex::statistics(const std::vector<SpeedRange> &included, const std::vector<SpeedRange> &excluded) const
{
    const auto selected = included.empty() && !stamps_.empty() ? std::vector<SpeedRange>{{stamps_.front(), stamps_.back()}} : mergeSpeedRanges(included);
    const auto removed = mergeSpeedRanges(excluded); const auto ranges = subtractSpeedRanges(selected, removed);
    double duration = 0., integral = 0., distance = 0., requested = 0.;
    double low = std::numeric_limits<double>::infinity(), high = -low; qint64 samples = 0;
    for (const auto &range : ranges) {
        const auto first = primitiveAt(range.begin), last = primitiveAt(range.end);
        duration += last.duration - first.duration; integral += last.integral - first.integral; distance += last.distance - first.distance;
        requested += double(range.end - range.begin) / Second;
        auto begin = size_t(std::lower_bound(stamps_.begin(), stamps_.end(), range.begin) - stamps_.begin());
        const auto end = size_t(std::upper_bound(stamps_.begin(), stamps_.end(), range.end) - stamps_.begin());
        while (begin < end) {
            // Blocks touching an excluded boundary use exact per-sample checks.
            if (begin % BlockSize == 0 && begin + BlockSize <= end
                && !contains(removed, stamps_[begin]) && !contains(removed, stamps_[begin + BlockSize - 1])) {
                const auto &block = blocks_[begin / BlockSize]; low = std::min(low, block.low); high = std::max(high, block.high); samples += block.count;
                begin += BlockSize;
            } else {
                if (std::isfinite(values_[begin]) && !contains(removed, stamps_[begin])) { low = std::min(low, values_[begin]); high = std::max(high, values_[begin]); ++samples; }
                ++begin;
            }
        }
    }
    QVariantMap result{{"mode", velocityModeName(mode_)}, {"unit", "m/s"}, {"requestedRanges", speedRangesToVariants(selected)},
        {"excludedRanges", speedRangesToVariants(removed)}, {"effectiveRanges", speedRangesToVariants(ranges)},
        {"validDurationSeconds", duration}, {"requestedDurationSeconds", requested}, {"validSampleCount", samples},
        {"coverage", requested > 0. ? std::clamp(duration / requested, 0., 1.) : 0.},
        {"maximumGapSeconds", double(maximumGap_) / Second}, {"complete", requested > 0. && std::abs(duration - requested) < std::max(1.e-8, requested * 1.e-10)},
        {"integration", "Trapezoidal integration of scalar speed/component at real timestamps; linearly interpolated boundaries; invalid samples, frame changes and long gaps excluded; no extrapolation."},
        {"extremaDefinition", "Valid original samples inside inclusive selected intervals; excluded boundary samples removed; no interpolated extrema."},
        {"distanceDefinition", distanceAvailable_ ? "Trapezoidal integral of three-dimensional speed magnitude (or explicitly declared scalar speed); metres." : "Unavailable: custom scalar has not been declared a speed magnitude."}};
    if (samples) { result.insert("maximumSI", high); result.insert("minimumSI", low); }
    if (duration > 0.) {
        result.insert("averageSI", integral / duration);
        if (distanceAvailable_) result.insert("distanceMetres", std::max(0., distance));
    }
    return result;
}
QVariantList SpeedIndex::plot(const std::vector<SpeedRange> &included, int maximumPoints) const
{
    maximumPoints = std::clamp(maximumPoints, 2, 200000);
    const auto ranges = included.empty() && !stamps_.empty() ? std::vector<SpeedRange>{{stamps_.front(), stamps_.back()}} : mergeSpeedRanges(included);
    QVariantList result;
    if (stamps_.empty()) return result;
    // Peak-preserving min/max buckets for display only, including invalid breaks.
    const auto buckets = size_t(std::max(1, maximumPoints / 3));
    const size_t bucket = std::max(size_t(1), (stamps_.size() + buckets - 1) / buckets);
    size_t previous = 0; bool havePrevious = false;
    const auto append = [&](size_t i) {
        if (!contains(ranges, stamps_[i])) return;
        bool gap = i > 0 && !validEdges_[i];
        if (havePrevious)
            for (size_t edge = previous + 1; edge <= i; ++edge) gap |= !validEdges_[edge];
        result.append(QVariantMap{{"stampNs", QString::number(stamps_[i])},
            {"valueSI", std::isfinite(values_[i]) ? QVariant(values_[i]) : QVariant()}, {"valid", std::isfinite(values_[i])},
            {"breakBefore", gap}});
        previous = i; havePrevious = true;
    };
    for (size_t start = 0; start < stamps_.size(); start += bucket) {
        const auto end = std::min(start + bucket, stamps_.size()); size_t lo = start, hi = start, gap = end;
        for (size_t i = start; i < end; ++i) {
            if (gap == end && (!std::isfinite(values_[i]) || (i > 0 && !validEdges_[i]))) gap = i;
            if (!std::isfinite(values_[i])) continue;
            if (!std::isfinite(values_[lo]) || values_[i] < values_[lo]) lo = i;
            if (!std::isfinite(values_[hi]) || values_[i] > values_[hi]) hi = i;
        }
        std::vector<size_t> selected{lo, hi}; if (gap != end) selected.push_back(gap);
        std::sort(selected.begin(), selected.end()); selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        for (const auto i : selected) if (result.size() < maximumPoints) append(i);
    }
    return result;
}
}
