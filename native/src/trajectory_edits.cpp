#include "trajectory_edits.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace replay {
namespace {
qint64 readStamp(const QJsonValue &value)
{
    bool valid = false;
    const auto stamp = value.toString().toLongLong(&valid);
    if (!valid || stamp < 0)
        throw std::invalid_argument("Invalid trajectory source timestamp.");
    return stamp;
}
void validate(const TrajectoryRange &range)
{
    if (range.id.isEmpty() || range.begin < 0 || range.end <= range.begin ||
        range.parts < DroneFrames || range.parts > Both)
        throw std::invalid_argument("Invalid trajectory exclusion range.");
}
}

qint64 trajectoryTimestamp(double seconds)
{
    const long double stamp = static_cast<long double>(seconds) * 1000000000.L;
    if (!std::isfinite(seconds) || seconds < 0. || stamp > std::numeric_limits<qint64>::max() - .5L)
        throw std::invalid_argument("Invalid trajectory source time.");
    return qint64(std::round(stamp));
}

QJsonObject TrajectoryEdits::toJson() const
{
    QJsonArray savedRanges, savedFrames;
    for (const auto &range : ranges) {
        validate(range);
        savedRanges.append(QJsonObject{{"id", range.id}, {"begin_ns", QString::number(range.begin)},
            {"end_ns", QString::number(range.end)}, {"parts", range.parts}});
    }
    for (auto i = frames.cbegin(); i != frames.cend(); ++i)
        savedFrames.append(QJsonObject{{"stamp_ns", QString::number(i.key())}, {"excluded", i->excluded},
            {"manual", i->manual}, {"opacity", i->opacity}});
    QJsonObject json{{"version", 1}, {"ranges", savedRanges}, {"frames", savedFrames}, {"show_line", showLine}};
    if (trim)
        json.insert("trim_ns", QJsonArray{QString::number(trim->first), QString::number(trim->second)});
    return json;
}

TrajectoryEdits TrajectoryEdits::fromJson(const QJsonObject &json)
{
    if (json.isEmpty())
        return {};
    if (json.value("version").toInt() != 1 || !json.value("ranges").isArray() || !json.value("frames").isArray())
        throw std::invalid_argument("Unsupported or invalid trajectory edit state.");
    TrajectoryEdits result;
    QSet<QString> ids;
    for (const auto &entry : json.value("ranges").toArray()) {
        const auto object = entry.toObject();
        TrajectoryRange range{object.value("id").toString(), readStamp(object.value("begin_ns")),
            readStamp(object.value("end_ns")), object.value("parts").toInt()};
        validate(range);
        if (ids.contains(range.id))
            throw std::invalid_argument("Duplicate trajectory exclusion identity.");
        ids.insert(range.id);
        result.ranges.push_back(std::move(range));
    }
    for (const auto &entry : json.value("frames").toArray()) {
        const auto object = entry.toObject();
        const auto stamp = readStamp(object.value("stamp_ns"));
        const double opacity = object.value("opacity").toDouble(-1.);
        if (!std::isfinite(opacity) || opacity < 0. || opacity > 1. || result.frames.contains(stamp) ||
            !object.value("excluded").isBool() || !object.value("manual").isBool())
            throw std::invalid_argument("Invalid trajectory frame edit.");
        result.frames.insert(stamp, {object.value("excluded").toBool(), object.value("manual").toBool(), opacity});
    }
    if (json.contains("trim_ns")) {
        const auto trim = json.value("trim_ns").toArray();
        if (trim.size() != 2)
            throw std::invalid_argument("Invalid trajectory generation range.");
        result.trim = {readStamp(trim[0]), readStamp(trim[1])};
        if (result.trim->second <= result.trim->first)
            throw std::invalid_argument("Invalid trajectory generation range.");
    }
    result.showLine = json.value("show_line").toBool();
    return result;
}

TrajectoryExclusions::TrajectoryExclusions(const TrajectoryEdits &edits)
{
    for (const auto &range : edits.ranges) {
        validate(range);
        if (range.parts & DroneFrames)
            drone_.emplace_back(range.begin, range.end);
        if (range.parts & TrajectoryLine)
            line_.emplace_back(range.begin, range.end);
    }
    for (auto *list : {&drone_, &line_}) {
        std::sort(list->begin(), list->end());
        size_t count = 0;
        for (const auto &range : *list) {
            if (count && range.first <= (*list)[count - 1].second)
                (*list)[count - 1].second = std::max((*list)[count - 1].second, range.second);
            else
                (*list)[count++] = range;
        }
        list->resize(count);
    }
}

const std::vector<TrajectoryExclusions::Range> &TrajectoryExclusions::ranges(int part) const
{
    if (part != DroneFrames && part != TrajectoryLine)
        throw std::invalid_argument("Choose one trajectory exclusion part.");
    return part == DroneFrames ? drone_ : line_;
}

bool TrajectoryExclusions::contains(qint64 stamp, int part) const
{
    const auto &list = ranges(part);
    const auto after = std::upper_bound(list.begin(), list.end(), stamp,
        [](qint64 stamp, const Range &range) { return stamp < range.first; });
    return after != list.begin() && stamp < std::prev(after)->second;
}

bool TrajectoryExclusions::intersects(qint64 begin, qint64 end, int part) const
{
    const auto &list = ranges(part);
    const auto first = std::lower_bound(list.begin(), list.end(), begin,
        [](const Range &range, qint64 stamp) { return range.second <= stamp; });
    return begin < end && first != list.end() && first->first < end;
}

qint64 TrajectoryExclusions::duration(int part) const
{
    qint64 total = 0;
    for (const auto &range : ranges(part))
        total += range.second - range.first;
    return total;
}
}
