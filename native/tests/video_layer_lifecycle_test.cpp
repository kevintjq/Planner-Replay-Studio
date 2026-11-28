#include "appearance_manager.h"
#include "clip_preview.h"
#include "replay_controller.h"
#include "video_reader.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void wait(const std::function<bool()> &done)
{
    if (done()) return;
    QEventLoop loop; QTimer poll, deadline; poll.setInterval(1); deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (done()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(); deadline.start(8000); loop.exec();
    check(done(), "Video layer lifecycle operation did not settle");
}
void advanceEvents(int milliseconds)
{
    QEventLoop loop; QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit); loop.exec();
}
ClipPreview *output(QObject &root, const QString &id)
{
    // Repeater owns its delegates through the visual tree, whose QObject
    // ownership can differ from parentItem(). Follow the actual item hierarchy.
    const auto find = [](QQuickItem *parent, const QString &name, const auto &self) -> QQuickItem * {
        if (!parent) return nullptr;
        if (parent->objectName() == name) return parent;
        for (auto *child : parent->childItems()) if (auto *match = self(child, name, self)) return match;
        return nullptr;
    };
    auto *layer = find(qobject_cast<QQuickItem *>(&root), "clip-" + id, find);
    return qobject_cast<ClipPreview *>(find(layer, "flightClipOutput", find));
}
QString idAt(const ReplayController &controller, int row)
{
    return controller.videoLayers().at(row).toMap().value("id").toString();
}
void fixture(const QString &path)
{
    QProcess process;
    process.start(QStandardPaths::findExecutable("ffmpeg"), {"-v", "error", "-y", "-f", "lavfi", "-i",
        "testsrc2=size=128x96:rate=24:duration=4", "-c:v", "libx264", "-crf", "0", path});
    check(process.waitForStarted() && process.waitForFinished(10000) && process.exitCode() == 0,
        "Cannot generate decodable lifecycle video fixture");
}
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    try {
        QTemporaryDir directory; check(directory.isValid(), "Cannot create lifecycle directory");
        // Model move/data changes must preserve identity, including during Undo insertion.
        VideoLayerListModel model; int resets = 0;
        QObject::connect(&model, &QAbstractItemModel::modelReset, &app, [&] { ++resets; });
        const QVariantMap first{{"id", "first"}}, second{{"id", "second"}}, third{{"id", "third"}};
        model.setLayers({first, second}); const QPersistentModelIndex retainedIndex(model.index(1));
        model.setLayers({second, third});
        check(retainedIndex.isValid() && retainedIndex.row() == 0 && resets == 0,
            "Adding/removing another video reset the retained model identity");
        auto changed = second; changed["offset"] = .5;
        model.setLayers({first, third, changed});
        check(retainedIndex.isValid() && retainedIndex.row() == 2 &&
            retainedIndex.data(VideoLayerListModel::VideoLayer).toMap() == changed && resets == 0,
            "Reordering/updating a video reset its model identity");

        const auto source = directory.filePath("lifecycle.mp4"); fixture(source);
        ReplayController controller;
        controller.addVideo(QUrl::fromLocalFile(source)); controller.addVideo(QUrl::fromLocalFile(source));
        const auto removedId = idAt(controller, 0), retainedId = idAt(controller, 1);
        controller.setVideoTrim(1, "0", "", "0.5"); controller.seek(1.5);
        AppearanceManager appearance(nullptr, directory.filePath("appearance.ini"));
        qmlRegisterType<ClipPreview>("UAVStudio", 1, 0, "ClipPreview");
        QQmlEngine engine; engine.rootContext()->setContextProperty("testReplay", &controller);
        engine.rootContext()->setContextProperty("appearance", &appearance);
        QQmlComponent component(&engine);
        const auto qmlDirectory = QUrl::fromLocalFile(QStringLiteral(NATIVE_QML_DIRECTORY) + '/').toString();
        component.setData((QStringLiteral(R"qml(
            import QtQuick
            import "%1" as Studio
            Item {
                id: host
                width: 640; height: 360
                property bool interactive: false
                property var layerItems: ({})
                function registerLayer(id, item) { layerItems[id] = item }
                function unregisterLayer(id, item) { if (layerItems[id] === item) delete layerItems[id] }
                Repeater {
                    model: testReplay.videoLayerModel
                    delegate: Studio.FlightClip {
                        required property int index
                        required property var videoLayer
                        objectName: "clip-" + videoLayer.id
                        controller: testReplay
                        footage: videoLayer
                        clipIndex: index
                        layout: testReplay.activeLayout
                        videoOnly: false; cloudOnly: false
                        sceneCanvas: host
                    }
                }
            }
        )qml").arg(qmlDirectory)).toUtf8(), QUrl("test:/video-layer-lifecycle.qml"));
        wait([&] { return component.status() != QQmlComponent::Loading; });
        std::unique_ptr<QObject> root(component.create());
        if (!root) std::cerr << component.errorString().toStdString();
        check(bool(root), "Cannot instantiate production FlightClip QML");
        wait([&] { return output(*root, retainedId) && output(*root, removedId); });
        QPointer<ClipPreview> retained = output(*root, retainedId), removed = output(*root, removedId);
        check(retained && removed, "Production video delegate has no decoder item");
        wait([&] { return retained->ready() && !retained->busy() && removed->ready() && !removed->busy(); });
        replay::VideoReader reference(source);
        check(reference.select(controller.videoSeconds(1)) && retained->sourcePts() == QString::number(reference.pts()),
            "Initial second-video preview does not match its exact timing/PTS");
        int blankFrames = 0, sourceChanges = 0;
        QObject::connect(retained, &ClipPreview::frameChanged, &app, [&] { if (retained && !retained->ready()) ++blankFrames; });
        QObject::connect(retained, &ClipPreview::sourceChanged, &app, [&] { ++sourceChanges; });
        auto *liveModel = controller.videoLayerModel(); int liveResets = 0;
        QObject::connect(liveModel, &QAbstractItemModel::modelReset, &app, [&] { ++liveResets; });
        const QPersistentModelIndex liveIndex(liveModel->index(1, 0));

        controller.togglePlayback(); const double beforeDelete = controller.position();
        controller.removeLayers({removedId});
        advanceEvents(140);
        if (!(controller.playing() && controller.position() > beforeDelete && removed.isNull() && retained &&
              output(*root, retainedId) == retained && liveIndex.isValid() && liveIndex.row() == 0 &&
              blankFrames == 0 && sourceChanges == 0 && liveResets == 0))
            std::cerr << "After deletion: playing=" << controller.playing() << " time=" << beforeDelete << "->" << controller.position()
                      << " removedDestroyed=" << removed.isNull() << " retained=" << bool(retained)
                      << " same=" << (output(*root, retainedId) == retained) << " indexValid=" << liveIndex.isValid()
                      << " row=" << liveIndex.row() << " blank=" << blankFrames << " sourceChanges=" << sourceChanges << " resets=" << liveResets << '\n';
        check(controller.playing() && controller.position() > beforeDelete && removed.isNull() && retained &&
            output(*root, retainedId) == retained && liveIndex.isValid() && liveIndex.row() == 0 &&
            blankFrames == 0 && sourceChanges == 0 && liveResets == 0,
            "Deleting another video recreated/blanked the retained decoder or interrupted playback");
        controller.pause();
        wait([&] { return retained->ready() && !retained->busy() && reference.select(controller.videoSeconds(0)) &&
            retained->sourcePts() == QString::number(reference.pts()); });
        check(reference.select(controller.videoSeconds(0)) && retained->sourcePts() == QString::number(reference.pts()),
            "Retained video uses the deleted row's timing after its index changed");

        controller.addVideo(QUrl::fromLocalFile(source));
        advanceEvents(80);
        check(output(*root, retainedId) == retained && liveIndex.isValid() && liveIndex.row() == 0 &&
            blankFrames == 0 && sourceChanges == 0 && liveResets == 0,
            "Adding another video recreated or blanked an existing decoder");
        auto *canvas = controller.canvas(); canvas->beginEdit();
        canvas->edit(retainedId, {{"x", .2}, {"y", .3}, {"width", .4}, {"height", .35}, {"rotation", 17.}, {"opacity", .7}});
        canvas->endEdit(); advanceEvents(30);
        check(output(*root, retainedId) == retained && blankFrames == 0 && sourceChanges == 0,
            "Canvas transform recreated or blanked the retained video decoder");

        controller.setVideoTrim(0, "0", "", "0.25");
        wait([&] { return !retained->busy() && reference.select(controller.videoSeconds(0)) &&
            retained->sourcePts() == QString::number(reference.pts()); });
        controller.undo();
        wait([&] { return !retained->busy() && reference.select(controller.videoSeconds(0)) &&
            retained->sourcePts() == QString::number(reference.pts()); });
        controller.undo(); controller.undo(); // Transform, then inserted clip.
        advanceEvents(30);
        controller.undo(); // Restore deleted first clip before the retained one.
        wait([&] { return liveModel->rowCount() == 2 && retained->ready() && !retained->busy() &&
            reference.select(controller.videoSeconds(1)) && retained->sourcePts() == QString::number(reference.pts()); });
        check(output(*root, retainedId) == retained && liveIndex.isValid() && liveIndex.row() == 1 &&
            reference.select(controller.videoSeconds(1)) && retained->sourcePts() == QString::number(reference.pts()) &&
            blankFrames == 0 && sourceChanges == 0 && liveResets == 0,
            "Undo insertion recreated a retained decoder or failed to synchronize its shifted index");
        std::cout << "Production FlightClip QML: exact generated-video PTS, stable decoder QObject/frames across add/remove, "
                     "active playback continuity, canvas transform, timing edit and Undo insertion verified.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
