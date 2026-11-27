#include <array>
#include "cloud_view.h"
#include "movie_exporter.h"
#include "replay_controller.h"
#include "topic_inspector.h"
#include "trajectory_controller.h"

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <exception>

namespace {
void videoOutputs(QQuickItem *item, QList<QVideoSink *> &sinks)
{
    if (item->objectName() == QStringLiteral("flightClipOutput")) {
        if (auto *sink = item->property("videoSink").value<QVideoSink *>())
            sinks.append(sink);
    }
    for (auto *child : item->childItems())
        videoOutputs(child, sinks);
}

int smokePng(QGuiApplication &app)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    ReplayController replay;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("replay", &replay);
    replay.setTransparentCanvas(true);
    QQuickWindow window;
    window.setColor(Qt::transparent);
    window.resize(80, 60);
    QQmlComponent component(&engine);
    component.setData(R"qml(
import QtQuick
Rectangle {
    width: 80
    height: 60
    color: replay.captureMatte === 1 ? "black" : replay.captureMatte === 2 ? "white" : "transparent"
    Rectangle { x: 20; y: 15; width: 40; height: 30; color: "red" }
    Rectangle { x: 2; y: 2; width: 10; height: 10; color: "#800000ff" }
}
)qml", QUrl());
    std::unique_ptr<QObject> root(component.create());
    auto *canvas = qobject_cast<QQuickItem *>(root.get());
    if (!canvas)
        return 1;
    canvas->setParentItem(window.contentItem());
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const auto file = temporary.filePath(QStringLiteral("canvas.png"));
    QObject::connect(&replay, &ReplayController::stateChanged, &app, [&] {
        if (replay.capturing())
            return;
        const QImage image(file);
        const bool valid = image.size() == QSize(80, 60) && image.pixelColor(0, 0).alpha() == 0 &&
                           image.pixelColor(40, 30) == QColor(Qt::red) &&
                           std::abs(image.pixelColor(4, 4).alpha() - 128) <= 1 &&
                           image.pixelColor(4, 4).blue() == 255;
        if (!valid)
            qWarning() << replay.status() << "Unexpected PNG dimensions or pixels"
                       << image.size() << image.hasAlphaChannel() << image.pixelColor(0, 0)
                       << image.pixelColor(40, 30);
        app.exit(valid ? 0 : 1);
    });
    window.show();
    QTimer::singleShot(100, &app, [&] { replay.exportFrame(QUrl::fromLocalFile(file), canvas); });
    QTimer::singleShot(8000, &app, [&] { app.exit(1); });
    return app.exec();
}

