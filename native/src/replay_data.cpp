#include "replay_data.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QtEndian>
#include <bzlib.h>
#include <lz4frame.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace replay {
namespace {
constexpr auto Second = 1'000'000'000LL;
constexpr qsizetype MaxChunkBytes = 256 * 1024 * 1024;
constexpr size_t MaxPoints = 16'000'000;

[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }

class Bytes {
public:
    explicit Bytes(QByteArrayView bytes) : bytes_(bytes) {}
    bool empty() const { return at_ == bytes_.size(); }
    QByteArrayView take(qsizetype n)
    {
        if (n < 0 || n > bytes_.size() - at_)
            fail("bag 消息不完整或长度无效。");
        const auto value = bytes_.sliced(at_, n);
        at_ += n;
        return value;
    }
    quint8 byte() { return quint8(take(1)[0]); }
    quint32 u32() { return qFromLittleEndian<quint32>(take(4).data()); }
    float float32() { return std::bit_cast<float>(u32()); }
    double real() { return std::bit_cast<double>(qFromLittleEndian<quint64>(take(8).data())); }
    QByteArrayView blob() { return take(u32()); }
    QString string() { return QString::fromUtf8(blob()); }
    qint64 time()
    {
        const auto sec = u32(), ns = u32();
        if (ns >= Second)
            fail("bag 时间戳无效。");
        return qint64(sec) * Second + ns;
    }
private:
    QByteArrayView bytes_;
    qsizetype at_ = 0;
};

using Header = QHash<QByteArray, QByteArray>;
Header fields(QByteArrayView bytes)
{
    Header values;
    Bytes reader(bytes);
    while (!reader.empty()) {
        const auto field = reader.blob();
        const auto split = field.indexOf('=');
        if (split < 1)
            fail("bag 记录头格式无效。");
        values.insert(field.first(split).toByteArray(), field.sliced(split + 1).toByteArray());
    }
    return values;
}

QByteArray readExactly(QFile &file, qsizetype size)
{
    if (size < 0 || size > file.size() - file.pos())
        fail("bag 文件不完整。");
    auto bytes = file.read(size);
    if (bytes.size() != size)
        fail("读取 bag 失败。");
    return bytes;
}

Header readHeader(QFile &file)
{
    auto length = readExactly(file, 4);
    auto size = Bytes(length).u32();
    if (size > 1024 * 1024)
        fail("bag 记录头过大。");
    return fields(readExactly(file, size));
}

quint32 number(const Header &header, const char *key)
{
    return Bytes(header.value(key)).u32();
}

quint8 opcode(const Header &header) { return Bytes(header.value("op")).byte(); }

struct HeaderMessage {
    qint64 stamp;
    QString frame;
};
HeaderMessage messageHeader(Bytes &reader)
{
    reader.u32(); // ROS1 sequence number.
    const auto time = reader.time();
    auto frame = reader.string();
    while (frame.startsWith('/'))
        frame.remove(0, 1);
    return {time, std::move(frame)};
}

struct Connection {
    QString topic;
    QByteArray type;
};
struct Chunk {
    qint64 offset;
    qsizetype size;
    qsizetype unpacked;
    QByteArray compression;
};

void checkCancel(const Cancel &cancel)
{
    if (cancel && cancel())
        fail("已取消读取。");
}

QByteArray unpack(QByteArray input, const Chunk &chunk)
{
    if (chunk.compression == "none") {
        if (input.size() != chunk.unpacked)
            fail("bag 数据块长度与记录头不一致。");
        return input;
    }
    if (chunk.unpacked < 1 || chunk.unpacked > MaxChunkBytes)
        fail("bag 解压块超出预览版的 256 MB 限制。");
    QByteArray output(chunk.unpacked, Qt::Uninitialized);
    if (chunk.compression == "bz2") {
        auto count = static_cast<unsigned int>(output.size());
        if (BZ2_bzBuffToBuffDecompress(output.data(), &count, input.data(),
                                      static_cast<unsigned int>(input.size()), 0, 0) != BZ_OK
            || count != output.size())
            fail("无法解压 BZ2 bag 数据块。");
    } else if (chunk.compression == "lz4") {
        LZ4F_dctx *raw = nullptr;
        if (LZ4F_isError(LZ4F_createDecompressionContext(&raw, LZ4F_VERSION)))
            fail("无法初始化 LZ4 解压。");
        std::unique_ptr<LZ4F_dctx, decltype(&LZ4F_freeDecompressionContext)> context(raw, LZ4F_freeDecompressionContext);
        size_t src = 0, dest = 0, pending;
        do {
            auto available = size_t(input.size()) - src;
            auto capacity = size_t(output.size()) - dest;
            pending = LZ4F_decompress(context.get(), output.data() + dest, &capacity,
                                      input.constData() + src, &available, nullptr);
            if (LZ4F_isError(pending) || (!available && !capacity && pending))
                fail("无法解压 LZ4 bag 数据块。");
            src += available;
            dest += capacity;
        } while (pending);
        if (dest != size_t(output.size()) || src != size_t(input.size()))
            fail("LZ4 解压数据长度不一致。");
    } else {
        fail("支持的 ROS1 bag 压缩方式为 none、bz2 和 lz4。");
    }
    return output;
}

bool plannerMap(const QString &topic)
{
    return topic.endsWith("/grid_map/occupancy") || topic.endsWith("/grid_map/occupancy_inflate")
        || topic.endsWith("/rog_map/occ") || topic.endsWith("/rog_map/inf_occ");
}

float pointComponent(QByteArrayView point, quint32 offset, quint8 type, bool big)
{
    const char *address = point.data() + offset;
    if (type == 7) {
        const auto bits = big ? qFromBigEndian<quint32>(address) : qFromLittleEndian<quint32>(address);
        return std::bit_cast<float>(bits);
    }
    const auto bits = big ? qFromBigEndian<quint64>(address) : qFromLittleEndian<quint64>(address);
    return float(std::bit_cast<double>(bits));
}

void readCloudPoints(std::vector<Point> &points, Bytes &reader, const Cancel &cancel)
{
    const auto height = reader.u32(), width = reader.u32(), fieldCount = reader.u32();
    if (fieldCount > 512)
        fail("PointCloud2 字段数无效。");
    struct Field { quint32 offset = 0; quint8 type = 0; };
    Field axes[3];
    for (quint32 i = 0; i < fieldCount; ++i) {
        const auto name = reader.blob();
        const auto offset = reader.u32();
        const auto type = reader.byte();
        const auto count = reader.u32();
        if (name.size() == 1 && name[0] >= 'x' && name[0] <= 'z' && count == 1)
            axes[name[0] - 'x'] = {offset, type};
    }
    const auto big = reader.byte() != 0;
    const auto pointStep = reader.u32(), rowStep = reader.u32();
    const auto cloud = reader.blob();
    if (!pointStep || quint64(width) * pointStep > rowStep || quint64(height) * rowStep > quint64(cloud.size()))
        fail("PointCloud2 行步长或字节长度无效。");
    for (const auto &axis : axes) {
        if ((axis.type != 7 && axis.type != 8) || quint64(axis.offset) + (axis.type == 7 ? 4 : 8) > pointStep)
            fail("PointCloud2 需要有效的 x、y、z 浮点字段。");
    }

    for (quint32 y = 0; width && y < height; ++y) {
        if ((y & 63) == 0)
            checkCancel(cancel);
        for (quint32 x = 0; x < width; ++x) {
            const auto point = cloud.sliced(qsizetype(y) * rowStep + qsizetype(x) * pointStep, pointStep);
            const Point value{pointComponent(point, axes[0].offset, axes[0].type, big),
                              pointComponent(point, axes[1].offset, axes[1].type, big),
                              pointComponent(point, axes[2].offset, axes[2].type, big)};
            if (std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)) {
                if (points.size() >= MaxPoints)
                    fail("点数超过原生预览版的 1600 万点上限，请使用较短的 bag。");
                points.push_back(value);
            }
        }
    }
}

