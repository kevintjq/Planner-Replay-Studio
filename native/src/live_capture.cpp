#include "live_capture.h"
#include "bridge_wire.h"

#include <QHash>
#include <QHostAddress>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <limits>

namespace replay {
namespace {
constexpr quint64 Second = 1'000'000'000;
constexpr qint64 ChunkTarget = 8 * 1024 * 1024;
constexpr qint64 RecordingLimit = 4LL * 1024 * 1024 * 1024;

QByteArray receive(QTcpSocket &stream, int length, bool allowEof = false)
{
    QByteArray result;
    result.reserve(length);
    while (result.size() < length) {
        if (stream.bytesAvailable() == 0 && !stream.waitForReadyRead(10'000)) {
            if (allowEof && result.isEmpty() && stream.state() == QAbstractSocket::UnconnectedState)
                return {};
            throw std::runtime_error("ROS bridge stopped sending or disconnected during a message.");
        }
        const auto data = stream.read(length - result.size());
        if (data.isEmpty())
            throw std::runtime_error("ROS bridge disconnected during a message.");
        result.append(data);
    }
    return result;
}

QByteArray stamp(quint64 time)
{
    if (time / Second > std::numeric_limits<quint32>::max())
        throw std::runtime_error("ROS bridge timestamp is outside the ROS1 range.");
    return bridge::u32(quint32(time / Second)) + bridge::u32(quint32(time % Second));
}

QByteArray entry(const QByteArray &key, const QByteArray &value)
{
    return bridge::field(key + '=' + value);
}

QByteArray record(char opcode, const QByteArray &header, const QByteArray &payload)
{
    return bridge::field(entry("op", QByteArray(1, opcode)) + header) + bridge::field(payload);
}

quint64 headerTime(const bridge::Message &message)
{
    if (message.payload.size() < 12)
        throw std::runtime_error("Truncated ROS point cloud header.");
    const auto seconds = qFromLittleEndian<quint32>(message.payload.constData() + 4);
    const auto nanos = qFromLittleEndian<quint32>(message.payload.constData() + 8);
    if (nanos >= Second)
        throw std::runtime_error("ROS point cloud header time is invalid.");
    const quint64 time = seconds * Second + nanos;
    return time ? time : message.time;
}

void save(QSaveFile &file, const QByteArray &data)
{
    if (data.size() > RecordingLimit - file.pos() || file.write(data) != data.size())
        throw std::runtime_error("Could not write the temporary bridge recording or the capture exceeded 4 GiB.");
}
}

void captureLiveBag(const QString &destination, const LiveCaptureOptions &options)
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, options.port))
        throw std::runtime_error(server.errorString().toUtf8().constData());
    QTextStream(stdout) << "Waiting for the ROS1 relay at 127.0.0.1:" << server.serverPort() << Qt::endl;
    if (!server.waitForNewConnection(60'000))
        throw std::runtime_error("No ROS1 relay connected within 60 seconds.");

    auto *peer = server.nextPendingConnection();
    if (!peer)
        throw std::runtime_error("The ROS relay disconnected before capture started.");
    auto &stream = *peer;
    server.close();
    stream.setReadBufferSize(bridge::MaxMessage + 4);
    if (receive(stream, int(bridge::Magic.size())) != bridge::Magic)
        throw std::runtime_error("Unsupported ROS bridge protocol.");

    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Could not open the temporary bridge recording.");
    save(file, "#ROSBAG V2.0\n");
    QByteArray chunk;
    const auto flush = [&] {
        if (chunk.isEmpty())
            return;
        save(file, record(5, entry("compression", "none") + entry("size", bridge::u32(quint32(chunk.size()))), chunk));
        chunk.clear();
    };

    const QByteArray cloud = options.cloudTopic.toUtf8();
    const QByteArray odometry = options.odometryTopic.toUtf8();
    const auto length = quint64((options.seconds + options.delay) * double(Second));
    QHash<QPair<QByteArray, QByteArray>, quint32> connections;
    quint64 firstCloud = 0, lastCloud = 0;
    while (true) {
        const auto prefix = receive(stream, 4, true);
        if (prefix.isEmpty())
            break;
        const auto size = qFromLittleEndian<quint32>(prefix.constData());
        if (size < 24 || size > bridge::MaxMessage)
            throw std::runtime_error("ROS bridge frame exceeds allowed size.");
        const auto message = bridge::decode(receive(stream, int(size)));
        const bool pointCloud = message.topic == cloud && message.type == "sensor_msgs/PointCloud2";
        const bool pose = message.topic == odometry && message.type == "nav_msgs/Odometry";
        const bool transform = (message.topic == "/tf" || message.topic == "/tf_static") &&
            (message.type == "tf2_msgs/TFMessage" || message.type == "tf/tfMessage");
        if (!pointCloud && !pose && !transform)
            continue;

        const auto key = qMakePair(message.topic, message.type);
        auto connection = connections.constFind(key);
        if (connection == connections.cend()) {
            const auto id = quint32(connections.size());
            save(file, record(7, entry("conn", bridge::u32(id)) + entry("topic", message.topic),
                              entry("type", message.type) + entry("message_definition", message.definition)));
            connections.insert(key, id);
            connection = connections.constFind(key);
        }
        if (pointCloud) {
            const quint64 time = headerTime(message);
            if (lastCloud && time < lastCloud)
                throw std::runtime_error("Point cloud time moved backwards. Start a new capture for each replay.");
            if (!firstCloud)
                firstCloud = time;
            lastCloud = time;
        }
        chunk += record(2, entry("conn", bridge::u32(connection.value())) + entry("time", stamp(message.time)), message.payload);
        if (chunk.size() >= ChunkTarget)
            flush();
        if (firstCloud && lastCloud - firstCloud >= length)
            break;
    }
    if (!firstCloud)
        throw std::runtime_error("The relay sent no point cloud data for the selected topic.");
    flush();
    if (!file.commit())
        throw std::runtime_error("Could not finish the temporary bridge recording.");
}
}
