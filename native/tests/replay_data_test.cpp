#include "replay_data.h"
#include "transform_tree.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    QByteArray result = u32(0) + stamp(sec) + blob("scanner");
    for (float value : {0.f, 1.5707963f, 1.5707963f, 0.f, 0.f, .2f, 20.f})
        result += u32(std::bit_cast<quint32>(value));
    return result + u32(2) + u32(std::bit_cast<quint32>(2.f)) + u32(std::bit_cast<quint32>(3.f)) + u32(0);
}

QByteArray livox(quint32 sec)
{
    QByteArray result = u32(0) + stamp(sec) + blob("scanner") + QByteArray(8, 0)
        + u32(1) + QByteArray(4, 0) + u32(1) + u32(0);
    for (const float coordinate : {5.f, 6.f, 7.f})
        result += u32(std::bit_cast<quint32>(coordinate));
    return result + QByteArray(3, 0);
}

QByteArray transform(const QByteArray &child, double x)
{
    QByteArray result = u32(0) + stamp(1) + blob("map") + blob(child);
    for (const double value : {x, 0., 0., 0., 0., 0., 1.}) {
        char bytes[8];
        qToLittleEndian(std::bit_cast<quint64>(value), bytes);
        result.append(bytes, sizeof(bytes));
    }
    return result;
}

QByteArray number64(double value)
{
    char bytes[8];
    qToLittleEndian(std::bit_cast<quint64>(value), bytes);
    return QByteArray(bytes, sizeof(bytes));
}

QByteArray spline(quint32 seconds)
{
    QByteArray data = u32(1) + QByteArray(8, 0) + stamp(seconds) + u32(4);
    for (double value : {0., 0., 1., 1.})
        data += number64(value);
    data += u32(2);
    for (double value : {1., 2., 3., 9., 2., 3.})
        data += number64(value);
    return data + u32(0) + number64(0.);
}

QByteArray velocity(quint32 seconds, double speed)
{
    return u32(0) + stamp(seconds) + blob("map") + number64(speed) + QByteArray(5*8, 0);
}

QByteArray cameraInfo(quint32 seconds)
{
    QByteArray result = u32(0) + stamp(seconds) + blob("optical") + u32(480) + u32(640) + blob("plumb_bob") + u32(0);
    for (const double coefficient : {320., 0., 320., 0., 240., 240., 0., 0., 1.})
        result += number64(coefficient);
    return result + QByteArray((9+12)*8 + 2*4 + 4*4 + 1, 0);
}