void readCloud(Recording &record, QByteArrayView data, qint64 recorded, const Cancel &cancel)
{
    Bytes reader(data);
    const auto header = messageHeader(reader);
    const auto stamp = header.stamp == 0 ? recorded : header.stamp;
    if (!record.frames.empty() && stamp < record.frames.back().stamp)
        fail("点云时间戳倒退，请检查 bag 内的话题时间。");
    if (record.frames.empty())
        record.frameId = header.frame;
    if (record.frameId.isEmpty() || header.frame != record.frameId)
        fail("点云话题必须使用同一坐标系。");

    const auto begin = int(record.points.size());
    readCloudPoints(record.points, reader, cancel);
    record.frames.push_back({stamp, begin, int(record.points.size())});
}

bool supportsTopic(const QString &kind, const QByteArray &type)
{
    if (kind == "cloud")
        return type == "sensor_msgs/PointCloud2";
    if (kind == "laser")
        return type == "sensor_msgs/LaserScan";
    if (kind == "path")
        return type == "nav_msgs/Path";
    if (kind == "position")
        return type == "geometry_msgs/PointStamped" || type == "geometry_msgs/PoseStamped" ||
               type == "geometry_msgs/PoseWithCovarianceStamped" || type == "nav_msgs/Odometry";
    return false;
}

