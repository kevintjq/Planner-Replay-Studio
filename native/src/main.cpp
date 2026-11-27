#include "cloud_view.h"
#include "replay_controller.h"

#include <QGuiApplication>
#include <QImage>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <exception>

namespace {
QQuickItem *videoOutput(QQuickItem *item)
{
    if (item->objectName() == QStringLiteral("flightClipOutput"))
        return item;
    for (auto *child : item->childItems()) {
        if (auto *match = videoOutput(child))
            return match;
    }
    return nullptr;
}
}

int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);
    app.setApplicationName("Planner Replay Native");
    app.setOrganizationName("Planner Replay");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    qmlRegisterType<CloudView>("PlannerReplay", 1, 0, "CloudView");
    qmlRegisterUncreatableType<ReplayController>("PlannerReplay", 1, 0, "ReplayController",
                                                 "Provided by the application");

    ReplayController replay;
    QQmlApplicationEngine engine;
    const bool smoke = app.arguments().contains("--smoke");
    engine.rootContext()->setContextProperty("replay", &replay);
    engine.rootContext()->setContextProperty("smokeTest", smoke);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (smoke) {
        QTimer::singleShot(0, &app, [&app, &engine, &replay] {
            try {
                QTemporaryDir scratch;
                if (!scratch.isValid()) {
                    app.exit(1);
                    return;
                }
                const auto project = replay::ProjectDocument::create(QStringLiteral("demo"))
                                         .saveAs(scratch.filePath(QStringLiteral("smoke.slamfpv")));
                replay.openProject(QUrl::fromLocalFile(project.path()));
                if (replay.duration() <= 0. || replay.projectPath() != project.path()) {
                    app.exit(1);
                    return;
                }
                const auto logoUrl = QUrl::fromLocalFile(scratch.filePath(QStringLiteral("image.png")));
                QImage logo(64, 32, QImage::Format_ARGB32);
                logo.fill(Qt::red);
                if (!logo.save(logoUrl.toLocalFile())) {
                    qWarning("Could not create the image fixture");
                    app.exit(1);
                    return;
                }
                replay.addSceneImage(logoUrl);
                replay.addSceneText(QStringLiteral("原生合成"));
                if (replay.sceneLayers().size() != 2) {
                    qWarning("Could not add the scene layers");
                    app.exit(1);
                    return;
                }
                const auto textId = replay.sceneLayers().at(1).toMap().value("id").toString();
                replay.editSceneLayer(textId, {{QStringLiteral("position"), QVariantList{.3, .4}},
                                               {QStringLiteral("start"), .5}, {QStringLiteral("end"), 2.}});
                replay.seek(.75);
                replay.addMarker();
                replay.renameMarker(.75, QStringLiteral("起飞"));
                replay.seek(1.5);
                replay.addMarker();
                replay.removeMarker(1.5);
                const auto sceneUrl = QUrl::fromLocalFile(scratch.filePath(QStringLiteral("scene.slamfpv")));
                replay.saveProjectAs(sceneUrl);
                replay.openProject(sceneUrl);
                if (replay.sceneLayers().size() != 2 ||
                    replay.sceneLayers().at(0).toMap().value("url").toUrl() != logoUrl ||
                    replay.sceneLayers().at(1).toMap().value("horizontal").toDouble() != .3 ||
                    replay.markers().size() != 1 ||
                    replay.markers().at(0).toMap().value("label").toString() != QStringLiteral("起飞")) {
                    qWarning("Scene layer settings did not survive reopening");
                    app.exit(1);
                    return;
                }

                const int fixtureIndex = app.arguments().indexOf("--smoke") + 1;
                if (fixtureIndex == app.arguments().size()) {
                    app.exit(0);
                    return;
                }
                const auto fixture = QUrl::fromLocalFile(QFileInfo(app.arguments().at(fixtureIndex)).absoluteFilePath());
                replay.addVideo(fixture);
                replay.addVideo(fixture);
                replay.setVideoAnchor(0, 0, QStringLiteral("0.2"));
                replay.seek(1.);
                replay.setVideoAnchor(0, 1, QStringLiteral("1.4"));
                replay.trimVideoSegment(0, 0, .1, 2.);
                replay.splitVideo(0, .8);
                replay.deleteVideoSegment(0, 1.2);
                const auto preview = [&replay](int layer, double seconds) {
                    replay.seek(seconds);
                    return replay.videoSeconds(layer);
                };
                if (preview(0, .05) >= 0. || preview(0, .5) < 0. || preview(0, 1.2) >= 0. ||
                    preview(1, 1.2) < 0. || !replay.canUndo()) {
                    qWarning("Video trim, split or deletion was not applied to the selected track");
                    app.exit(1);
                    return;
                }
                replay.undo();
                if (preview(0, 1.2) < 0. || !replay.canRedo()) {
                    qWarning("Video deletion could not be undone");
                    app.exit(1);
                    return;
                }
                replay.redo();
                const auto editedUrl = QUrl::fromLocalFile(scratch.filePath(QStringLiteral("edited.slamfpv")));
                replay.saveProjectAs(editedUrl);
                replay.openProject(editedUrl);
                if (preview(0, 1.2) >= 0. || preview(0, .5) < 0. || replay.canUndo()) {
                    qWarning("The saved video edits did not survive reopening");
                    app.exit(1);
                    return;
                }
                auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
                auto *output = window ? videoOutput(window->contentItem()) : nullptr;
                auto *sink = output ? output->property("videoSink").value<QVideoSink *>() : nullptr;
                if (!sink) {
                    qWarning("Video preview output was not created");
                    app.exit(1);
                    return;
                }
                QObject::connect(sink, &QVideoSink::videoFrameChanged, &app, [&app](const QVideoFrame &frame) {
                    if (frame.isValid())
                        app.exit(0);
                });
                QTimer::singleShot(12000, &app, [&app] {
                    qWarning("Video preview did not decode a frame");
                    app.exit(1);
                });
                replay.togglePlayback();
            } catch (const std::exception &) {
                app.exit(1);
            }
        });
    } else if (app.arguments().size() == 2) {
        const auto path = QFileInfo(app.arguments().at(1)).absoluteFilePath();
        const auto url = QUrl::fromLocalFile(path);
        const auto extension = QFileInfo(path).suffix().toLower();
        if (extension == QStringLiteral("slamfpv"))
            replay.openProject(url);
        else if (extension == QStringLiteral("rviz") || extension == QStringLiteral("yaml") || extension == QStringLiteral("yml"))
            replay.openRviz(url);
        else
            replay.openBag(url);
    }
    return app.exec();
}