QByteArray marker(quint32 seconds, quint32 id, quint32 type, quint32 action, quint32 lifetime = 0)
{
    QByteArray data = u32(0) + stamp(seconds) + blob("map") + blob("native");
    data += u32(id) + u32(type) + u32(action);
    for (double value : {4., 2., 3., 0., 0., 0., 1., 2., 2., 2.})
        data += number64(value);
    for (float value : {1.f, .4f, 0.f, .5f})
        data += u32(std::bit_cast<quint32>(value));
    return data + stamp(lifetime) + QByteArray(1, 0) + QByteArray(4*4+1, 0);
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

        replay::TransformTree tree;
        tree.add("map", "odom", 0, {{10, 0, 0}, {}}, true);
        tree.add("odom", "body", 1'000'000'000, {{0, 1, 0}, {}}, false);
        tree.add("odom", "body", 1'400'000'000, {{0, 3, 0}, {}}, false);
        tree.prepare();
        const auto world = tree.lookup("map", "body", 1'200'000'000);
        const auto inverse = tree.lookup("body", "map", 1'200'000'000);
        require(world && inverse && world->translation == QVector3D(10, 2, 0) &&
                inverse->map(world->translation) == QVector3D(), "TF chain interpolation or inverse is incorrect.");
        require(!tree.lookup("map", "body", 1'500'000'000), "TF extrapolated outside recorded coverage.");

        const QByteArray schema =
            "Header header\nSensor[] readings\nuint64 count\n"
            "================================================================================\n"
            "MSG: fixture/Sensor\nstring name\nfloat64[] samples\n";
        QByteArray message = u32(1) + stamp(2) + blob("map") + u32(1) + blob("lidar") + u32(129);
        for (int i = 0; i < 129; ++i)
            message += number64(double(i) / 4.);
        message += QByteArray(8, char(255));
        const auto inspected = replay::decodeTopicMessage("fixture/Reading", schema, message, 2'000'000'000);
        const auto object = QJsonDocument::fromJson(inspected.fields.toUtf8()).object();
        const auto readings = object.value("readings").toArray();
        require(!readings.isEmpty() && object.value("count").toString() == "18446744073709551615" &&
                object.value("header").toObject().value("frame_id").toString() == "map",
                "Custom ROS message fields did not decode.");
        const auto samples = readings[0].toObject().value("samples").toObject();
        require(samples.value("total").toInt() == 129 &&
                samples.value("items").toArray().at(4).toDouble() == 1., "Large ROS array summary is incorrect.");
        const auto secondPage = replay::decodeTopicMessage("fixture/Reading", schema, message, 2'000'000'000, 1);
        const auto secondObject = QJsonDocument::fromJson(secondPage.fields.toUtf8()).object();
        const auto lastSamples = secondObject.value("readings").toArray().first().toObject().value("samples").toObject();
        require(inspected.arrayPageCount == 2 && lastSamples.value("offset").toInt() == 128 &&
                lastSamples.value("items").toArray().size() == 1 &&
                lastSamples.value("items").toArray().first().toDouble() == 32., "ROS array paging missed the final entry.");
        const auto imageBuffer = u32(1) + stamp(2) + blob("optical") + u32(2) + u32(1) + blob("bgr8") +
            QByteArray(1, 0) + u32(4) + blob(QByteArray::fromHex("0708090028323c00"));
        const auto imageMessage = replay::decodeTopicMessage("sensor_msgs/Image", {}, imageBuffer, 2'000'000'000);
        require(imageMessage.image.size() == QSize(1, 2) &&
                imageMessage.image.pixelColor(0, 0) == QColor(9, 8, 7) &&
                imageMessage.image.pixelColor(0, 1) == QColor(60, 50, 40), "ROS camera frame row stride was not honored.");

        QTemporaryDir dir;
        QFile file(dir.filePath("synthetic.bag"));
        require(file.open(QIODevice::WriteOnly), "Could not create temporary test file.");
        const auto connection = field("conn", u32(0));
        const auto secondary = field("conn", u32(1));
        const auto pathConnection = field("conn", u32(2));
        const auto scanConnection = field("conn", u32(3));
        const auto tfConnection = field("conn", u32(4));
        const auto livoxConnection = field("conn", u32(5));
        const auto payload = record(2, tfConnection + field("time", stamp(1)),
                                    u32(2) + transform("scanner", 10.) + transform("published", 20.))
                           + record(2, connection + field("time", stamp(1)), cloud(1, 4.f))
                           + record(2, secondary + field("time", stamp(2)), cloud(2, 42.f))
                           + record(2, pathConnection + field("time", stamp(2)), path(2))
                           + record(2, scanConnection + field("time", stamp(2)), scan(2))
                           + record(2, livoxConnection + field("time", stamp(2)), livox(2))
                           + record(2, secondary + field("time", stamp(3)), cloud(3, 90.f, "camera"))
                           + record(2, connection + field("time", stamp(3)), cloud(3, 9.f));
        const auto contents = QByteArray("#ROSBAG V2.0\n")
            + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload)
            + record(7, connection + field("topic", "/rog_map/occ"), field("type", "sensor_msgs/PointCloud2"))
            + record(7, secondary + field("topic", "/secondary"), field("type", "sensor_msgs/PointCloud2"))
            + record(7, pathConnection + field("topic", "/plan"), field("type", "nav_msgs/Path"))
            + record(7, scanConnection + field("topic", "/laser"), field("type", "sensor_msgs/LaserScan"))
            + record(7, tfConnection + field("topic", "/tf_static"), field("type", "tf2_msgs/TFMessage"))
            + record(7, livoxConnection + field("topic", "/livox/lidar"), field("type", "livox_ros_driver2/CustomMsg"));
        require(file.write(contents) == contents.size(), "Could not write temporary test file.");
        file.close();
        const auto inventory = replay::inspectBag(file.fileName());
        const auto topics = inventory.value("topics").toArray();
        require(inventory.value("storage_start_ns").toInteger() == 1'000'000'000 &&
                inventory.value("storage_end_ns").toInteger() == 3'000'000'001 && topics.size() == 6 &&
                topics.first().toObject().value("count").toInt() == 2 &&
                topics.first().toObject().value("type").toString() == "sensor_msgs/msg/PointCloud2" &&
                inventory.value("tf_pairs").toArray().size() == 2,
                "Bag inventory lost a storage timestamp, count, type or TF pair.");
        const auto bag = replay::loadBag(file.fileName());
        const std::vector<replay::TopicDisplay> lazyDisplays{
            {"lazy", "/secondary", "cloud", "Lazy cloud", QColor("#ff0000")},
            {"laser", "/laser", "laser", "Lazy laser", QColor("#00ff00")}};
        const auto lazy = replay::loadDisplayData(file.fileName(), "map", lazyDisplays);
        require(lazy->cloudTopic.isEmpty() && lazy->odometryTopic.isEmpty() && lazy->speedTopic.isEmpty()
            && lazy->points.empty() && lazy->poses.empty() && !lazy->showMainCloud,
            "Lazy Display data selected legacy global roles.");
        require(lazy->topicOverlays.size() == 2 && lazy->topicOverlays[0].points.front().x == 42.f
            && lazy->topicOverlays[1].points.front().x == 12.f && lazy->transforms,
            "Lazy Display data lost requested topics or actual TF.");
        const auto missingFrame = replay::loadDisplayData(file.fileName(), "not_available", lazyDisplays);
        require(missingFrame->topicOverlays[0].samples.empty() && !missingFrame->topicWarnings.empty(),
            "Lazy Display data silently ignored missing TF.");
        const auto cachedTarget = bag->transforms ? bag->transforms->lookup("map", "scanner", 2'000'000'000) : std::nullopt;
        require(cachedTarget && cachedTarget->translation.x() == 10.f,
                "Default bag loading must retain TF for camera targets without a configured TF display.");
        require(bag->replace && bag->cloudTopic == "/rog_map/occ", "Planner map defaults are wrong.");
        require(bag->duration() == 2. && bag->points.back().x == 9.f, "ROS1 PointCloud2 data was misread.");
        require(bag->frameAt(bag->stampAt(1)).first == 0 && bag->frameAt(bag->stampAt(2)).first == 1,
                "Timeline chose the wrong PointCloud2 frame.");
        replay::LoadOptions currentScan;
        currentScan.autoScan = true;
        currentScan.scanMaximumAge = .25;
        auto withScan = replay::loadBag(file.fileName(), currentScan);
        require(withScan->scanTopics.contains("/laser") && withScan->scanTopic == "/livox/lidar",
                "The preferred current scan topic was not selected.");
        const auto current = std::find_if(withScan->topicOverlays.begin(), withScan->topicOverlays.end(),
                                          [](const auto &layer) { return layer.display.plannerRole == "scan"; });
        require(current != withScan->topicOverlays.end(), "Current scan did not produce a render layer.");
        const auto &liveScan = *current;
        require(liveScan.display.plannerRole == "scan" && liveScan.points.size() == 1 &&
                liveScan.points.front().x == 15.f && liveScan.points.front().z == 7.f &&
                liveScan.sampleAt(withScan->stampAt(1.125)) && !liveScan.sampleAt(withScan->stampAt(1.25)),
                "Livox current scan was decoded, transformed or expired incorrectly.");
        currentScan.scanTopic = "/rog_map/occ";
        withScan = replay::loadBag(file.fileName(), currentScan);
        require(withScan->scanTopic == withScan->cloudTopic &&
                std::none_of(withScan->topicOverlays.begin(), withScan->topicOverlays.end(),
                             [](const auto &layer) { return layer.display.plannerRole == "scan"; }),
                "The current primary scan should reuse the parsed map points.");
        const auto topicAtTime = replay::inspectBagTopic(file.fileName(), "/plan", 2'500'000'000);
        require(topicAtTime && topicAtTime->stamp == 2'000'000'000 && topicAtTime->type == "nav_msgs/Path" &&
                !replay::inspectBagTopic(file.fileName(), "/plan", 1'900'000'000),
                "Topic inspection did not select the latest message before the playhead.");
        replay::LoadOptions configured;
        configured.showMainCloud = false;
        configured.showPlan = false;
        configured.topicLayout.displays = {
            {"one", "/secondary", "cloud", "Second sensor", QColor("#0a141e")},
            {"two", "/secondary", "cloud", "Disabled sensor", QColor("#ffffff"), false},
            {"three", "/missing", "path", "Missing path", QColor("#ffffff")},
            {"four", "/plan", "path", "Plan", QColor("#ffff00")},
            {"five", "/laser", "laser", "Scan", QColor("#0000ff")},
            {"six", "/tf_static", "tf", "Static transforms", QColor("#ff0000")}
        };
        const auto overlayBag = replay::loadBag(file.fileName(), configured);
        require(!overlayBag->showMainCloud && overlayBag->topicOverlays.size() == 6, "Configured displays were not cached or cloud fallback is wrong.");
        require(!overlayBag->topicOverlays[1].display.enabled && !overlayBag->topicOverlays[1].samples.empty(),
                "Initially disabled display must retain decoded data for live visibility changes.");
        const auto &overlay = overlayBag->topicOverlays.front();
        require(overlay.display.color == QColor("#0a141e") && overlay.samples.size() == 1 &&
                overlay.points[size_t(overlay.samples.front().range.first)].x == 42.f,
                "Imported layer color, source or frame filtering is incorrect.");
        require(!overlay.sampleAt(overlayBag->stampAt(0)) && overlay.sampleAt(overlayBag->stampAt(2)) == &overlay.samples.front(),
                "Imported overlay did not follow the replay clock.");
        const auto &pathLayer = overlayBag->topicOverlays[2];
        require(pathLayer.points.size() == 1 && pathLayer.points.front().z == 3.f, "Path points were misread.");
        const auto &scanLayer = overlayBag->topicOverlays[3];
        require(scanLayer.points.size() == 2 && scanLayer.points.front().x == 12.f &&
                std::abs(scanLayer.points.back().y - 3.f) < .001f, "LaserScan polar coordinates were misread.");
        require(overlayBag->topicOverlays[4].points.front().x == 10.f &&
                overlayBag->topicOverlays[5].points.front().x == 20.f, "TF layer lost one of the child frames.");
        require(overlayBag->topicWarnings.contains("Missing path"), "Missing topic did not produce a warning.");
        configured.topicLayout.fixedFrame = "published";
        const auto fixed = replay::loadBag(file.fileName(), configured);
        require(fixed->frameId == "published" && fixed->points.front().x == -16.f &&
                fixed->topicOverlays[3].points.front().x == -8.f, "Imported fixed frame did not map every layer.");
        configured.topicLayout.fixedFrame = "unavailable";
        const auto fallback = replay::loadBag(file.fileName(), configured);
        require(fallback->frameId == "map" && fallback->points.front().x == 4.f, "Missing fixed frame moved the main view.");
        bool cancelled = false;
        try { replay::loadBag(file.fileName(), {}, [] { return true; }); }
        catch (const std::runtime_error &) { cancelled = true; }
        require(cancelled, "Cancelled bag import did not stop.");

        QFile plannerFile(dir.filePath("planners.bag"));
        require(plannerFile.open(QIODevice::WriteOnly), "Could not create the EGO/SUPER fixture.");
        const auto splineConnection = field("conn", u32(1));
        const auto corridorConnection = field("conn", u32(2));
        const auto goalConnection = field("conn", u32(3));
        const auto cameraConnection = field("conn", u32(4));
        const auto plannerTf = field("conn", u32(5));
        const auto speedConnection = field("conn", u32(6));
        const auto plannerMessages =
              record(2, connection + field("time", stamp(1)), cloud(1, 0.f))
            + record(2, splineConnection + field("time", stamp(1)), spline(1))
            + record(2, cameraConnection + field("time", stamp(1)), cameraInfo(1))
            + record(2, plannerTf + field("time", stamp(1)), u32(1) + transform("optical", 2.))
            + record(2, speedConnection + field("time", stamp(1)), velocity(1, 3.))
            + record(2, speedConnection + field("time", stamp(3)), velocity(3, 7.))
            + record(2, corridorConnection + field("time", stamp(1)), u32(2) + marker(1, 5, 1, 0) + marker(1, 6, 1, 0))
            + record(2, goalConnection + field("time", stamp(1)), marker(1, 1, 2, 0, 1))
            + record(2, corridorConnection + field("time", stamp(2)), u32(1) + marker(2, 0, 1, 3))
            + record(2, connection + field("time", stamp(3)), cloud(3, 9.f));
        const auto plannerFixture = QByteArray("#ROSBAG V2.0\n")
            + record(5, field("compression", "none") + field("size", u32(quint32(plannerMessages.size()))), plannerMessages)
            + record(7, connection + field("topic", "/cloud_registered"), field("type", "sensor_msgs/PointCloud2"))
            + record(7, splineConnection + field("topic", "/drone/planning/bspline"), field("type", "traj_utils/Bspline"))
            + record(7, corridorConnection + field("topic", "/drone/visualization/exp_sfc"), field("type", "visualization_msgs/MarkerArray"))
            + record(7, goalConnection + field("topic", "/drone/visualization/goal"), field("type", "visualization_msgs/Marker"))
            + record(7, cameraConnection + field("topic", "/camera/color/camera_info"), field("type", "sensor_msgs/CameraInfo"))
            + record(7, plannerTf + field("topic", "/tf_static"), field("type", "tf2_msgs/TFMessage"))
            + record(7, speedConnection + field("topic", "/drone/velocity"), field("type", "geometry_msgs/TwistStamped"));
        require(plannerFile.write(plannerFixture) == plannerFixture.size(), "Could not write the planner fixture.");
        plannerFile.close();
        const auto planner = replay::loadBag(plannerFile.fileName());
        require(planner->topicOverlays.size() == 5 && planner->planningEvents.size() == 2,
                "Native EGO/SUPER topics or planning events were not detected.");
        const auto &planned = planner->topicOverlays[0];
        require(planned.display.plannerRole == "plan" && planned.points.size() > 16 &&
                planned.points.front().x == 1.f && planned.points.back().x == 9.f,
                "EGO spline was not evaluated at both endpoints.");
        const auto &corridor1 = planner->topicOverlays[1];
        const auto &corridor2 = planner->topicOverlays[4];
        const auto &goal = planner->topicOverlays[2];
        require(corridor1.points.size() == 24 && corridor2.points.size() == 24 &&
                corridor1.sampleAt(1'500'000'000) && corridor2.sampleAt(1'500'000'000) &&
                !corridor1.sampleAt(2'000'000'000) && !corridor2.sampleAt(2'000'000'000) &&
                corridor1.sampleAt(1'500'000'000), "Marker identities or delete-all rewind are incorrect.");
        require(goal.sampleAt(1'500'000'000) && !goal.sampleAt(2'000'000'000),
                "Marker lifetime was not honored.");

        const auto &camera = planner->topicOverlays[3];
        const auto cameraPose = planner->transforms->lookup("map", "optical", 1'500'000'000);
        require(camera.display.plannerRole == "camera" && camera.points.size() == 16 &&
                camera.samples.front().frameLocked && std::abs(camera.points[1].z - .85f) < .001f &&
                cameraPose && cameraPose->translation.x() == 2.f, "CameraInfo frustum or TF frame-lock is incorrect.");

        replay::LoadOptions tfPose;
        tfPose.tfPose = true;
        tfPose.world = "map";
        tfPose.body = "optical";
        tfPose.speedTopic = "/drone/velocity";
        tfPose.maximumGap = 3.;
        const auto fromTf = replay::loadBag(plannerFile.fileName(), tfPose);
        const auto tfSample = fromTf->poseAt(2'000'000'000);
        const auto independentSpeed = fromTf->speedAt(2'000'000'000);
        require(fromTf->tfPose && fromTf->bodyFrame == "optical" && tfSample && tfSample->position.x() == 2.f &&
                independentSpeed && *independentSpeed == 5.f, "TF pose or independent velocity playback is incorrect.");

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