void appendPoint(std::vector<Point> &points, const Point &point)
{
    if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)) {
        if (points.size() >= MaxPoints)
            fail("附加图层的点数超过 1600 万。");
        points.push_back(point);
    }
}

Point readPosition(Bytes &message)
{
    return {float(message.real()), float(message.real()), float(message.real())};
}

void readOverlay(TopicOverlay &overlay, const QByteArray &type, QByteArrayView data, qint64 recorded,
                 const Cancel &cancel)
{
    Bytes message(data);
    const auto header = messageHeader(message);
    const int start = int(overlay.points.size());
    if (type == "sensor_msgs/PointCloud2") {
        readCloudPoints(overlay.points, message, cancel);
    } else if (type == "sensor_msgs/LaserScan") {
        const double minimumAngle = message.float32();
        message.take(4); // Maximum angle is derived from the range index.
        const double increment = message.float32();
        message.take(8); // Time increment and scan time.
        const float minimumRange = message.float32(), maximumRange = message.float32();
        const auto count = message.u32();
        Bytes ranges(message.take(qsizetype(count) * 4));
        for (quint32 i = 0; i < count; ++i) {
            if ((i & 1023) == 0)
                checkCancel(cancel);
            const auto range = ranges.float32();
            if (range >= minimumRange && range <= maximumRange) {
                const auto angle = minimumAngle + i * increment;
                appendPoint(overlay.points, {float(range * std::cos(angle)), float(range * std::sin(angle)), 0.f});
            }
        }
    } else if (type == "nav_msgs/Path") {
        const auto count = message.u32();
        for (quint32 i = 0; i < count; ++i) {
            if ((i & 1023) == 0)
                checkCancel(cancel);
            const auto pose = messageHeader(message);
            const auto point = readPosition(message);
            message.take(4 * 8);
            if (pose.frame.isEmpty() || pose.frame == header.frame)
                appendPoint(overlay.points, point);
        }
    } else {
        if (type == "nav_msgs/Odometry")
            message.blob(); // Child frame.
        appendPoint(overlay.points, readPosition(message));
    }

    overlay.samples.push_back({{header.stamp == 0 ? recorded : header.stamp,
                                start, int(overlay.points.size())}, header.frame});
}

struct Odometry {
    Pose pose;
    QString parent;
    QString child;
};

Odometry readOdometry(QByteArrayView data)
{
    Bytes reader(data);
    const auto header = messageHeader(reader);
    auto child = reader.string();
    while (child.startsWith('/'))
        child.remove(0, 1);
    const float x = float(reader.real());
    const float y = float(reader.real()), z = float(reader.real());
    const float qx = float(reader.real()), qy = float(reader.real()), qz = float(reader.real()), qw = float(reader.real());
    reader.take(36 * 8); // pose covariance
    const double vx = reader.real(), vy = reader.real(), vz = reader.real();
    const auto speed = std::hypot(vx, vy, vz);
    return {{header.stamp, {x, y, z}, QQuaternion(qw, qx, qy, qz).normalized(),
             std::isfinite(speed) && speed <= std::numeric_limits<float>::max()
                 ? std::optional<float>(float(speed)) : std::nullopt},
            header.frame, std::move(child)};
}

