#include "trajectory_edits.h"

#include <QJsonArray>
#include <QColor>
#include <QSet>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
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
QJsonObject endpointJson(const TrajectoryEndpointStyle &style)
{
    return {{"enabled", style.enabled}, {"show_text", style.showText}, {"show_time", style.showTime},
        {"fill", style.fill}, {"stroke", style.stroke}, {"text_color", style.textColor}, {"text", style.text},
        {"size", style.size}, {"stroke_width", style.strokeWidth}, {"opacity", style.opacity}, {"font_size", style.fontSize}};
}
void readEndpoint(const QJsonObject &json, TrajectoryEndpointStyle &style)
{
    if (json.isEmpty()) return;
    style.enabled = json.value("enabled").toBool(style.enabled);
    style.showText = json.value("show_text").toBool(style.showText);
    style.showTime = json.value("show_time").toBool(style.showTime);
    style.fill = json.value("fill").toString(style.fill); style.stroke = json.value("stroke").toString(style.stroke);
    style.textColor = json.value("text_color").toString(style.textColor); style.text = json.value("text").toString(style.text);
    style.size = json.value("size").toDouble(style.size); style.strokeWidth = json.value("stroke_width").toDouble(style.strokeWidth);
    style.opacity = json.value("opacity").toDouble(style.opacity); style.fontSize = json.value("font_size").toDouble(style.fontSize);
    if (!QColor(style.fill).isValid() || !QColor(style.stroke).isValid() || !QColor(style.textColor).isValid() ||
        !std::isfinite(style.size) || style.size < .1 || style.size > 2048. ||
        !std::isfinite(style.strokeWidth) || style.strokeWidth < 0. || style.strokeWidth > 512. ||
        !std::isfinite(style.opacity) || style.opacity < 0. || style.opacity > 1. ||
        !std::isfinite(style.fontSize) || style.fontSize < 1. || style.fontSize > 512. || style.text.size() > 4096)
        throw std::invalid_argument("Invalid trajectory endpoint style.");
}
}

qint64 trajectoryTimestamp(double seconds)
{
    const long double stamp = static_cast<long double>(seconds) * 1000000000.L;
    if (!std::isfinite(seconds) || seconds < 0. || stamp > std::numeric_limits<qint64>::max() - .5L)
        throw std::invalid_argument("Invalid trajectory source time.");
    return qint64(std::round(stamp));
}

QString trajectoryMaskContentKey(const TrajectoryFrameEdit &edit)
{
    if (edit.maskStrokes.empty()) return QStringLiteral("base");
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint64(edit.maskStrokes.size());
    for (const auto &stroke : edit.maskStrokes) {
        stream << stroke.radius << quint8(stroke.erase) << quint64(stroke.points.size());
        for (const auto &point : stroke.points) stream << point.x() << point.y();
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject TrajectoryEdits::toJson() const
{
    QJsonArray savedRanges, savedFrames;
    for (const auto &range : ranges) {
        validate(range);
        savedRanges.append(QJsonObject{{"id", range.id}, {"begin_ns", QString::number(range.begin)},
            {"end_ns", QString::number(range.end)}, {"parts", range.parts}});
    }
    for (auto i = frames.cbegin(); i != frames.cend(); ++i) {
        QJsonArray strokes;
        for (const auto &stroke : i->maskStrokes) {
            QJsonArray points;
            for (const auto &point : stroke.points) points.append(QJsonArray{point.x(), point.y()});
            strokes.append(QJsonObject{{"points", points}, {"radius", stroke.radius}, {"erase", stroke.erase}});
        }
        savedFrames.append(QJsonObject{{"stamp_ns", QString::number(i.key())}, {"excluded", i->excluded},
            {"manual", i->manual}, {"opacity", i->opacity}, {"mask_strokes", strokes}, {"removed", i->removed}});
    }
    QJsonObject json{{"version", 1}, {"ranges", savedRanges}, {"frames", savedFrames}, {"show_line", showLine},
        {"line_only", lineOnly}, {"skip_invalid", skipInvalid},
        {"line_style", QJsonObject{{"color", line.color}, {"width", line.width}, {"opacity", line.opacity}, {"pattern", line.pattern}}},
        {"start_style", endpointJson(start)}, {"goal_style", endpointJson(goal)}};
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
        result.frames[stamp].removed = object.value("removed").toBool();
        const auto strokes = object.value("mask_strokes").toArray();
        if (strokes.size() > 10000) throw std::invalid_argument("Too many trajectory mask strokes.");
        for (const auto &value : strokes) {
            const auto saved = value.toObject();
            TrajectoryMaskStroke stroke;
            stroke.radius = saved.value("radius").toDouble(-1.);
            stroke.erase = saved.value("erase").toBool();
            const auto points = saved.value("points").toArray();
            if (!std::isfinite(stroke.radius) || stroke.radius <= 0. || stroke.radius > .5 || points.isEmpty() || points.size() > 100000)
                throw std::invalid_argument("Invalid trajectory mask stroke.");
            for (const auto &value : points) {
                const auto point = value.toArray();
                if (point.size() != 2 || !point[0].isDouble() || !point[1].isDouble() ||
                    !std::isfinite(point[0].toDouble()) || !std::isfinite(point[1].toDouble()) ||
                    point[0].toDouble() < 0. || point[0].toDouble() > 1. || point[1].toDouble() < 0. || point[1].toDouble() > 1.)
                    throw std::invalid_argument("Invalid trajectory mask point.");
                stroke.points.append(QPointF(point[0].toDouble(), point[1].toDouble()));
            }
            result.frames[stamp].maskStrokes.push_back(std::move(stroke));
        }
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
    result.lineOnly = json.value("line_only").toBool();
    result.skipInvalid = json.value("skip_invalid").toBool();
    const auto line = json.value("line_style").toObject();
    result.line.color = line.value("color").toString(result.line.color);
    result.line.width = line.value("width").toDouble(result.line.width);
    result.line.opacity = line.value("opacity").toDouble(result.line.opacity);
    result.line.pattern = line.value("pattern").toInt(result.line.pattern);
    if (!QColor(result.line.color).isValid() || !std::isfinite(result.line.width) || result.line.width <= 0. || result.line.width > 512. ||
        !std::isfinite(result.line.opacity) || result.line.opacity < 0. || result.line.opacity > 1. || result.line.pattern < 0 || result.line.pattern > 3)
        throw std::invalid_argument("Invalid trajectory line style.");
    readEndpoint(json.value("start_style").toObject(), result.start);
    readEndpoint(json.value("goal_style").toObject(), result.goal);
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
