#include <array>
#include "cloud_view.h"
#include "movie_exporter.h"
#include "replay_controller.h"
#include "topic_inspector.h"
#include "trajectory_controller.h"
#include "appearance_manager.h"
#include "app_info.h"
#include "update_manager.h"
#include "appearance_acceptance.h"
#include "display_acceptance.h"
#include "movie_frame_acceptance.h"
#include "frame_acceptance.h"
#include "camera_acceptance.h"
#include "native_rviz_client.h"
#include "native_replay_acceptance.h"
#include "native_scene_acceptance.h"
#include "frame_batch_exporter.h"
#include "clip_preview.h"
#include "speed_meter_model.h"
#include "speed_gauge_item.h"
#include "trajectory_layer_item.h"
#include "canvas_scientific_item.h"
#include <QQuickStyle>
#include <QIcon>

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QSettings>
#include <QScopeGuard>
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
void videoOutputs(QQuickItem *item, QList<ClipPreview *> &sinks)
{
    if (item->objectName() == QStringLiteral("flightClipOutput")) {
        if (auto *sink = qobject_cast<ClipPreview *>(item))
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
    const auto toneStatistics = [](qsizetype count, const auto &sample) {
        double mean = 0.;
        for (qsizetype i = 0; i < count; ++i) mean += sample(i);
        mean /= double(count);
        double energy = 0.;
        for (qsizetype i = 0; i < count; ++i) {
            const auto value = sample(i) - mean;
            energy += value * value;
        }
        const double rms = std::sqrt(energy / double(count));
        // A decoded AAC tail can ring around zero after the audible signal ends.
        // Count a period only after excursions on both sides of a Schmitt band;
        // retain the interpolated zero crossing for the frequency measurement.
        const double threshold = rms * .25;
        qsizetype crossings = 0;
        double first = 0., last = 0., pending = 0.;
        bool armed = false, hasPending = false;
        for (qsizetype i = 1; i < count; ++i) {
            const auto value = sample(i) - mean;
            if (value < -threshold) {
                armed = true;
                hasPending = false;
            }
            if (!armed) continue;
            const auto previous = sample(i - 1) - mean;
            if (previous <= 0. && value > 0.) {
                pending = double(i - 1) - previous / (value - previous);
                hasPending = true;
            }
            if (hasPending && value > threshold) {
                if (!crossings) first = pending;
                last = pending;
                ++crossings;
                armed = false;
                hasPending = false;
            }
        }
        const double frequency = crossings > 1 && last > first
            ? double(crossings - 1) * 48000. / (last - first) : 0.;
        return std::array<double, 2>{rms, frequency};
    };
    const auto audible440 = [](const auto &statistics) {
        return statistics[0] > .01 && statistics[0] < .2 && std::abs(statistics[1] - 440.) < 15.;
    };
    // Exercise the same detector with a short, DC-offset signal plus low-level
    // high-frequency tail noise. Wrong pitch and silence must still fail.
    for (const double frequency : {0., 220., 440., 490., 880.}) {
        const auto statistics = toneStatistics(8320, [frequency](qsizetype i) {
            const double time = double(i + 960) / 48000.;
            const double tone = frequency > 0. && time < .185
                ? .125 * std::sin(2. * 3.14159265358979323846 * frequency * time) : 0.;
            const double tail = frequency > 0. && time >= .185 && time < .189
                ? (i % 2 ? .001 : -.001) : 0.;
            return tone + tail + .003;
        });
        if (audible440(statistics) != (frequency == 440.)) {
            qWarning() << "Audio pitch detector control failed:" << frequency << statistics[0] << statistics[1];
            return 1;
        }
        qInfo() << "Audio pitch detector control:" << frequency << statistics[0] << statistics[1];
    }
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
            "sine=frequency=440:sample_rate=48000:duration=1", "-vf", "setpts=PTS+5/TB", "-af", "asetpts=PTS+5/TB",
            "-c:v", "libx264", "-c:a", "aac", input}) != 0)
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

    QList<double> capturedTimes;
    bool capturedSizes = true;
    const QVariantMap originalOptions{{"retained", "original"}};
    canvas->setProperty("captureOptions", originalOptions);
    QObject::connect(&movie, &MovieExporter::frameCaptured, &app, [&](const QImage &image, double time) {
        capturedTimes.append(time); capturedSizes &= image.size() == QSize(1920, 1080);
    });
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
        bool audible = false;
        double audioRms = 0., audioFrequency = 0.;
        if (audio) {
            QProcess pcm;
            pcm.start(ffmpeg, {"-v", "error", "-i", output, "-map", "0:a:0", "-ac", "1", "-ar", "48000",
                              "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"});
            if (pcm.waitForFinished(4000) && pcm.exitStatus() == QProcess::NormalExit && pcm.exitCode() == 0) {
                const auto bytes = pcm.readAllStandardOutput();
                const auto sample = [&](qsizetype i) {
                    const int word = int(uchar(bytes[2*i])) | (int(uchar(bytes[2*i + 1])) << 8);
                    return double(word >= 32768 ? word - 65536 : word) / 32768.;
                };
                const auto samples = bytes.size() / 2;
                // AAC priming/padding and atempo can alter the edges. Inspect the interior signal.
                constexpr qsizetype edge = 960; // 20 ms at 48 kHz.
                if (!(bytes.size() % 2) && samples >= 7200 && samples <= 12480) {
                    const auto statistics = toneStatistics(samples - 2*edge, [&](qsizetype i) { return sample(i + edge); });
                    audioRms = statistics[0];
                    audioFrequency = statistics[1];
                    audible = audible440(statistics);
                }
            }
            qInfo() << "Decoded exported audio RMS/frequency:" << audioRms << audioFrequency;
        }
        const bool captureValid = capturedSizes && capturedTimes.size() == 12 && std::abs(capturedTimes.first() - .1) < 1.e-9 &&
            std::abs(capturedTimes.last() - (.1 + 11. / 120.)) < 1.e-9;
        const bool restored = replay.position() == .5 && replay.playing() && canvas->property("captureOptions").toMap() == originalOptions;
        if (!audio || !audible || !video || !blue || !captureValid || !restored)
            qWarning() << movie.message() << "Expected 12 full-resolution timed frames, decoded pixels, audible 440 Hz AAC, restored playhead/playback and capture settings";
        app.exit(audio && audible && video && blue && captureValid && restored ? 0 : 1);
    });
    window.show();
    QTimer::singleShot(100, &app, [&] { replay.togglePlayback(); movie.start(QUrl::fromLocalFile(output), canvas, .1, .2, .5, 0); });
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
    AppInfo appInfo;
    app.setApplicationName(appInfo.appName());
    app.setApplicationVersion(appInfo.version());
    app.setOrganizationName(appInfo.appName());
    app.setWindowIcon(QIcon(":/brand/uav-studio.svg"));
    QQuickStyle::setStyle("Basic");
    QFont font;
    font.setFamilies({"Segoe UI", "Microsoft YaHei UI", "Noto Sans CJK SC"});
    font.setPixelSize(13);
    app.setFont(font);
    const int appearanceArg = app.arguments().indexOf("--acceptance-appearance");
    const int frameArg = app.arguments().indexOf("--acceptance-frame");
    const int displayArg = app.arguments().indexOf("--acceptance-displays");
    const int movieFrameArg = app.arguments().indexOf("--acceptance-movie-frames");
    const int cameraArg = app.arguments().indexOf("--acceptance-camera");
    const int nativePlayheadArg = app.arguments().indexOf("--acceptance-native-playhead");
    const int nativeSceneArg = app.arguments().indexOf("--acceptance-native-scene");
    if (nativeSceneArg >= 0 && app.arguments().size() <= nativeSceneArg + 5) return 2;
    if (nativePlayheadArg >= 0 && app.arguments().size() <= nativePlayheadArg + 5) return 2;
    if (cameraArg >= 0 && (app.arguments().size() <= cameraArg + 2 || app.arguments().value(cameraArg + 1).startsWith("--"))) return 2;
    if (movieFrameArg >= 0 && (app.arguments().size() <= movieFrameArg + 2 || app.arguments().value(movieFrameArg + 1).startsWith("--"))) return 2;
    if (displayArg >= 0 && (app.arguments().size() <= displayArg + 2 || app.arguments().value(displayArg + 1).startsWith("--"))) return 2;
    if (appearanceArg >= 0 && (app.arguments().size() <= appearanceArg + 1 || app.arguments().value(appearanceArg + 1).startsWith("--"))) return 2;
    if (frameArg >= 0 && (app.arguments().size() <= frameArg + 1 || app.arguments().value(frameArg + 1).startsWith("--"))) return 2;
    const int acceptanceArg = nativeSceneArg >= 0 ? nativeSceneArg : appearanceArg >= 0 ? appearanceArg : frameArg >= 0 ? frameArg : displayArg >= 0 ? displayArg : movieFrameArg >= 0 ? movieFrameArg : cameraArg >= 0 ? cameraArg : nativePlayheadArg;
    const auto acceptanceDirectory = acceptanceArg >= 0 ? QFileInfo(app.arguments().value(acceptanceArg + 1)).absoluteFilePath() : QString();
    if (nativePlayheadArg >= 0 || nativeSceneArg >= 0) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, acceptanceDirectory);
    }
    AppearanceManager appearance(nullptr, acceptanceDirectory.isEmpty() ? QString() : acceptanceDirectory + "/appearance.ini");
    QQuickWindow::setDefaultAlphaBuffer(true);
    if (app.arguments().contains("--smoke-png"))
        return smokePng(app);
    if (app.arguments().contains("--smoke-mp4"))
        return smokeMovie(app);
    const auto args = app.arguments();
    const bool batch = args.size() > 1 && args[1] == QStringLiteral("--batch");
    const bool layoutSmoke = args.contains("--smoke-layout");
    const bool software = layoutSmoke || (batch && qEnvironmentVariable("QT_QUICK_BACKEND") == QStringLiteral("software"));
    QQuickWindow::setGraphicsApi(software ? QSGRendererInterface::Software : QSGRendererInterface::OpenGL);
    qmlRegisterType<CloudView>("UAVStudio", 1, 0, "CloudView");
    qmlRegisterType<ClipPreview>("UAVStudio", 1, 0, "ClipPreview");
    qmlRegisterType<SpeedGaugeItem>("UAVStudio", 1, 0, "SpeedGaugeItem");
    qmlRegisterType<TrajectoryLayerItem>("UAVStudio", 1, 0, "TrajectoryLayerItem");
    qmlRegisterType<CanvasScientificItem>("UAVStudio", 1, 0, "CanvasScientificItem");
    qmlRegisterUncreatableType<ReplayController>("UAVStudio", 1, 0, "ReplayController",
                                                 "Provided by the application");

    ReplayController replay;
    NativeRvizClient nativeRviz;
    // The engine owns image providers. Controllers and their worker cleanup
    // still use those providers while being destroyed, so the engine outlives them.
    QQmlApplicationEngine engine;
    auto *trajectoryImages = new PreviewImages;
    auto *movieImages = new MovieImages;
    auto *rosImages = new PreviewImages;
    TopicInspector inspector(replay, *rosImages);
    SpeedMeterModel speedMeterData;
    std::shared_ptr<const replay::Recording> speedRecording;
    QSet<QString> speedMeterIds;
    const auto synchronizeSpeedSources = [&] {
        const auto current = replay.recording();
        if (speedRecording != current) {
            speedMeterData.clear();
            speedMeterData.setTransforms(current ? current->transforms : nullptr);
            speedRecording = current;
            speedMeterIds.clear();
        }
        QSet<QString> wanted;
        for (const auto &value : replay.speedMeters()) {
            const auto meter = value.toMap();
            const auto id = meter.value("id").toString();
            wanted.insert(id);
            const auto source = meter.value("source").toMap();
            if (!source.value("topic").toString().isEmpty())
                speedMeterData.loadSource(id, replay.demo() ? QUrl() : QUrl::fromLocalFile(replay.bagPath()), source);
            else
                speedMeterData.useRecording(id, replay.demo() ? nullptr : current);
        }
        for (const auto &id : speedMeterIds - wanted) speedMeterData.removeMeter(id);
        speedMeterIds = wanted;
    };
    QObject::connect(&replay, &ReplayController::recordingChanged, &speedMeterData, synchronizeSpeedSources);
    synchronizeSpeedSources();
    TrajectoryController trajectory(*trajectoryImages);
    QObject::connect(&trajectory, &TrajectoryController::settingsEdited, &replay, &ReplayController::setTrajectorySettings);
    QObject::connect(&replay, &ReplayController::trajectorySettingsLoaded, &trajectory, &TrajectoryController::restoreSettings);
    MovieExporter movie(replay, *movieImages);
    movie.setNativeClient(&nativeRviz);
    FrameBatchExporter frameBatch(replay, movie);
    UpdateManager updater(appInfo.metadata());
    engine.rootContext()->setContextProperty("appInfo", &appInfo);
    engine.rootContext()->setContextProperty("updater", &updater);
    engine.rootContext()->setContextProperty("appearance", &appearance);
    engine.addImageProvider(QStringLiteral("trajectory"), trajectoryImages);
    engine.addImageProvider(QStringLiteral("movie"), movieImages);
    engine.addImageProvider(QStringLiteral("ros-inspector"), rosImages);
    engine.rootContext()->setContextProperty("inspector", &inspector);
    engine.rootContext()->setContextProperty("speedMeterData", &speedMeterData);
    engine.rootContext()->setContextProperty("trajectory", &trajectory);
    engine.rootContext()->setContextProperty("movie", &movie);
    engine.rootContext()->setContextProperty("frameBatch", &frameBatch);
    engine.rootContext()->setContextProperty("nativeRviz", &nativeRviz);
    const bool smoke = args.contains("--smoke");
    engine.rootContext()->setContextProperty("replay", &replay);
    engine.rootContext()->setContextProperty("smokeTest", smoke);
    engine.rootContext()->setContextProperty("automaticNativeBackend", !smoke && !layoutSmoke && acceptanceArg < 0);
    engine.load(QUrl(batch ? QStringLiteral("qrc:/qml/BatchExport.qml") : QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    // Stop QML views/decoders before their context controllers. Providers remain
    // owned by the still-live engine until every controller has finished cleanup.
    const auto closeViews = qScopeGuard([&engine] { qDeleteAll(engine.rootObjects()); });
    // Background network checks only run for an ordinary interactive session.
    // Offline rendering, acceptance tools and smoke tests never need networking.
    if (!batch && !smoke && !layoutSmoke && acceptanceArg < 0)
        updater.startAutomaticCheck();
    if (nativeSceneArg >= 0) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) return 1;
        window->setProperty("nativeBackendSelected", true);
        runNativeSceneAcceptance(app, *window, replay, nativeRviz, movie, acceptanceDirectory,
            args.value(nativeSceneArg+2), args.value(nativeSceneArg+3), args.value(nativeSceneArg+4), args.value(nativeSceneArg+5));
        return app.exec();
    }
    if (nativePlayheadArg >= 0) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) return 1;
        window->setProperty("nativeBackendSelected", true);
        runNativeReplayAcceptance(app, *window, replay, nativeRviz, acceptanceDirectory,
            args.value(nativePlayheadArg + 2), args.value(nativePlayheadArg + 3),
            args.value(nativePlayheadArg + 4), args.value(nativePlayheadArg + 5));
        return app.exec();
    }
    if (cameraArg >= 0) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) return 1;
        runCameraAcceptance(app, *window, replay, movie, acceptanceDirectory, app.arguments().value(cameraArg + 2));
        return app.exec();
    }
    if (movieFrameArg >= 0) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) return 1;
        runMovieFrameAcceptance(app, *window, replay, movie, acceptanceDirectory,
            app.arguments().value(movieFrameArg + 2), app.arguments().value(movieFrameArg + 3));
        return app.exec();
    }
    if (displayArg >= 0) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) return 1;
        runDisplayAcceptance(app, *window, replay, acceptanceDirectory, app.arguments().value(displayArg + 2));
        return app.exec();
    }
    if (frameArg >= 0) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) return 1;
        runFrameAcceptance(app, *window, replay, movie, frameBatch, trajectory, appearance, acceptanceDirectory,
                           app.arguments().value(frameArg + 2), app.arguments().value(frameArg + 3));
        return app.exec();
    }
    if (appearanceArg >= 0) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window) return 1;
        runAppearanceAcceptance(app, *window, appearance, replay, acceptanceDirectory, app.arguments().value(appearanceArg + 2));
        return app.exec();
    }
    if (layoutSmoke) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window)
            return 1;
        window->resize(980, 640);
        QTimer::singleShot(250, &app, [&] {
            const auto *canvas = window->findChild<QQuickItem *>(QStringLiteral("previewCanvas"));
            const auto image = window->grabWindow();
            const auto destination = args.value(args.indexOf("--smoke-layout") + 1);
            const bool valid = canvas && canvas->width() > 350. && canvas->height() > 190. &&
                std::abs(canvas->width() / canvas->height() - 16. / 9.) < .01 && !image.isNull();
            app.exit(valid && (destination.isEmpty() || image.save(destination)) ? 0 : 1);
        });
        return app.exec();
    }
    if (batch) {
        if (args.size() != 6 && args.size() != 10 && args.size() != 12)
            return 2;
        bool beginOk = false, secondsOk = false;
        const double begin = args[4].toDouble(&beginOk), seconds = args[5].toDouble(&secondsOk);
        if (!beginOk || !secondsOk || !std::isfinite(begin) || !std::isfinite(seconds) || begin < 0. || seconds < 0.)
            return 2;
        std::array<int, 5> videoOptions{1920, 1080, 60, 18, 1};
        for (int i = 0; i < std::min(int(videoOptions.size()), int(args.size()) - 6); ++i) {
            bool ok = false;
            videoOptions[i] = args[6 + i].toInt(&ok);
            if (!ok)
                return 2;
        }
        const auto encoder = args.value(11, QStringLiteral("cpu"));
        if (encoder != "cpu" && encoder != "nvenc")
            return 2;
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        auto *canvas = window ? window->findChild<QQuickItem *>(QStringLiteral("batchCanvas")) : nullptr;
        if (!canvas)
            return 1;
        const bool demo = args[2] == QStringLiteral(":demo:");
        const QFileInfo source(demo ? QString() : args[2]);
        const auto destination = QUrl::fromLocalFile(QFileInfo(args[3]).absoluteFilePath());
        const bool still = QFileInfo(destination.toLocalFile()).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0;
        const bool project = QStringList{"uavstudio", "slamfpv"}.contains(source.suffix().toLower());
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
        // Allow the exporter's 30-second graphics preflight to report its
        // specific failure and restore state before the outer watchdog.
        renderTimeout.setInterval(35'000);
        QObject::connect(&movie, &MovieExporter::changed, &renderTimeout, [&] {
            if (movie.exporting())
                renderTimeout.start();
            else
                renderTimeout.stop();
        });
        QObject::connect(&renderTimeout, &QTimer::timeout, &app, [completed] {
            completed(false, QStringLiteral("The export window did not produce another frame within 35 seconds."));
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
                            videoOptions[0], videoOptions[1], videoOptions[2], videoOptions[3], videoOptions[4], encoder == "nvenc");
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
            // An empty desktop is intentional. Only an explicit CLI demo
            // request initializes demonstration data before export validation.
            if (demo)
                replay.openDemo();
            else if (project)
                replay.openProject(url);
            else
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
                QList<ClipPreview *> sinks;
                if (window)
                    videoOutputs(window->contentItem(), sinks);
                if (sinks.size() != 2) {
                    qWarning("Both video preview outputs were not created");
                    app.exit(1);
                    return;
                }
                // Settle both precise, paused targets before advancing through the
                // short first clip. A slow first decode must not be mistaken for
                // a broken decoder after that clip has legitimately become hidden.
                replay.pause();
                replay.seek(.2);
                auto decoded = std::make_shared<QSet<ClipPreview *>>();
                auto playbackStarted = std::make_shared<bool>(false);
                for (auto *sink : sinks) {
                    QObject::connect(sink, &ClipPreview::frameChanged, &app, [&app, &replay, decoded, playbackStarted, sink, sinks] {
                        if (!*playbackStarted && sink->ready() && !sink->busy()) {
                            decoded->insert(sink);
                            if (decoded->size() == sinks.size()) {
                                *playbackStarted = true;
                                const auto initial = sinks.last()->actualSeconds();
                                replay.togglePlayback();
                                QTimer::singleShot(1000, &app, [&app, sinks, initial] {
                                    if (!sinks.last()->ready() || sinks.last()->actualSeconds() <= initial) {
                                        qWarning("Video preview did not advance during playback");
                                        app.exit(1);
                                    } else app.exit(0);
                                });
                            }
                        }
                    });
                }
                QTimer::singleShot(12000, &app, [&app, sinks] {
                    qWarning("Video preview did not decode a frame");
                    for (const auto *sink : sinks)
                        qWarning() << "Preview source/target/enabled/ready/busy/actual/error:"
                                   << sink->source() << sink->sourceSeconds() << sink->decodingEnabled()
                                   << sink->ready() << sink->busy() << sink->actualSeconds() << sink->error();
                    app.exit(1);
                });
            } catch (const std::exception &) {
                app.exit(1);
            }
        });
    } else if (app.arguments().size() == 2) {
        const auto path = QFileInfo(app.arguments().at(1)).absoluteFilePath();
        const auto url = QUrl::fromLocalFile(path);
        const auto extension = QFileInfo(path).suffix().toLower();
        if (extension == QStringLiteral("slamfpv") || extension == QStringLiteral("uavstudio"))
            replay.openProject(url);
        else if (extension == QStringLiteral("rviz") || extension == QStringLiteral("yaml") || extension == QStringLiteral("yml"))
            replay.openRviz(url);
        else
            replay.openBag(url);
    }
    return app.exec();
}
