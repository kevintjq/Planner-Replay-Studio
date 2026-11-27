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
QByteArray cloud(quint32 sec, float x, const QByteArray &frame = "map")
{
    QByteArray data = u32(0) + stamp(sec) + blob(frame) + u32(1) + u32(1) + u32(3);
    for (int i = 0; i < 3; ++i)
        data += blob(QByteArray(1, char('x'+i))) + u32(4*i) + QByteArray(1, char(7)) + u32(1);
    return data + QByteArray(1, 0) + u32(12) + u32(12)
        + blob(u32(std::bit_cast<quint32>(x)) + u32(0) + u32(0)) + QByteArray(1, char(1));
}

QByteArray path(quint32 sec)
{
    QByteArray xyz;
    for (const double coordinate : {1., 2., 3.}) {
        char bytes[8];
        qToLittleEndian(std::bit_cast<quint64>(coordinate), bytes);
        xyz.append(bytes, sizeof(bytes));
    }
    const auto header = u32(0) + stamp(sec) + blob("map");
    return header + u32(1) + header + xyz + QByteArray(4 * 8, 0);
}

QByteArray scan(quint32 sec)
{
    QByteArray result = u32(0) + stamp(sec) + blob("map");
    for (float value : {0.f, 1.5707963f, 1.5707963f, 0.f, 0.f, .2f, 20.f})
        result += u32(std::bit_cast<quint32>(value));
    return result + u32(2) + u32(std::bit_cast<quint32>(2.f)) + u32(std::bit_cast<quint32>(3.f)) + u32(0);
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
        const auto secondary = field("conn", u32(1));
        const auto pathConnection = field("conn", u32(2));
        const auto scanConnection = field("conn", u32(3));
        const auto payload = record(2, connection + field("time", stamp(1)), cloud(1, 4.f))
                           + record(2, secondary + field("time", stamp(2)), cloud(2, 42.f))
                           + record(2, pathConnection + field("time", stamp(2)), path(2))
                           + record(2, scanConnection + field("time", stamp(2)), scan(2))
                           + record(2, secondary + field("time", stamp(3)), cloud(3, 90.f, "camera"))
                           + record(2, connection + field("time", stamp(3)), cloud(3, 9.f));
        const auto contents = QByteArray("#ROSBAG V2.0\n")
            + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload)
            + record(7, connection + field("topic", "/rog_map/occ"), field("type", "sensor_msgs/PointCloud2"))
            + record(7, secondary + field("topic", "/secondary"), field("type", "sensor_msgs/PointCloud2"))
            + record(7, pathConnection + field("topic", "/plan"), field("type", "nav_msgs/Path"))
            + record(7, scanConnection + field("topic", "/laser"), field("type", "sensor_msgs/LaserScan"));
        require(file.write(contents) == contents.size(), "Could not write temporary test file.");
        file.close();
        const auto bag = replay::loadBag(file.fileName());
        require(bag->replace && bag->cloudTopic == "/rog_map/occ", "Planner map defaults are wrong.");
        require(bag->duration() == 2. && bag->points.back().x == 9.f, "ROS1 PointCloud2 data was misread.");
        require(bag->frameAt(bag->stampAt(1)).first == 0 && bag->frameAt(bag->stampAt(2)).first == 1,
                "Timeline chose the wrong PointCloud2 frame.");
        replay::LoadOptions configured;
        configured.showMainCloud = false;
        configured.topicLayout.displays = {
            {"one", "/secondary", "cloud", "Second sensor", QColor("#0a141e")},
            {"two", "/secondary", "cloud", "Disabled sensor", QColor("#ffffff"), false},
            {"three", "/missing", "path", "Missing path", QColor("#ffffff")},
            {"four", "/plan", "path", "Plan", QColor("#ffff00")},
            {"five", "/laser", "laser", "Scan", QColor("#0000ff")}
        };
        const auto overlayBag = replay::loadBag(file.fileName(), configured);
        require(!overlayBag->showMainCloud && overlayBag->topicOverlays.size() == 3, "Enabled cloud layer did not replace the default cloud.");
        const auto &overlay = overlayBag->topicOverlays.front();
        require(overlay.display.color == QColor("#0a141e") && overlay.samples.size() == 1 &&
                overlay.points[size_t(overlay.samples.front().range.first)].x == 42.f,
                "Imported layer color, source or frame filtering is incorrect.");
        require(!overlay.sampleAt(overlayBag->stampAt(0)) && overlay.sampleAt(overlayBag->stampAt(2)) == &overlay.samples.front(),
                "Imported overlay did not follow the replay clock.");
        const auto &pathLayer = overlayBag->topicOverlays[1];
        require(pathLayer.points.size() == 1 && pathLayer.points.front().z == 3.f, "Path points were misread.");
        const auto &scanLayer = overlayBag->topicOverlays[2];
        require(scanLayer.points.size() == 2 && scanLayer.points.front().x == 2.f &&
                std::abs(scanLayer.points.back().y - 3.f) < .001f, "LaserScan polar coordinates were misread.");
        require(overlayBag->topicWarnings.contains("Missing path"), "Missing topic did not produce a warning.");
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