void bounds(Recording &record, const Cancel &cancel = {})
{
    if (record.points.empty())
        fail("点云话题没有有效点。");
    Point lo = record.points.front(), hi = lo;
    for (size_t i = 0; i < record.points.size(); ++i) {
        if ((i & 65535) == 0)
            checkCancel(cancel);
        const auto &p = record.points[i];
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    record.center = {lo.x + (hi.x - lo.x) / 2, lo.y + (hi.y - lo.y) / 2, lo.z + (hi.z - lo.z) / 2};
    record.radius = std::max(1.f, QVector3D(hi.x-lo.x, hi.y-lo.y, hi.z-lo.z).length() * .5f);
}
} // namespace

const TopicOverlay::Sample *TopicOverlay::sampleAt(qint64 stamp) const
{
    const auto next = std::upper_bound(samples.begin(), samples.end(), stamp,
        [](qint64 time, const Sample &sample) { return time < sample.range.stamp; });
    return next == samples.begin() ? nullptr : &*std::prev(next);
}

qint64 Recording::stampAt(double seconds) const
{
    return frames.front().stamp + qint64(std::round(std::clamp(seconds, 0., duration()) * Second));
}

double Recording::duration() const
{
    const auto end = poses.empty() ? frames.back().stamp : std::max(frames.back().stamp, poses.back().stamp);
    return double(end - frames.front().stamp) / Second;
}

const Frame &Recording::frameAt(qint64 stamp) const
{
    const auto next = std::upper_bound(frames.begin(), frames.end(), stamp,
                                      [](qint64 t, const Frame &frame) { return t < frame.stamp; });
    return next == frames.begin() ? frames.front() : *std::prev(next);
}

std::optional<Pose> Recording::poseAt(qint64 stamp) const
{
    const auto next = std::lower_bound(poses.begin(), poses.end(), stamp,
                                      [](const Pose &pose, qint64 t) { return pose.stamp < t; });
    if (next != poses.end() && next->stamp == stamp)
        return *next;
    if (next == poses.begin() || next == poses.end())
        return std::nullopt;
    const auto &before = *std::prev(next);
    if (next->stamp - before.stamp > Second / 2)
        return std::nullopt;
    const auto weight = float(stamp - before.stamp) / float(next->stamp - before.stamp);
    std::optional<float> speed;
    if (before.speed && next->speed)
        speed = std::lerp(*before.speed, *next->speed, weight);
    return Pose{stamp, before.position * (1-weight) + next->position * weight,
                QQuaternion::slerp(before.rotation, next->rotation, weight), speed};
}