int smokeMovie(QGuiApplication &app)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const auto input = temporary.filePath("audio-source.mp4");
    const auto output = temporary.filePath("export.mp4");
    const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    const auto ffprobe = QStandardPaths::findExecutable("ffprobe");
    if (ffmpeg.isEmpty() || ffprobe.isEmpty() ||
        QProcess::execute(ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i",
            "color=c=blue:size=160x90:rate=24:duration=1", "-f", "lavfi", "-i",
            "sine=frequency=440:sample_rate=48000:duration=1", "-c:v", "libx264", "-c:a", "aac", input}) != 0)
        return 1;

    ReplayController replay;
    QQmlEngine engine;
    auto *images = new MovieImages;
    engine.addImageProvider("movie", images);
    MovieExporter movie(replay, *images);
    engine.rootContext()->setContextProperty("movieExporter", &movie);
    engine.rootContext()->setContextProperty("replay", &replay);
    replay.addVideo(QUrl::fromLocalFile(input));
    replay.seek(.5);
    QQuickWindow window;
    window.resize(160, 90);
    QQmlComponent component(&engine);
    component.setData(R"qml(
import QtQuick
Rectangle {
    width: 160
    height: 90
    color: "black"
    Image {
        anchors.fill: parent
        source: movieExporter.frames[0]?.url ?? ""
        cache: false
        asynchronous: false
    }
    Rectangle { width: 12; height: 12; x: replay.position * 30; color: "red" }
}
)qml", QUrl());
    std::unique_ptr<QObject> root(component.create());
    auto *canvas = qobject_cast<QQuickItem *>(root.get());
    if (!canvas)
        return 1;
    canvas->setParentItem(window.contentItem());

    QObject::connect(&movie, &MovieExporter::changed, &app, [&] {
        if (movie.exporting())
            return;
        QProcess probe;
        probe.start(ffprobe, {"-v", "error", "-show_streams", "-of", "json", output});
        if (!probe.waitForFinished(4000)) {
            app.exit(1);
            return;
        }
        const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value("streams").toArray();
        bool audio = false, video = false;
        for (const auto &entry : streams) {
            const auto stream = entry.toObject();
            if (stream.value("codec_type").toString() == "audio")
                audio = stream.value("codec_name").toString() == "aac";
            if (stream.value("codec_type").toString() == "video") {
                video = stream.value("width").toInt() == 1920 && stream.value("height").toInt() == 1080 &&
                        stream.value("avg_frame_rate").toString() == "60/1" &&
                        stream.value("nb_frames").toString() == "12";
            }
        }
        bool blue = false;
        if (video) {
            QProcess pixels;
            pixels.start(ffmpeg, {"-v", "error", "-i", output, "-vf", "crop=2:2:960:540", "-frames:v", "1",
                                  "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"});
            if (pixels.waitForFinished(4000)) {
                const auto rgb = pixels.readAllStandardOutput();
                blue = rgb.size() == 12 && uchar(rgb[0]) < 60 && uchar(rgb[1]) < 60 && uchar(rgb[2]) > 180;
            }
        }
        if (!audio || !video || !blue || replay.position() != .5)
            qWarning() << movie.message() << "Expected 12 1080p/60 fps frames, decoded source pixels, AAC, and restored playhead";
        app.exit(audio && video && blue && replay.position() == .5 ? 0 : 1);
    });
    window.show();
    QTimer::singleShot(100, &app, [&] { movie.start(QUrl::fromLocalFile(output), canvas, .1, .2, .5, 0); });
    QTimer::singleShot(25000, &app, [&] { app.exit(1); });
    return app.exec();
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
    QQuickWindow::setDefaultAlphaBuffer(true);
    if (app.arguments().contains("--smoke-png"))
        return smokePng(app);
    if (app.arguments().contains("--smoke-mp4"))
        return smokeMovie(app);
    const auto args = app.arguments();
    const bool batch = args.size() > 1 && args[1] == QStringLiteral("--batch");
    const bool software = batch && qEnvironmentVariable("QT_QUICK_BACKEND") == QStringLiteral("software");
    QQuickWindow::setGraphicsApi(software ? QSGRendererInterface::Software : QSGRendererInterface::OpenGL);
    qmlRegisterType<CloudView>("PlannerReplay", 1, 0, "CloudView");
    qmlRegisterUncreatableType<ReplayController>("PlannerReplay", 1, 0, "ReplayController",
                                                 "Provided by the application");

    ReplayController replay;
    auto *trajectoryImages = new PreviewImages;
    auto *movieImages = new MovieImages;
    auto *rosImages = new PreviewImages;
    TopicInspector inspector(replay, *rosImages);
    TrajectoryController trajectory(*trajectoryImages);
    MovieExporter movie(replay, *movieImages);
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("trajectory"), trajectoryImages);
    engine.addImageProvider(QStringLiteral("movie"), movieImages);
    engine.addImageProvider(QStringLiteral("ros-inspector"), rosImages);
    engine.rootContext()->setContextProperty("inspector", &inspector);
    engine.rootContext()->setContextProperty("trajectory", &trajectory);
    engine.rootContext()->setContextProperty("movie", &movie);
    const bool smoke = args.contains("--smoke");
    engine.rootContext()->setContextProperty("replay", &replay);
    engine.rootContext()->setContextProperty("smokeTest", smoke);
    engine.load(QUrl(batch ? QStringLiteral("qrc:/qml/BatchExport.qml") : QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (batch) {
        if (args.size() != 6 && args.size() != 10)
            return 2;
        bool beginOk = false, secondsOk = false;
        const double begin = args[4].toDouble(&beginOk), seconds = args[5].toDouble(&secondsOk);
        if (!beginOk || !secondsOk || !std::isfinite(begin) || !std::isfinite(seconds) || begin < 0. || seconds < 0.)
            return 2;
        std::array<int, 4> videoOptions{1920, 1080, 60, 18};
        for (int i = 0; args.size() == 10 && i < int(videoOptions.size()); ++i) {
            bool ok = false;
            videoOptions[i] = args[6 + i].toInt(&ok);
            if (!ok)
                return 2;
        }
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        auto *canvas = window ? window->findChild<QQuickItem *>(QStringLiteral("batchCanvas")) : nullptr;
        if (!canvas)
            return 1;
        const bool demo = args[2] == QStringLiteral(":demo:");
        const QFileInfo source(demo ? QString() : args[2]);
        const auto destination = QUrl::fromLocalFile(QFileInfo(args[3]).absoluteFilePath());
        const bool still = QFileInfo(destination.toLocalFile()).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0;
        const bool project = source.suffix().compare(QStringLiteral("slamfpv"), Qt::CaseInsensitive) == 0;
        const auto completed = [&app](bool success, const QString &message) {
            QTextStream stream(success ? stdout : stderr);
            stream << message << Qt::endl;
            app.exit(success ? 0 : 1);
        };
        QObject::connect(&movie, &MovieExporter::finished, &app, completed);
        app.setQuitOnLastWindowClosed(false);
        const auto closed = QObject::connect(window, &QWindow::visibleChanged, &app, [completed](bool visible) {
            if (!visible)
                completed(false, QStringLiteral("The export window was closed."));
        });
        QTimer renderTimeout;
        renderTimeout.setSingleShot(true);
        renderTimeout.setInterval(30'000);
        QObject::connect(&movie, &MovieExporter::changed, &renderTimeout, [&] {
            if (movie.exporting())
                renderTimeout.start();
            else
                renderTimeout.stop();
        });
        QObject::connect(&renderTimeout, &QTimer::timeout, &app, [completed] {
            completed(false, QStringLiteral("The export window did not produce another frame within 30 seconds."));
        });
        const auto exportLoaded = [&] {
            if ((!demo && !source.isFile()) || (project && replay.projectPath().isEmpty()) ||
                (!demo && !project && replay.bagPath().isEmpty())) {
                completed(false, replay.status().isEmpty() ? QStringLiteral("Cannot open the export source.") : replay.status());
                return;
            }
            if (still)
                movie.startStill(destination, canvas, begin);
            else {
                const auto prefs = replay.moviePreferences();
                const auto end = seconds > 0. ? std::min(replay.duration(), begin + seconds) : replay.duration();
                movie.start(destination, canvas, begin, end, prefs.value("speed").toDouble(), prefs.value("audioIndex").toInt(),
                            videoOptions[0], videoOptions[1], videoOptions[2], videoOptions[3]);
            }
        };
        bool exportScheduled = false;
        QMetaObject::Connection loadProgress;
        const auto schedule = [&] {
            if (!replay.loading() && !exportScheduled) {
                exportScheduled = true;
                QTimer::singleShot(0, &app, exportLoaded);
            }
        };
        QTimer::singleShot(0, &app, [&] {
            const auto url = QUrl::fromLocalFile(source.absoluteFilePath());
            if (!demo && project)
                replay.openProject(url);
            else if (!demo)
                replay.openBag(url);
            if (replay.loading())
                loadProgress = QObject::connect(&replay, &ReplayController::stateChanged, &app, schedule);
            schedule();
        });
        const int result = app.exec();
        QObject::disconnect(closed);
        QObject::disconnect(loadProgress);
        return result;
    }
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
                replay.setCamera(4);
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
                replay.addCloudView(2);
                if (replay.cloudViews().size() != 1) {
                    qWarning("Could not add a cloud view");
                    app.exit(1);
                    return;
                }
                const auto cloudId = replay.cloudViews().first().toMap().value("id").toString();
                replay.setCloudView(cloudId, {{"x", .25}, {"width", .5}});
                replay.addLayerMotion(QStringLiteral("view"), cloudId, 0., .4, 1., .5, .5, 1.);
                const auto sceneUrl = QUrl::fromLocalFile(scratch.filePath(QStringLiteral("scene.slamfpv")));
                replay.saveProjectAs(sceneUrl);
                replay.openProject(sceneUrl);
                const auto view = replay.cloudViews().value(0).toMap();
                const auto base = QRectF(10., 10., 50., 30.);
                const auto atMiddle = replay.animatedLayer(QStringLiteral("view"), cloudId, 1.7, base, QSizeF(100., 100.));
                if (view.value("id").toString() != cloudId || view.value("camera").toInt() != 2 ||
                    view.value("x").toDouble() != .25 || view.value("width").toDouble() != .5 ||
                    replay.layerMotions().size() != 1 || std::abs(atMiddle.width() - 75.) > .01 ||
                    replay.animatedLayer(QStringLiteral("view"), cloudId, 2., base, QSizeF(100., 100.)) != base ||
                    replay.sceneLayers().size() != 2 || replay.cameraIndex() != 4 ||
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
                if (replay::readTrajectoryFrame(fixture.toLocalFile(), .25).isNull()) {
                    qWarning("OpenCV could not decode the installed application's video fixture");
                    app.exit(1);
                    return;
                }
                replay.addVideo(fixture);
                replay.addVideo(fixture);
                replay.setVideoAnchor(0, 0, QStringLiteral("0.2"));
                replay.seek(1.);
                replay.setVideoAnchor(0, 1, QStringLiteral("1.4"));
                replay.trimVideoSegment(0, 0, .1, 2.);
                replay.splitVideo(0, 1.4);
                replay.deleteVideoSegment(0, 1.6);
                const auto preview = [&replay](int layer, double seconds) {
                    replay.seek(seconds);
                    return replay.videoSeconds(layer);
                };
                if (preview(0, .05) >= 0. || preview(0, .5) < 0. || preview(0, 1.6) >= 0. ||
                    preview(1, 1.6) < 0. || !replay.canUndo()) {
                    qWarning("Video trim, split or deletion was not applied to the selected track");
                    app.exit(1);
                    return;
                }
                replay.undo();
                if (preview(0, 1.6) < 0. || !replay.canRedo()) {
                    qWarning("Video deletion could not be undone");
                    app.exit(1);
                    return;
                }
                replay.redo();

                replay.seek(.125);
                replay.setLayoutCue(QStringLiteral("backdrop"), 0, .5);
                const auto backdrop = replay.activeLayout().value("backdropId").toString();
                replay.seek(.25);
                replay.setLayoutCue(QStringLiteral("inset"), 0, .5);
                replay.seek(.5);
                if (backdrop.isEmpty() || replay.activeLayout().value("progress").toDouble() != .5) {
                    qWarning("The backdrop-to-inset layout transition was not evaluated correctly");
                    app.exit(1);
                    return;
                }
                replay.seek(1.);
                replay.setLayoutCue(QStringLiteral("cloud"), 0, 0.);
                replay.removeLayoutCue(1.);
                const auto editedUrl = QUrl::fromLocalFile(scratch.filePath(QStringLiteral("edited.slamfpv")));
                replay.saveProjectAs(editedUrl);
                replay.openProject(editedUrl);
                if (preview(0, 1.6) >= 0. || preview(0, .5) < 0. || replay.canUndo() ||
                    replay.layoutCues().size() != 2 || replay.activeLayout().value("progress").toDouble() != .5) {
                    qWarning("The saved video edits did not survive reopening");
                    app.exit(1);
                    return;
                }
                auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
                QList<QVideoSink *> sinks;
                if (window)
                    videoOutputs(window->contentItem(), sinks);
                if (sinks.size() != 2) {
                    qWarning("Both video preview outputs were not created");
                    app.exit(1);
                    return;
                }
                auto decoded = std::make_shared<QSet<QVideoSink *>>();
                for (auto *sink : sinks) {
                    QObject::connect(sink, &QVideoSink::videoFrameChanged, &app, [&app, decoded, sink](const QVideoFrame &frame) {
                        if (frame.isValid()) {
                            decoded->insert(sink);
                            if (decoded->size() == 2)
                                app.exit(0);
                        }
                    });
                }
                QTimer::singleShot(12000, &app, [&app] {
                    qWarning("Video preview did not decode a frame");
                    app.exit(1);
                });
                replay.seek(.2);
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
