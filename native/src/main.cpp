#include "cloud_view.h"
#include "replay_controller.h"

#include <QGuiApplication>
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
                const int fixtureIndex = app.arguments().indexOf("--smoke") + 1;
                if (fixtureIndex == app.arguments().size()) {
                    app.exit(0);
                    return;
                }
                replay.addVideo(QUrl::fromLocalFile(QFileInfo(app.arguments().at(fixtureIndex)).absoluteFilePath()));
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
        if (QFileInfo(path).suffix().compare(QStringLiteral("slamfpv"), Qt::CaseInsensitive) == 0)
            replay.openProject(url);
        else
            replay.openBag(url);
    }
    return app.exec();
}