std::shared_ptr<const Recording> loadBag(const QString &path, const LoadOptions &options,
                                         const Cancel &cancel, const Progress &progress)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("无法读取所选 bag 文件。");
    if (file.readLine(14).trimmed() != "#ROSBAG V2.0")
        fail("原生预览版支持 ROS1 bag 2.0。");

    std::vector<Chunk> chunks;
    std::map<quint32, Connection> connections;
    auto record = std::make_shared<Recording>();
    record->name = QFileInfo(file).fileName();
    while (!file.atEnd()) {
        checkCancel(cancel);
        const auto header = readHeader(file);
        const auto sizeBytes = readExactly(file, 4);
        const auto size = qsizetype(Bytes(sizeBytes).u32());
        switch (opcode(header)) {
        case 0x05:
            if (size > MaxChunkBytes)
                fail("bag 数据块超过预览版的 256 MB 限制。");
            chunks.push_back({file.pos(), size, qsizetype(number(header, "size")), header.value("compression")});
            break;
        case 0x07: {
            if (size > 1024 * 1024)
                fail("bag 连接描述过大。");
            const auto id = number(header, "conn");
            const auto type = fields(readExactly(file, size)).value("type");
            connections.insert_or_assign(id, Connection{QString::fromUtf8(header.value("topic")), type});
            continue;
        }
        }
        if (size > file.size() - file.pos() || !file.seek(file.pos() + size))
            fail("bag 记录长度无效。");
    }

    for (const auto &[id, connection] : connections) {
        if (connection.type == "sensor_msgs/PointCloud2")
            record->cloudTopics.append(connection.topic);
        if (connection.type == "nav_msgs/Odometry")
            record->odometryTopics.append(connection.topic);
    }
    record->cloudTopics.removeDuplicates();
    record->odometryTopics.removeDuplicates();
    record->cloudTopics.sort();
    record->odometryTopics.sort();
    if (record->cloudTopics.isEmpty())
        fail("bag 内没有可读取的 PointCloud2 话题。");
    record->cloudTopic = options.cloudTopic;
    if (record->cloudTopic.isEmpty()) {
        const auto preferred = std::find_if(record->cloudTopics.begin(), record->cloudTopics.end(),
            [](const QString &name) { return name.endsWith("/cloud_registered"); });
        record->cloudTopic = preferred == record->cloudTopics.end() ? record->cloudTopics.front() : *preferred;
    }
    if (!record->cloudTopics.contains(record->cloudTopic))
        fail("所选点云话题不存在。");
    record->odometryTopic = options.odometryTopic;
    if (record->odometryTopic.isEmpty() && !record->odometryTopics.isEmpty()) {
        record->odometryTopic = record->odometryTopics.contains("/Odometry") ? "/Odometry" : record->odometryTopics.front();
    }
    if (!record->odometryTopic.isEmpty() && !record->odometryTopics.contains(record->odometryTopic))
        fail("所选里程计话题不存在。");
    record->replace = options.mapMode ? *options.mapMode == MapMode::Replace : plannerMap(record->cloudTopic);

    QHash<QString, std::vector<size_t>> overlayIndices;
    for (const auto &display : options.topicLayout.displays) {
        if (!display.enabled)
            continue;

        const auto match = std::find_if(connections.begin(), connections.end(), [&](const auto &entry) {
            return entry.second.topic == display.topic && supportsTopic(display.kind, entry.second.type);
        });
        if (match == connections.end()) {
            record->topicWarnings.append(display.name.isEmpty() ? display.topic : display.name);
            continue;
        }
        overlayIndices[display.topic].push_back(record->topicOverlays.size());
        record->topicOverlays.push_back({display});
    }
    QSet<QString> unreadableTopics;
    size_t overlayPoints = 0;
    std::vector<Odometry> odometry;
    for (size_t i = 0; i < chunks.size(); ++i) {
        checkCancel(cancel);
        const auto &chunk = chunks[i];
        if (!file.seek(chunk.offset))
            fail("无法定位 bag 数据块。");
        const auto decompressed = unpack(readExactly(file, chunk.size), chunk);
        Bytes reader(decompressed);
        while (!reader.empty()) {
            checkCancel(cancel);
            const auto header = fields(reader.blob());
            const auto data = reader.blob();
            if (opcode(header) != 0x02)
                continue;
            const auto entry = connections.find(number(header, "conn"));
            if (entry == connections.end())
                continue;
            const auto &connection = entry->second;
            if (connection.topic == record->cloudTopic && connection.type == "sensor_msgs/PointCloud2")
                readCloud(*record, data, Bytes(header.value("time")).time(), cancel);
            else if (connection.topic == record->odometryTopic && connection.type == "nav_msgs/Odometry")
                odometry.push_back(readOdometry(data));
            const auto layers = overlayIndices.constFind(connection.topic);
            if (layers != overlayIndices.cend()) {
                for (const auto index : *layers) {
                    auto &overlay = record->topicOverlays[index];
                    if (!supportsTopic(overlay.display.kind, connection.type))
                        continue;
                    const auto before = overlay.points.size();
                    try {
                        readOverlay(overlay, connection.type, data, Bytes(header.value("time")).time(), cancel);
                    } catch (const std::runtime_error &) {
                        checkCancel(cancel);
                        overlay.points.resize(before);
                        unreadableTopics.insert(overlay.display.topic);
                    }
                    overlayPoints += overlay.points.size() - before;
                    if (overlayPoints > MaxPoints)
                        fail("附加图层的点数超过 1600 万，请减少导入的图层。");
                }
            }
        }
        if (progress)
            progress(.88 * double(i + 1) / double(chunks.size()));
    }

    if (record->frames.empty())
        fail("未找到所选话题的点云帧。");
    bool local = !odometry.empty() && record->frameId == odometry.front().child;
    for (const auto &sample : odometry) {
        checkCancel(cancel);
        if (sample.pose.stamp > 0 && !sample.pose.rotation.isNull()
            && std::isfinite(sample.pose.position.lengthSquared())
            && (local ? sample.child == record->frameId && sample.parent == odometry.front().parent
                      : sample.parent == record->frameId))
            record->poses.push_back(sample.pose);
    }
    std::stable_sort(record->poses.begin(), record->poses.end(), [](const Pose &a, const Pose &b) {
        return a.stamp < b.stamp;
    });
    if (local) {
        for (const auto &scan : record->frames) {
            checkCancel(cancel);
            const auto pose = record->poseAt(scan.stamp);
            if (!pose && scan.first != scan.end)
                fail("机体点云的部分帧缺少同步里程计，请选择配准点云或录制对应位姿。");
            for (int i = scan.first; pose && i < scan.end; ++i) {
                const auto p = record->points[size_t(i)];
                const auto world = pose->rotation.rotatedVector({p.x, p.y, p.z}) + pose->position;
                record->points[size_t(i)] = {world.x(), world.y(), world.z()};
            }
        }
        record->frameId = odometry.front().parent;
    }

    bool importedCloud = false;
    for (auto &overlay : record->topicOverlays) {
        checkCancel(cancel);
        std::erase_if(overlay.samples, [&](const auto &sample) { return sample.frameId != record->frameId; });
        std::stable_sort(overlay.samples.begin(), overlay.samples.end(), [](const auto &a, const auto &b) {
            return a.range.stamp < b.range.stamp;
        });
        if (overlay.samples.empty())
            record->topicWarnings.append(overlay.display.topic);
        else if (overlay.display.kind == "cloud")
            importedCloud = true;
    }
    record->showMainCloud = options.showMainCloud || !importedCloud;
    record->topicWarnings.append(unreadableTopics.values());
    record->topicWarnings.removeDuplicates();
    record->topicWarnings.sort();

    bounds(*record, cancel);
    if (progress)
        progress(1);
    return record;
}

