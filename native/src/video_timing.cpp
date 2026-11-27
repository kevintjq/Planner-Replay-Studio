#include "video_timing.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace replay {
namespace {
[[noreturn]] void invalid()
{
    throw std::runtime_error("Invalid video timing, crop, or synchronization settings");
}

double number(const QJsonValue &value)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        invalid();
    return value.toDouble();
}

std::optional<double> endTime(const QJsonValue &value)
{
    return value.isUndefined() || value.isNull() ? std::nullopt : std::optional<double>(number(value));
}
}

VideoTiming VideoTiming::fromJson(const QJsonObject &edit)
{
    VideoTiming result;
    if (edit.contains("offset"))
        result.offset = number(edit.value("offset"));
    if (edit.contains("trim_in"))
        result.trimIn = number(edit.value("trim_in"));
    result.trimOut = endTime(edit.value("trim_out"));
    if (result.trimIn < 0 || (result.trimOut && *result.trimOut <= result.trimIn))
        invalid();

    if (edit.contains("crop")) {
        const auto crop = edit.value("crop").toArray();
        if (crop.size() != 4)
            invalid();
        for (int i = 0; i < 4; ++i)
            result.crop[size_t(i)] = number(crop[i]);
        const auto &[x0, y0, x1, y1] = result.crop;
        if (x0 < 0 || y0 < 0 || x1 > 1 || y1 > 1 || x1 <= x0 || y1 <= y0)
            invalid();
    }

    if (edit.contains("anchors")) {
        const auto value = edit.value("anchors");
        const auto anchors = value.toArray();
        if (!value.isArray() || anchors.size() > 2)
            invalid();
        for (const auto &entry : anchors) {
            const auto pair = entry.toArray();
            if (pair.size() != 2)
                invalid();
            const auto replay = number(pair[0]), media = number(pair[1]);
            if (replay < 0 || media < 0)
                invalid();
            result.anchors.emplace_back(replay, media);
        }
        if (anchors.size() == 2) {
            auto ordered = result.anchors;
            std::sort(ordered.begin(), ordered.end());
            if (ordered[0].first == ordered[1].first || ordered[0].second >= ordered[1].second ||
                !std::isfinite(result.rate()) || result.rate() <= 0.)
                invalid();
        }
    }

    const auto segments = edit.value("segments");
    if (!segments.isUndefined() && !segments.isNull()) {
        if (!segments.isArray())
            invalid();
        result.segments.emplace();
        std::optional<double> previousEnd = 0.;
        for (const auto &entry : segments.toArray()) {
            const auto pair = entry.toArray();
            if (pair.size() != 2)
                invalid();
            const auto start = number(pair[0]);
            const auto end = endTime(pair[1]);
            if (!previousEnd || start < *previousEnd || (end && *end <= start))
                invalid();
            result.segments->emplace_back(start, end);
            previousEnd = end;
        }
    }
    return result;
}

double VideoTiming::rate() const
{
    if (anchors.size() < 2)
        return 1.;
    return (anchors[1].second - anchors[0].second) / (anchors[1].first - anchors[0].first);
}

double VideoTiming::sourceSeconds(double replaySeconds) const
{
    return anchors.empty() ? replaySeconds - offset
                           : anchors.front().second + (replaySeconds - anchors.front().first) * rate();
}

double VideoTiming::replaySeconds(double seconds) const
{
    return anchors.empty() ? seconds + offset
                           : anchors.front().first + (seconds - anchors.front().second) / rate();
}

std::vector<VideoTiming::Range> VideoTiming::spans() const
{
    if (!segments)
        return {{trimIn, trimOut}};

    std::vector<Range> result;
    for (const auto &[first, last] : *segments) {
        const double start = std::max(first, trimIn);
        auto end = last;
        if (trimOut && (!end || *trimOut < *end))
            end = trimOut;
        if (!end || start < *end)
            result.emplace_back(start, end);
    }
    return result;
}

QJsonArray VideoTiming::encodeSpans(const std::vector<Range> &ranges)
{
    QJsonArray output;
    for (const auto &[first, last] : ranges)
        output.append(QJsonArray{first, last ? QJsonValue(*last) : QJsonValue(QJsonValue::Null)});
    return output;
}

bool VideoTiming::includes(double seconds) const
{
    constexpr double epsilon = 1e-9;
    if (!std::isfinite(seconds) || seconds < trimIn - epsilon || (trimOut && seconds >= *trimOut - epsilon))
        return false;
    if (!segments)
        return true;
    return std::any_of(segments->begin(), segments->end(), [this, seconds](const auto &range) {
        const double start = std::max(range.first, trimIn);
        auto end = range.second;
        if (trimOut && (!end || *trimOut < *end))
            end = trimOut;
        return seconds >= start - epsilon && (!end || (start < *end && seconds < *end - epsilon));
    });
}
}
