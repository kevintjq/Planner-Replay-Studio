#include "replay_controller.h"
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
QByteArray u32(quint32 value) { char data[4]; qToLittleEndian(value, data); return QByteArray(data, 4); }
QByteArray blob(const QByteArray &value) { return u32(quint32(value.size())) + value; }
QByteArray field(const QByteArray &key, const QByteArray &value) { return blob(key + '=' + value); }
QByteArray record(quint8 op, const QByteArray &header, const QByteArray &body) {
    return blob(field("op", QByteArray(1, char(op))) + header) + blob(body);
}
QByteArray stamp(quint32 seconds) { return u32(seconds) + u32(0); }
QByteArray cloud(quint32 seconds) {
    auto data = u32(0) + stamp(seconds) + blob("map") + u32(1) + u32(1) + u32(3);
    for (int i = 0; i < 3; ++i)
        data += blob(QByteArray(1, char('x' + i))) + u32(4 * i) + QByteArray(1, 7) + u32(1);
    return data + QByteArray(1, 0) + u32(12) + u32(12) + blob(u32(std::bit_cast<quint32>(float(seconds))) + u32(0) + u32(0)) + QByteArray(1, 1);
}
void write(const QString &path, const QByteArray &bytes) {
    QFile file(path); expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Cannot write test fixture");
}
void waitForLoad(ReplayController &controller) {
    if (!controller.loading()) return;
    QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
    QObject::connect(&controller, &ReplayController::stateChanged, &loop, [&] { if (!controller.loading()) loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(90000); loop.exec();
    expect(!controller.loading(), "Bag load timed out");
}
void advanceEvents() {
    QEventLoop loop; QTimer::singleShot(80, &loop, &QEventLoop::quit); loop.exec();
}
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        QTemporaryDir directory; expect(directory.isValid(), "No temporary directory");
        QString bag;
        if (argc > 1) bag = QString::fromLocal8Bit(argv[1]);
        else {
            bag = directory.filePath("visibility.bag");
            QByteArray payload;
            for (quint32 second : {1u, 2u, 3u})
                payload += record(2, field("conn", u32(0)) + field("time", stamp(second)), cloud(second));
            write(bag, QByteArray("#ROSBAG V2.0\n") + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload)
                + record(7, field("conn", u32(0)) + field("topic", "/cloud"), field("type", "sensor_msgs/PointCloud2")));
        }
        ReplayController baseline; baseline.openBag(QUrl::fromLocalFile(bag)); waitForLoad(baseline);
        expect(baseline.duration() > 0. && !baseline.cloudTopic().isEmpty(), "Initial bag load failed");
        const auto rviz = directory.filePath("hidden.rviz");
        write(rviz, "Visualization Manager:\n  Displays:\n    - Class: rviz/PointCloud2\n      Name: Hidden cloud\n      Enabled: false\n      Topic: '"
            + baseline.cloudTopic().toUtf8().replace("'", "''") + "'\n      Color: 60; 160; 240\n    - Class: rviz/TF\n      Enabled: false\n");
        QJsonArray cases;
        for (bool beforeBag : {true, false}) {
            ReplayController controller;
            if (beforeBag) controller.openRviz(QUrl::fromLocalFile(rviz));
            controller.openBag(QUrl::fromLocalFile(bag)); waitForLoad(controller);
            if (!beforeBag) { controller.openRviz(QUrl::fromLocalFile(rviz)); waitForLoad(controller); }
            const auto data = controller.recording();
            expect(controller.topicDisplays().size() == 3 && !data->showMainCloud, "Import order changed display configuration or fallback cloud");
            const auto cloudLayer = std::find_if(data->topicOverlays.begin(), data->topicOverlays.end(), [](const auto &layer) { return layer.display.kind == "cloud"; });
            expect(cloudLayer != data->topicOverlays.end() && !cloudLayer->display.enabled && !cloudLayer->samples.empty(), "Hidden cloud was not decoded");
            controller.seek(controller.duration() * .3); controller.setCamera(4); controller.addMarker();
            controller.addSceneText("Visibility state preservation"); controller.addCloudView(2);
            const auto markers = controller.markers(), layers = controller.sceneLayers(), views = controller.cloudViews();
            const auto position = controller.position(); const auto *points = data->points.data();
            int loadSignals = 0;
            QObject::connect(&controller, &ReplayController::stateChanged, &controller, [&] { if (controller.loading()) ++loadSignals; });
            controller.togglePlayback();
            for (int cycle = 0; cycle < 20; ++cycle) {
                controller.setTopicDisplayEnabled(0, true); controller.setTopicDisplayEnabled(0, false);
                expect(controller.playing() && !controller.loading() && controller.recording() == data && data->points.data() == points, "Visibility recreated recording or paused playback");
            }
            advanceEvents();
            expect(controller.position() > position && loadSignals == 0, "Playback did not advance or bag was reopened");
            controller.pause(); controller.seek(position); controller.setTopicDisplayEnabled(0, true);
            expect(!controller.playing() && controller.position() == position && controller.cameraIndex() == 4 &&
                controller.markers() == markers && controller.sceneLayers() == layers && controller.cloudViews() == views, "Paused edit or camera state changed");
            controller.setTopicDisplayEnabled(1, true);
            expect(controller.topicDisplays()[1].toMap()["enabled"].toBool() && controller.topicDisplays()[2].toMap()["enabled"].toBool(), "Single TF node split into conflicting visibility states");
            const auto project = directory.filePath(beforeBag ? "before.slamfpv" : "after.slamfpv");
            controller.saveProjectAs(QUrl::fromLocalFile(project));
            const auto saved = replay::ProjectDocument::load(project).topicLayout();
            expect(saved.displays[0].enabled && saved.displays[1].enabled && saved.displays[2].enabled, "Visibility was not persisted");
            expect(!replay::serializeRviz(saved).isEmpty(), "Coupled TF cannot be exported");
            const int toggleLoadSignals = loadSignals;
            controller.openProject(QUrl::fromLocalFile(project)); waitForLoad(controller);
            expect(controller.topicDisplays()[0].toMap()["enabled"].toBool(), "Saved visibility not restored");
            cases.append(QJsonObject{{"config_before_bag", beforeBag}, {"passed", true}, {"visibility_cycles", 20}, {"load_signals_during_toggles", toggleLoadSignals}});
        }
        std::cout << QJsonDocument(QJsonObject{{"passed", true}, {"real_bag", argc > 1}, {"cases", cases}, {"gpu_rendering_verified", false}}).toJson().constData();
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
