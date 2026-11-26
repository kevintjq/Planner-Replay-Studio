#include "replay_data.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
QByteArray u32(quint32 n)
{
    char bytes[4];
    qToLittleEndian(n, bytes);
    return QByteArray(bytes, sizeof(bytes));
}

QByteArray blob(const QByteArray &bytes) { return u32(quint32(bytes.size())) + bytes; }
QByteArray field(const QByteArray &key, const QByteArray &data) { return blob(key + '=' + data); }
QByteArray record(quint8 op, const QByteArray &header, const QByteArray &body)
{
    return blob(field("op", QByteArray(1, char(op))) + header) + blob(body);
}
QByteArray stamp(quint32 sec) { return u32(sec) + u32(0); }
QByteArray cloud(quint32 sec, float x)
{
    QByteArray data = u32(0) + stamp(sec) + blob("map") + u32(1) + u32(1) + u32(3);
    for (int i = 0; i < 3; ++i)
        data += blob(QByteArray(1, char('x'+i))) + u32(4*i) + QByteArray(1, char(7)) + u32(1);
    return data + QByteArray(1, 0) + u32(12) + u32(12)
        + blob(u32(std::bit_cast<quint32>(x)) + u32(0) + u32(0)) + QByteArray(1, char(1));
}

void require(bool good, const char *message)
{
    if (!good)
        throw std::runtime_error(message);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const auto demo = replay::makeDemo();
        require(demo->points.size() > 100'000, "Demo point data is missing.");
        require(demo->frameAt(demo->stampAt(0)).end < demo->frameAt(demo->stampAt(demo->duration())).end,
                "Timeline did not accumulate points.");
        const auto pose = demo->poseAt(demo->stampAt(1.032));
        require(pose && pose->speed && *pose->speed > 5, "Demo interpolated pose and speed are missing.");

        QTemporaryDir dir;
        QFile file(dir.filePath("synthetic.bag"));
        require(file.open(QIODevice::WriteOnly), "Could not create temporary test file.");
        const auto connection = field("conn", u32(0));
        const auto payload = record(2, connection + field("time", stamp(1)), cloud(1, 4.f))
                           + record(2, connection + field("time", stamp(3)), cloud(3, 9.f));
        const auto contents = QByteArray("#ROSBAG V2.0\n")
            + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload)
            + record(7, connection + field("topic", "/rog_map/occ"), field("type", "sensor_msgs/PointCloud2"));
        require(file.write(contents) == contents.size(), "Could not write temporary test file.");
        file.close();
        const auto bag = replay::loadBag(file.fileName());
        require(bag->replace && bag->cloudTopic == "/rog_map/occ", "Planner map defaults are wrong.");
        require(bag->duration() == 2. && bag->points.back().x == 9.f, "ROS1 PointCloud2 data was misread.");
        require(bag->frameAt(bag->stampAt(1)).first == 0 && bag->frameAt(bag->stampAt(2)).first == 1,
                "Timeline chose the wrong PointCloud2 frame.");
        bool cancelled = false;
        try { replay::loadBag(file.fileName(), {}, [] { return true; }); }
        catch (const std::runtime_error &) { cancelled = true; }
        require(cancelled, "Cancelled bag import did not stop.");

        for (const auto &path : app.arguments().mid(1)) {
            const auto real = replay::loadBag(path);
            require(real->frames.size() > 1 && real->points.size() > 100, "Recorded fixture data is missing.");
            require(!real->poses.empty() && real->poseAt(real->poses.front().stamp)->speed,
                    "Recorded odometry speed is missing.");
            std::cout << real->frames.size() << " scans, " << real->points.size() << " points, "
                      << real->poses.size() << " poses\n";
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
