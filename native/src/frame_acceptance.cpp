#include "frame_acceptance.h"
#include "appearance_manager.h"
#include "cloud_view.h"
#include "movie_exporter.h"
#include "frame_batch_exporter.h"
#include "trajectory_controller.h"
#include "replay_controller.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QTimer>
#include <cmath>

void runFrameAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                        MovieExporter &movie, FrameBatchExporter &batch, TrajectoryController &trajectory, AppearanceManager &appearance, const QString &directory,
                        const QString &bag, const QString &video)
{
    struct Case { QString file; int width, height, dpi; double time; QString scope, background; };
    struct State {
        QList<Case> cases;
        int index = 0;
        bool valid = true;
        QJsonArray results;
        QImage themeReference;
        QQuickItem *canvas = nullptr;
        CloudView *cloud = nullptr;
        std::shared_ptr<const replay::Recording> record;
        QString viewId;
        int batchIndex = 0;
        bool batchesStarted = false, cancelQueued = false;
        QJsonArray batchResults;
        int trajectoryPhase = -1, trajectoryIndex = 0;
        QJsonArray trajectoryResults;
        QUrl trajectoryPreview;
        QVariantMap trajectorySettings;
    };
    auto state = std::make_shared<State>();
    state->cases = {
        {"canvas-4k.png", 3840, 2160, 600, 3.125, "canvas", "transparent"},
        {"canvas-8k.png", 7680, 4320, 1200, 7.21, "canvas", "transparent"},
        {"view.png", 2560, 1440, 300, 1.05, "view", "transparent"},
        {"canvas.pdf", 3840, 2160, 300, 5.75, "canvas", "white"},
        {"canvas.svg", 3840, 2160, 300, 5.75, "canvas", "white"},
        {"layers.png", 1920, 1080, 300, 2.25, "layers", "transparent"},
        {"theme-dark.png", 1920, 1080, 300, 3.125, "canvas", "transparent"},
        {"theme-light.png", 1920, 1080, 300, 3.125, "canvas", "transparent"},
        {"canvas.jpg", 1920, 1080, 300, 2.5, "canvas", "white"},
        {"canvas.tiff", 3840, 2160, 600, 2.5, "canvas", "transparent"}
    };
    QDir().mkpath(directory);
    window.resize(1440, 900);
    auto next = std::make_shared<std::function<void()>>();
    auto nextBatch = std::make_shared<std::function<void()>>();
    auto nextTrajectory = std::make_shared<std::function<void()>>();
    auto report = [&, state, directory] {
            const QJsonObject report{{"passed", state->valid}, {"real_bag", !replay.demo()},
                {"video_layers", int(replay.videoClips().size())}, {"cases", state->results},
                {"batches", state->batchResults},
                {"trajectory_frames", state->trajectoryResults},
                {"limitations", "Does not certify exact video PTS, vector scene overlays, tiling or native RViz parity."}};
            QFile file(QDir(directory).filePath("frame-acceptance.json"));
            if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0) state->valid = false;
            app.exit(state->valid ? 0 : 1);
    };
    *nextTrajectory = [&, state, report, video, directory] {
        if (video.isEmpty()) { report(); return; }
        if (state->trajectoryPhase < 0) {
            state->trajectoryPhase = 0; window.setProperty("workspaceIndex", 3);
            trajectory.open(QUrl::fromLocalFile(video)); return;
        }
        if (state->trajectoryIndex >= 4) { report(); return; }
        const QStringList formats{"png", "tiff", "pdf", "svg"};
        const bool eightK = state->trajectoryIndex == 1;
        trajectory.exportFrame(QUrl::fromLocalFile(QDir(directory).filePath("trajectory." + formats[state->trajectoryIndex])),
            {{"width", eightK ? 7680 : 3840}, {"height", eightK ? 4320 : 2160}, {"dpi", eightK ? 1200 : 600}, {"background", "transparent"}});
    };
    QObject::connect(&trajectory, &TrajectoryController::changed, &app, [&, state, nextTrajectory, directory] {
        if (trajectory.working()) return;
        if (state->trajectoryPhase == 0) { state->trajectoryPhase = 1; trajectory.preview(3.125); }
        else if (state->trajectoryPhase == 1) {
            state->trajectoryPhase = 2; state->trajectoryPreview = trajectory.image(); state->trajectorySettings = trajectory.settings();
            QTimer::singleShot(300, &app, [&, state, nextTrajectory, directory] {
                window.grabWindow().save(QDir(directory).filePath("trajectory-workspace.png")); (*nextTrajectory)();
            });
        }
    });
    QObject::connect(&trajectory, &TrajectoryController::frameExported, &app, [&, state, nextTrajectory, directory](bool success, const QString &message) {
        const QStringList formats{"png", "tiff", "pdf", "svg"};
        const auto path = QDir(directory).filePath("trajectory." + formats[state->trajectoryIndex]);
        bool valid = success && trajectory.image() == state->trajectoryPreview && trajectory.settings() == state->trajectorySettings;
        if (state->trajectoryIndex < 2) {
            const QImage image(path); const bool eightK = state->trajectoryIndex == 1;
            // 源视频若恰好铺满画布，角落自然不透明；透明背景不等于自动抠像。
            valid &= image.size() == (eightK ? QSize(7680, 4320) : QSize(3840, 2160)) && image.hasAlphaChannel();
            valid &= std::abs(image.dotsPerMeterX() * .0254 - (eightK ? 1200 : 600)) < .1;
        } else { QFile file(path); valid &= file.open(QIODevice::ReadOnly) && file.size() > 1000; }
        state->valid &= valid; state->trajectoryResults.append(QJsonObject{{"file", path}, {"passed", valid}, {"experiment_time", 3.125}, {"message", message}});
        ++state->trajectoryIndex; QTimer::singleShot(0, &app, *nextTrajectory);
    });
    *nextBatch = [&, state, nextTrajectory, directory] {
        if (state->batchIndex >= 5) { (*nextTrajectory)(); return; }
        replay.pause(); replay.seek(4.321);
        if (state->batchIndex == 0) replay.togglePlayback();
        QVariantMap options{{"width", 640}, {"height", 360}, {"dpi", 300}, {"background", "white"}, {"format", "png"}};
        if (state->batchIndex == 0) {
            options.insert("mode", "manual"); options.insert("times", "3.125,1.25");
            options.insert("allViews", true); options.insert("figure", true); options.insert("figureFormat", "svg");
        } else if (state->batchIndex == 1) {
            options.insert("mode", "interval"); options.insert("begin", 1.); options.insert("end", 1.5); options.insert("interval", .25);
        } else if (state->batchIndex == 2) {
            replay.seek(1.25); replay.addMarker(); replay.seek(3.125); replay.addMarker(); replay.seek(4.321);
            options.insert("mode", "markers");
        } else if (state->batchIndex == 3) {
            options.insert("mode", "samples"); options.insert("times", "0,30"); options.insert("sampleFps", 30.);
        } else {
            options.insert("mode", "interval"); options.insert("begin", 1.); options.insert("end", 7.); options.insert("interval", 1.);
        }
        batch.start(QUrl::fromLocalFile(directory), state->canvas, options);
    };
    QObject::connect(&batch, &FrameBatchExporter::changed, &app, [&, state] {
        if (state->batchIndex == 4 && batch.active() && !state->cancelQueued && batch.progress() > 0.) {
            state->cancelQueued = true; QTimer::singleShot(0, &batch, &FrameBatchExporter::cancel);
        }
    });
    QObject::connect(&batch, &FrameBatchExporter::finished, &app, [&, state, nextBatch](bool success, const QString &message) {
        const bool cancelled = state->batchIndex == 4;
        bool valid = (success != cancelled) && replay.recording() == state->record &&
            state->canvas->property("captureOptions").toMap().isEmpty() && std::abs(replay.position() - 4.321) < 1.e-8 && replay.playing() == (state->batchIndex == 0);
        QFile manifest(QDir(batch.outputDirectory()).filePath("manifest.json"));
        valid &= manifest.open(QIODevice::ReadOnly);
        const auto object = QJsonDocument::fromJson(manifest.readAll()).object();
        const auto completed = object.value("completed").toArray();
        const int expected[] = {4, 3, 2, 2, 7};
        valid &= object.value("status").toString() == (cancelled ? "cancelled" : "complete");
        valid &= cancelled ? completed.size() >= 1 && completed.size() < expected[state->batchIndex] : completed.size() == expected[state->batchIndex];
        for (const auto &value : completed) {
            const auto item = value.toObject();
            if (!item.value("passed").toBool()) continue;
            const QImage image(QDir(batch.outputDirectory()).filePath(item.value("file").toString()));
            valid &= image.size() == QSize(640, 360) && image.text("Description").contains(QString::number(item.value("experiment_time").toDouble(), 'f', 9));
        }
        if (state->batchIndex == 0) {
            QFile figure(QDir(batch.outputDirectory()).filePath("figure.svg"));
            valid &= figure.open(QIODevice::ReadOnly);
            const auto bytes = figure.readAll(); valid &= bytes.contains("<text") && bytes.contains("(d)") && bytes.contains("3.125000000");
        }
        state->valid &= valid;
        state->batchResults.append(QJsonObject{{"case", state->batchIndex}, {"passed", valid}, {"message", message}, {"directory", batch.outputDirectory()}, {"completed", completed.size()}});
        ++state->batchIndex; QTimer::singleShot(50, &app, *nextBatch);
    });
    *next = [&, state, nextBatch] {
        if (state->index >= state->cases.size()) { state->batchesStarted = true; (*nextBatch)(); return; }
        const auto item = state->cases[state->index];
        replay.seek(item.time);
        if (item.file.startsWith("theme-")) appearance.setMode(item.file.contains("light") ? AppearanceManager::Light : AppearanceManager::Dark);
        const QVariantMap options{{"width", item.width}, {"height", item.height}, {"dpi", item.dpi},
            {"background", item.background}, {"scope", item.scope}, {"viewId", state->viewId},
            {"includeText", true}, {"includeGauge", true}, {"layerIds", QStringList{"speed"}}};
        movie.startStill(QUrl::fromLocalFile(QDir(directory).filePath(item.file)), state->canvas, item.time, options);
    };
    QObject::connect(&movie, &MovieExporter::finished, &app, [&, state, next, directory](bool success, const QString &message) {
        if (state->batchesStarted) return;
        const auto item = state->cases[state->index];
        bool valid = success && replay.recording() == state->record &&
            window.findChild<QQuickItem *>("previewCanvas") == state->canvas &&
            window.findChild<CloudView *>("primaryCloudView") == state->cloud &&
            std::abs(replay.position() - item.time) < 1.e-8 && state->canvas->property("captureOptions").toMap().isEmpty();
        const auto path = QDir(directory).filePath(item.file);
        if (item.file.endsWith("png") || item.file.endsWith("jpg") || item.file.endsWith("tiff")) {
            const QImage image(path);
            valid &= image.size() == QSize(item.width, item.height) && std::abs(image.dotsPerMeterX() * .0254 - item.dpi) < .1;
            if (item.file.endsWith("png")) valid &= image.text("Description").contains(QString::number(item.time, 'f', 9));
            if (!image.isNull()) {
                if (item.background == "transparent") valid &= image.pixelColor(0, 0).alpha() == 0;
                int nonEmpty = 0;
                for (int y = 0; y < image.height(); y += 11)
                    for (int x = 0; x < image.width(); x += 11) nonEmpty += image.pixelColor(x, y).alpha() > 0;
                valid &= nonEmpty > 10;
                if (item.file == "theme-dark.png") state->themeReference = image;
                if (item.file == "theme-light.png") valid &= image == state->themeReference;
            }
        } else {
            QFile file(path);
            valid &= file.open(QIODevice::ReadOnly);
            const auto bytes = file.readAll();
            valid &= item.file.endsWith("pdf") ? bytes.startsWith("%PDF-") : bytes.contains("<svg") && bytes.contains("embedded raster");
        }
        state->valid &= valid;
        state->results.append(QJsonObject{{"file", item.file}, {"passed", valid}, {"message", message},
            {"cloud_framebuffers", QJsonArray::fromVariantList(movie.captureMetrics())},
            {"experiment_time", item.time}, {"width", item.width}, {"height", item.height}, {"dpi", item.dpi}});
        ++state->index;
        QTimer::singleShot(200, &app, *next);
    });
    auto *ready = new QTimer(&app);
    ready->setInterval(300);
    QObject::connect(ready, &QTimer::timeout, &app, [&, ready, state, next, video, bag] {
        if (replay.loading()) return;
        ready->stop();
        state->valid &= bag.isEmpty() || !replay.demo();
        state->valid &= replay.duration() > 8 && window.rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL;
        state->record = replay.recording();
        state->canvas = window.findChild<QQuickItem *>("previewCanvas");
        state->cloud = window.findChild<CloudView *>("primaryCloudView");
        if (!state->canvas || !state->cloud) { app.exit(1); return; }
        if (!video.isEmpty()) replay.addVideo(QUrl::fromLocalFile(video));
        replay.addCloudView(3);
        if (!replay.cloudViews().isEmpty()) state->viewId = replay.cloudViews().first().toMap().value("id").toString();
        replay.addSceneText("Experiment Frame · 科研画面");
        if (!replay.sceneLayers().isEmpty()) replay.editSceneLayer(replay.sceneLayers().last().toMap().value("id").toString(), {{"start", 0}, {"end", replay.duration()}});
        replay.setSpeedGauge({{"visible", true}});
        QTimer::singleShot(1500, &app, *next);
    });
    if (!bag.isEmpty()) replay.openBag(QUrl::fromLocalFile(bag));
    ready->start();
    QTimer::singleShot(300000, &app, [&app] { app.exit(1); });
}