std::shared_ptr<const Recording> makeDemo()
{
    auto record = std::make_shared<Recording>();
    record->name = QStringLiteral("内置模拟回放");
    record->cloudTopic = "/cloud_registered";
    record->cloudTopics = {record->cloudTopic};
    record->odometryTopic = "/Odometry";
    record->odometryTopics = {record->odometryTopic};
    record->frameId = "world";
    constexpr int Frames = 150;
    for (int frame = 0; frame < Frames; ++frame) {
        const int start = int(record->points.size());
        const auto stamp = Second + frame * (Second / 15);
        const float front = -8 + frame * .38f;
        for (int row = 0; row < (frame == 0 ? 45 : 5); ++row) {
            const float x = front - row * .077f;
            for (int i = -65; i <= 65; ++i) {
                const float y = i * .09f;
                const float center = 4.f * std::sin(x * .105f);
                const float noise = std::sin((i * 27.f + row * 5.f + frame * 113.f) * .81f);
                record->points.push_back({x + .032f * noise, center + y, .10f * std::sin(y) + .032f * noise});
                if (std::abs(i) > 45) {
                    const float tree = std::pow(std::max(0.f, std::sin(x * 1.9f + (i < 0 ? .4f : 0.f))), 4.f);
                    record->points.push_back({x + .17f * noise, center + y + .12f * noise,
                                              tree * (.8f + .18f * std::abs(i - (i < 0 ? -45 : 45)))});
                }
            }
        }
        const float x = front - 5.4f;
        const float yaw = std::atan(.42f * std::cos(x * .105f)) * 57.2957795f;
        record->poses.push_back({stamp, {x, 4.f * std::sin(x *.105f), 1.7f},
                                 QQuaternion::fromAxisAndAngle(0, 0, 1, yaw),
                                 5.7f * std::hypot(1.f, .42f * std::cos(x * .105f))});
        record->frames.push_back({stamp, start, int(record->points.size())});
    }
    bounds(*record);
    return record;
}
} // namespace replay
