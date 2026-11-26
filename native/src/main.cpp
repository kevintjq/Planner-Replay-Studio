#include "cloud_view.h"
#include "replay_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

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
    if (smoke)
        QTimer::singleShot(0, &app, [&app, &replay] { app.exit(replay.duration() > 0 ? 0 : 1); });
    else if (app.arguments().size() == 2)
        replay.openBag(QUrl::fromLocalFile(app.arguments().at(1)));
    return app.exec();
}
