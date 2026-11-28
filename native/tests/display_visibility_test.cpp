#include "replay_controller.h"
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include "transform_tree.h"
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
QByteArray u32(quint32 value) { char data[4]; qToLittleEndian(value, data); return QByteArray(data, 4); }
QByteArray real(double value) { char data[8]; qToLittleEndian(std::bit_cast<quint64>(value), data); return QByteArray(data, 8); }
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
QByteArray staticTransform() {
    return u32(1) + u32(0) + stamp(0) + blob("map") + blob("body") +
        real(2.) + real(3.) + real(1.) + real(0.) + real(0.) + real(0.) + real(1.);
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
void advanceEvents(int milliseconds = 80) {
    QEventLoop loop; QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit); loop.exec();
}
void waitForDisplays(ReplayController &controller) {
    QElapsedTimer elapsed; elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!controller.independentDisplayLoading()) { QCoreApplication::processEvents(); return; }
    } while (elapsed.elapsed() < 90000);
    throw std::runtime_error("Independent Topic decode timed out");
}
QVariantMap displayRow(const ReplayController &controller, const QString &id) {
    for (const auto &value : controller.independentDisplays()) if (value.toMap().value("id").toString() == id) return value.toMap();
    return {};
}
QVariantList videoEdits(QVariantList layers) {
    // Visible timeline spans are derived from the newly loaded Bag duration;
    // identity, timing, layout, anchors, crop and style must remain unchanged.
    for (auto &value : layers) { auto row = value.toMap(); row.remove("segments"); value = row; }
    return layers;
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
            for (quint32 second : {1u, 2u, 3u}) {
                payload += record(2, field("conn", u32(0)) + field("time", stamp(second)), cloud(second));
                if (second >= 2) payload += record(2, field("conn", u32(1)) + field("time", stamp(second)), cloud(second));
            }
            write(bag, QByteArray("#ROSBAG V2.0\n") + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload)
                + record(7, field("conn", u32(0)) + field("topic", "/cloud"), field("type", "sensor_msgs/PointCloud2"))
                + record(7, field("conn", u32(1)) + field("topic", "/other_cloud"), field("type", "sensor_msgs/PointCloud2")));
        }
        if (argc == 1) {
            ReplayController controller;
            expect(!controller.demo() && controller.recording()->points.empty() && !controller.recording()->hasMessages,
                "Startup silently installed simulated ROS data");
            expect(controller.independentDisplayStates().size() == 2 && controller.independentDisplayModelActive(),
                "Empty-data startup has no editable Grid/Axes configuration");
            controller.exportRviz(QUrl::fromLocalFile(directory.filePath("empty-start.rviz")));
            expect(QFileInfo::exists(directory.filePath("empty-start.rviz")), "Default scene cannot export before loading a Bag");
            const auto emptyProject = directory.filePath("empty-start.uavstudio"); controller.saveProjectAs(QUrl::fromLocalFile(emptyProject));
            ReplayController emptyRestored; emptyRestored.openProject(QUrl::fromLocalFile(emptyProject));
            expect(!emptyRestored.demo() && emptyRestored.recording()->frames.empty() && emptyRestored.recording()->points.empty() &&
                emptyRestored.experimentStampNs().isEmpty() && emptyRestored.nativeReplaySource().isEmpty(),
                "Reopening a no-source project invented a data frame or experiment timestamp");
            const auto first = controller.addIndependentDisplay("rviz/PointCloud2", "/cloud");
            expect(!first.isEmpty() && displayRow(controller, first).value("status") == "Waiting for Data", "Empty-data Display CRUD or waiting status failed");
            const auto copy = controller.duplicateIndependentDisplay(first);
            expect(!copy.isEmpty() && copy != first, "Duplicate Display did not get an independent identity");
            expect(controller.editIndependentDisplay(copy, {{"color", "#ee2244"}, {"alpha", .25}, {"pointSize", 9.}}), "Standalone style edit failed");
            expect(displayRow(controller, first).value("alpha").toDouble() == 1. && displayRow(controller, copy).value("alpha").toDouble() == .25,
                "Same-topic instances share mutable style state");
            const auto media = directory.filePath("project-reference.mp4"); write(media, "explicit metadata-only media fixture");
            controller.addVideo(QUrl::fromLocalFile(media)); controller.addSceneText("Keep canvas before ROS data");
            controller.setCamera(4); controller.setMainView({{"targetFrame", "map"}, {"pointSize", 6.}, {"followMode", 2}}); controller.addCloudView(2);
            const auto videos = videoEdits(controller.videoLayers()), scenes = controller.sceneLayers(), views = controller.cloudViews();
            const auto mainView = controller.mainView();
            controller.openBag(QUrl::fromLocalFile(bag)); waitForLoad(controller); waitForDisplays(controller);
            expect(videoEdits(controller.videoLayers()) == videos, "Importing the first Bag changed video edit state");
            expect(controller.videoLayers().front().toMap().value("segments").toList().front().toMap().value("last").toDouble() == controller.duration(),
                "Importing the first Bag did not update the derived visible timeline span");
            expect(controller.sceneLayers() == scenes, "Importing the first Bag changed scene layer state");
            expect(controller.cloudViews() == views, "Importing the first Bag changed additional view state");
            expect(controller.cameraIndex() == 4, "Importing the first Bag changed the main camera");
            expect(controller.mainView() == mainView, "Importing the first Bag changed the main view settings");
            const auto base = controller.recording(); const auto points = base->points.data();
            auto data = controller.independentDisplayRecordings();
            expect(data.size() == 1 && !data.front()->topicOverlays.empty() && data.front()->topicOverlays.front().samples.size() == 3,
                "Same-topic instances did not share one real decoded Topic cache");
            const auto firstCache = data.front(); data.clear();
            controller.seek(.5); controller.togglePlayback();
            const auto oldBackground = controller.sceneBackground();
            controller.setSceneBackground(QColor("#e5eaee"));
            const auto backgroundExport = directory.filePath("edited-background.rviz"); controller.exportRviz(QUrl::fromLocalFile(backgroundExport));
            expect(replay::readRviz(backgroundExport).backgroundColor == QColor("#e5eaee") && controller.playing() &&
                controller.recording() == base && controller.independentDisplayRecordings().front() == firstCache && !controller.loading(),
                "Global background edit did not export accurately or disturbed active data/playback");
            controller.undo(); expect(controller.sceneBackground() == oldBackground && controller.playing(), "Background undo lost the old scene color or playback state");
            controller.redo(); expect(controller.sceneBackground() == QColor("#e5eaee") && controller.playing(), "Background redo lost the new scene color or playback state");
            for (int cycle = 0; cycle < 10; ++cycle) controller.editIndependentDisplay(copy, {{"alpha", cycle % 2 ? .2 : .7}, {"pointSize", double(3 + cycle)}});
            expect(controller.playing() && controller.recording() == base && base->points.data() == points &&
                controller.independentDisplayRecordings().front() == firstCache && !controller.loading(), "Display style edit reloaded Bag/data or interrupted playback");
            controller.pause();
            const auto other = controller.addIndependentDisplay("rviz/PointCloud2", "/other_cloud"); waitForDisplays(controller);
            expect(controller.independentDisplayRecordings().size() == 2 && controller.recording() == base,
                "Adding another Topic rebuilt the main Recording or did not decode independently");
            controller.seek(0.); advanceEvents(150);
            expect(displayRow(controller, other).value("status") == "Waiting at Time", "Future Topic geometry was shown before its actual Header time");
            int timeStatusSignals = 0;
            const auto statusConnection = QObject::connect(&controller, &ReplayController::independentDisplaysChanged, &controller, [&] { ++timeStatusSignals; });
            for (int step = 0; step < 25; ++step) controller.seek(1. + step * .001);
            expect(timeStatusSignals == 0, "Rapid seeks rebuilt Display status synchronously");
            advanceEvents(150);
            expect(timeStatusSignals == 1 && displayRow(controller, other).value("status") == "Data Decoded",
                "Seek did not coalesce status notifications or did not refresh real current data availability");
            controller.seek(1.5); advanceEvents(150);
            expect(timeStatusSignals == 1, "Unchanged time status repeatedly rebuilt the Display tree");
            QObject::disconnect(statusConnection);
            std::weak_ptr<const replay::Recording> otherCache;
            for (const auto &cache : controller.independentDisplayRecordings()) if (cache != firstCache) otherCache = cache;
            controller.removeIndependentDisplays({other});
            expect(controller.independentDisplayRecordings().size() == 1 && otherCache.expired(), "Deleting the last Topic instance retained its CPU geometry cache");
            const auto replacement = directory.filePath("scene-replacement.bag"); expect(QFile::copy(bag, replacement), "Cannot copy Bag fixture");
            controller.openBag(QUrl::fromLocalFile(replacement)); waitForLoad(controller); waitForDisplays(controller);
            expect(videoEdits(controller.videoLayers()) == videos && controller.sceneLayers() == scenes && controller.cloudViews() == views && controller.cameraIndex() == 4 &&
                displayRow(controller, copy).value("pointSize").toDouble() == 12. && controller.independentDisplayRecordings().front() != firstCache,
                "Switching Bag discarded Display/camera/layout state or reused the old source cache");
            QString sourceId;
            for (const auto &value : controller.sources()) if (value.toMap().value("kind") == "bag" && value.toMap().value("referenceCount").toInt() > 0) sourceId = value.toMap().value("id").toString();
            expect(!sourceId.isEmpty() && controller.removeSources({sourceId}, true), "Cannot remove active Bag source");
            expect(controller.independentDisplayRecordings().empty() && !displayRow(controller, copy).isEmpty() && controller.cameraIndex() == 4 &&
                displayRow(controller, first).value("status") == "Waiting for Data", "Removing Bag erased Display configuration or retained ghost data");
            controller.undo(); waitForDisplays(controller);
            expect(controller.bagPath() == replacement && controller.independentDisplayRecordings().size() == 1, "Undo source removal did not restore actual data");
            const auto project = directory.filePath("independent-workflow.uavstudio"); controller.saveProjectAs(QUrl::fromLocalFile(project));
            ReplayController restored; restored.openProject(QUrl::fromLocalFile(project)); waitForLoad(restored); waitForDisplays(restored);
            expect(videoEdits(restored.videoLayers()) == videos && restored.sceneLayers() == scenes && restored.cloudViews() == views && restored.cameraIndex() == 4 &&
                displayRow(restored, copy).value("pointSize").toDouble() == 12. && displayRow(restored, copy).value("color").toString() == "#ee2244",
                "Project reopen lost independent Display IDs/styles or media layout");
            QStringList allIds; for (const auto &value : restored.independentDisplays()) allIds.append(value.toMap().value("id").toString());
            restored.removeIndependentDisplays(allIds);
            expect(restored.independentDisplayStates().empty() && restored.independentDisplayRecordings().empty(), "Delete-all restored ghost default Displays/data");
            restored.openBag(QUrl::fromLocalFile(bag)); waitForLoad(restored); waitForDisplays(restored);
            expect(restored.independentDisplayStates().empty(), "Loading another Bag recreated explicitly removed Display defaults");
        }
        if (argc == 1) {
            const auto staticBag = directory.filePath("static-tf-only.bag");
            QByteArray payload = record(2, field("conn", u32(1)) + field("time", stamp(1)), staticTransform());
            for (quint32 second : {1u, 2u, 3u}) payload += record(2, field("conn", u32(0)) + field("time", stamp(second)), cloud(second));
            write(staticBag, QByteArray("#ROSBAG V2.0\n") + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload)
                + record(7, field("conn", u32(0)) + field("topic", "/cloud"), field("type", "sensor_msgs/PointCloud2"))
                + record(7, field("conn", u32(1)) + field("topic", "/tf_static"), field("type", "tf2_msgs/TFMessage")));
            ReplayController controller; const auto tfId = controller.addIndependentDisplay("rviz/TF");
            controller.openBag(QUrl::fromLocalFile(staticBag)); waitForLoad(controller); waitForDisplays(controller);
            const auto row = displayRow(controller, tfId);
            expect(row.value("status") == "Data Decoded" && row.value("sourceTopics").toStringList() == QStringList{"/tf_static"},
                "TF Display rejected a valid static-only TF source or invented a dynamic source");
            const auto caches = controller.independentDisplayRecordings();
            expect(caches.size() == 1 && caches.front()->topicOverlays.size() == 1 &&
                caches.front()->topicOverlays.front().display.topic == "/tf_static" && caches.front()->topicOverlays.front().points.size() == 6,
                "Static-only TF geometry was not decoded from the real serialized message");
            controller.seek(2.); advanceEvents(150);
            expect(displayRow(controller, tfId).value("status") == "Data Decoded" && caches.front()->transforms &&
                caches.front()->transforms->lookup("map", "body", controller.recording()->stampAt(controller.position())),
                "Static TF ceased to be valid at a later experiment timestamp");
            const auto imported = directory.filePath("background-import.rviz");
            write(imported, "Visualization Manager:\n  Global Options: {Fixed Frame: map, Background Color: '36; 48; 60'}\n  Displays: []\n");
            controller.openRviz(QUrl::fromLocalFile(imported));
            expect(controller.sceneBackground() == QColor(36, 48, 60), "Imported Global Options background did not reach the live scene");
            controller.undo(); expect(controller.sceneBackground() != QColor(36, 48, 60), "RViz import background undo failed");
            controller.redo(); expect(controller.sceneBackground() == QColor(36, 48, 60), "RViz import background redo failed");
        }
        ReplayController baseline; baseline.openBag(QUrl::fromLocalFile(bag)); waitForLoad(baseline);
        expect(baseline.duration() > 0. && !baseline.cloudTopic().isEmpty(), "Initial bag load failed");
        const auto nativeSource = baseline.nativeReplaySource();
        expect(nativeSource.value("bag").toUrl().toLocalFile() == bag &&
            nativeSource.value("topics").toStringList().contains(baseline.cloudTopic()), "Native replay source did not identify actual bag/topics");
        baseline.seek(baseline.duration() * .375);
        expect(baseline.experimentStampNs() == QString::number(baseline.recording()->stampAt(baseline.position())),
            "Native replay timestamp changed integer Header precision");
        const auto initialRevision = nativeSource.value("revision");
        baseline.setCamera(3);
        expect(baseline.nativeReplaySource().value("revision") == initialRevision, "Camera edit invalidated the native source index");
        baseline.openBag(QUrl::fromLocalFile(bag)); waitForLoad(baseline);
        expect(baseline.nativeReplaySource().value("revision") != initialRevision, "Reload did not invalidate native source identity");
        if (argc == 1) {
            const auto originalRecord = baseline.recording();
            QString sourceId;
            for (const auto &value : baseline.sources()) {
                const auto source = value.toMap();
                if (source.value("kind") == "bag") sourceId = source.value("id").toString();
            }
            expect(!sourceId.isEmpty(), "Bag source is missing from the complete source catalog");
            const auto replacement = directory.filePath("replacement.bag");
            expect(QFile::copy(bag, replacement), "Cannot copy read-only source to replacement fixture");
            baseline.replaceSource(sourceId, QUrl::fromLocalFile(replacement));
            expect(baseline.loading() && baseline.recording() == originalRecord && baseline.bagPath() == bag,
                "Replacement changed project references before accurate data was ready");
            waitForLoad(baseline);
            const auto replacedRecord = baseline.recording();
            expect(replacedRecord != originalRecord && baseline.bagPath() == replacement,
                "Bag replacement left the old parsed scientific data in memory");
            baseline.undo();
            expect(baseline.recording() == originalRecord && baseline.bagPath() == bag,
                "Undo source replacement failed to restore parsed data and references together");
            baseline.redo();
            expect(baseline.recording() == replacedRecord && baseline.bagPath() == replacement,
                "Redo source replacement failed to restore parsed data");
            QString replacementId;
            for (const auto &value : baseline.sources()) {
                const auto source = value.toMap();
                if (source.value("kind") == "bag") replacementId = source.value("id").toString();
            }
            const auto invalid = directory.filePath("invalid.bag"); write(invalid, "invalid ROS Bag");
            baseline.replaceSource(replacementId, QUrl::fromLocalFile(invalid)); waitForLoad(baseline);
            expect(baseline.recording() == replacedRecord && baseline.bagPath() == replacement,
                "Failed replacement corrupted the previous valid source and data");
            expect(QFileInfo::exists(bag) && QFileInfo::exists(replacement), "Source replacement removed an original file");
            baseline.openBag(QUrl::fromLocalFile(bag)); waitForLoad(baseline);
        }
        const auto rviz = directory.filePath("hidden.rviz");
        write(rviz, "Visualization Manager:\n  Displays:\n    - Class: rviz/PointCloud2\n      Name: Hidden cloud\n      Enabled: false\n      Topic: '"
            + baseline.cloudTopic().toUtf8().replace("'", "''") + "'\n      Color: 60; 160; 240\n    - Class: rviz/TF\n      Enabled: false\n");
        QJsonArray cases;
        for (bool beforeBag : {true, false}) {
            ReplayController controller;
            if (beforeBag) controller.openRviz(QUrl::fromLocalFile(rviz));
            controller.openBag(QUrl::fromLocalFile(bag)); waitForLoad(controller);
            if (!beforeBag) { controller.openRviz(QUrl::fromLocalFile(rviz)); waitForLoad(controller); }
            waitForDisplays(controller);
            const auto data = controller.recording();
            expect(controller.topicDisplays().size() == 3 && controller.independentDisplayModelActive(), "Import order changed display configuration");
            const auto decoded = controller.independentDisplayRecordings();
            expect(!decoded.empty() && !decoded.front()->topicOverlays.empty() && !decoded.front()->topicOverlays.front().samples.empty(), "Hidden cloud was not decoded independently");
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
