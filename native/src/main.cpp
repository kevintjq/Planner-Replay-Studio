#include "cloud_view.h"
#include "replay_controller.h"

#include <QGuiApplication>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <exception>

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
        QTimer::singleShot(0, &app, [&app, &replay] {
            try {
                QTemporaryDir scratch;
                if (!scratch.isValid()) {
                    app.exit(1);
                    return;
                }
                const auto project = replay::ProjectDocument::create(QStringLiteral("demo"))
                                         .saveAs(scratch.filePath(QStringLiteral("smoke.slamfpv")));
                replay.openProject(QUrl::fromLocalFile(project.path()));
                app.exit(replay.duration() > 0. && replay.projectPath() == project.path() ? 0 : 1);
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
