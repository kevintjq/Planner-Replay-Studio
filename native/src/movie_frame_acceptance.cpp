#include "movie_frame_acceptance.h"
#include "cloud_view.h"
#include "movie_exporter.h"
#include "video_reader.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
#include <algorithm>
#include <cmath>

namespace {
QJsonObject compare(const QImage &first, const QImage &second)
{
    if (first.isNull() || first.size() != second.size()) return {{"valid", false}};
    const auto a = first.convertToFormat(QImage::Format_RGB888), b = second.convertToFormat(QImage::Format_RGB888);
    double absolute = 0., squared = 0.; int maximum = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width() * 3; ++x) {
            const int delta = std::abs(int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x]));
            absolute += delta; squared += delta * delta; maximum = std::max(maximum, delta);
        }
    const double samples = double(a.width()) * a.height() * 3., mse = squared / samples;
    return {{"valid", true}, {"max_channel_delta", maximum}, {"mean_absolute_error", absolute / samples},
        {"psnr_db", mse > 0. ? 10. * std::log10(255. * 255. / mse) : 100.}};
}
}
void runMovieFrameAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                             MovieExporter &movie, const QString &directory, const QString &bag, const QString &video)
{
    struct State {
        int caseIndex = 0, stillIndex = 0;
        bool moviePhase = true, valid = true, captureValid = true;
        QList<QImage> captures, decoded;
        QList<double> times;
        QJsonArray results, movies;
        QJsonObject capturedDifference;
        QQuickItem *canvas = nullptr;
        CloudView *cloud = nullptr;
        std::shared_ptr<const replay::Recording> recording;
        double previous = 0.; bool previousPlaying = false;
    };
    auto state = std::make_shared<State>();
    QDir().mkpath(directory); window.resize(1440, 900);
    auto next = std::make_shared<std::function<void()>>();
    const auto report = [&, state, directory] {
        const QJsonObject report{{"passed", state->valid}, {"real_bag", !replay.demo()},
            {"source_video", !video.isEmpty()}, {"movies", state->movies}, {"frames", state->results},
            {"limitations", "Short 1080P/4K clips only. MP4 uses YUV420 chroma subsampling, not pixel-identical RGB. Does not certify source-video PTS, long exports, audio, NVENC or performance."}};
        QFile file(QDir(directory).filePath("movie-frame-acceptance.json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0) state->valid = false;
        app.exit(state->valid ? 0 : 1);
    };
    *next = [&, state, directory, report] {
        if (state->caseIndex >= 2) { report(); return; }
        const int width = state->caseIndex ? 3840 : 1920, height = state->caseIndex ? 2160 : 1080;
        const double time = state->caseIndex ? 7.21 : 3.125;
        const auto prefix = QString::number(width);
        if (state->moviePhase) {
            state->captures.clear(); state->decoded.clear(); state->times.clear();
            replay.pause(); replay.seek(4.321); if (!state->caseIndex) replay.togglePlayback();
            state->previous = replay.position(); state->previousPlaying = replay.playing();
            movie.start(QUrl::fromLocalFile(QDir(directory).filePath(prefix + ".mp4")), state->canvas,
                time, time + .1, 1., -1, width, height, 30, 0);
        } else {
            // Deliberately begin at unrelated playhead positions before each still.
            replay.pause(); replay.seek(state->stillIndex % 2 ? 1.05 : 8.);
            state->previous = replay.position(); state->previousPlaying = false; state->captureValid = true;
            movie.startStill(QUrl::fromLocalFile(QDir(directory).filePath(prefix + "-" + QString::number(state->stillIndex) + ".png")),
                state->canvas, time + state->stillIndex / 30., {{"width", width}, {"height", height}, {"dpi", 300}, {"background", replay.sceneBackground().name()}});
        }
    };
    QObject::connect(&movie, &MovieExporter::frameCaptured, &app, [&, state, directory](const QImage &image, double time) {
        if (state->moviePhase) {
            if (state->captures.isEmpty()) image.save(QDir(directory).filePath(QString::number(state->caseIndex ? 3840 : 1920) + "-movie-raw.png"));
            state->captures.append(image); state->times.append(time);
        }
        else {
            if (!state->stillIndex) image.save(QDir(directory).filePath(QString::number(state->caseIndex ? 3840 : 1920) + "-still-raw.png"));
            if (state->stillIndex >= state->captures.size()) { state->captureValid = false; return; }
            state->capturedDifference = compare(state->captures[state->stillIndex], image);
            state->captureValid &= state->capturedDifference["valid"].toBool() && state->capturedDifference["max_channel_delta"].toInt(255) == 0 &&
                std::abs(time - state->times[state->stillIndex]) < 1.e-9;
        }
    });
    QObject::connect(&movie, &MovieExporter::finished, &app, [&, state, next, directory, report](bool success, const QString &message) {
        const int width = state->caseIndex ? 3840 : 1920, height = state->caseIndex ? 2160 : 1080;
        bool valid = success && replay.recording() == state->recording && replay.playing() == state->previousPlaying &&
            std::abs(replay.position() - state->previous) < 1.e-8 && state->canvas->property("captureOptions").toMap().isEmpty() &&
            state->cloud->captureSize().isEmpty();
        const auto metrics = movie.captureMetrics();
        valid &= !metrics.isEmpty() && metrics.first().toMap().value("width").toInt() == width && metrics.first().toMap().value("height").toInt() == height;
        if (state->moviePhase) {
            valid &= state->captures.size() == 3;
            try {
                replay::VideoReader reader(QDir(directory).filePath(QString::number(width) + ".mp4"));
                while (reader.advance() && state->decoded.size() < 4) state->decoded.append(replay::imageOf(reader.frame));
                valid &= state->decoded.size() == 3;
            } catch (const std::exception &) { valid = false; }
            state->movies.append(QJsonObject{{"width", width}, {"passed", valid}, {"decoded_frame_count", state->decoded.size()},
                {"cloud_framebuffers", QJsonArray::fromVariantList(metrics)}, {"playback_restored", replay.playing() == state->previousPlaying}, {"message", message}});
            state->valid &= valid;
            if (!valid) { report(); return; }
            state->moviePhase = false; state->stillIndex = 0;
        } else {
            const QImage image(QDir(directory).filePath(QString::number(width) + "-" + QString::number(state->stillIndex) + ".png"));
            const auto encodedDifference = compare(image, state->decoded[state->stillIndex]);
            valid &= state->captureValid && image.size() == QSize(width, height) && encodedDifference["valid"].toBool() &&
                encodedDifference["mean_absolute_error"].toDouble(255.) <= 3. && encodedDifference["psnr_db"].toDouble() >= 30.;
            state->valid &= valid;
            state->results.append(QJsonObject{{"width", width}, {"frame_index", state->stillIndex}, {"passed", valid},
                {"experiment_time", state->times[state->stillIndex]}, {"preencode_difference", state->capturedDifference},
                {"mp4_difference", encodedDifference}, {"message", message}});
            if (++state->stillIndex == 3) { ++state->caseIndex; state->moviePhase = true; }
        }
        QTimer::singleShot(150, &app, *next);
    });
    auto *ready = new QTimer(&app); ready->setInterval(300);
    QObject::connect(ready, &QTimer::timeout, &app, [&, ready, state, next, video] {
        if (replay.loading()) return;
        ready->stop();
        if (replay.demo() || replay.duration() <= 8.) { app.exit(1); return; }
        state->canvas = window.findChild<QQuickItem *>("previewCanvas"); state->cloud = window.findChild<CloudView *>("primaryCloudView");
        if (!state->canvas || !state->cloud) { app.exit(1); return; }
        state->recording = replay.recording();
        if (!video.isEmpty()) replay.addVideo(QUrl::fromLocalFile(video));
        replay.addSceneText("Still / movie frame agreement · 科研画面");
        replay.editSceneLayer(replay.sceneLayers().last().toMap().value("id").toString(), {{"start", 0.}, {"end", replay.duration()}});
        replay.addCloudView(3); replay.setSpeedGauge({{"visible", true}});
        QTimer::singleShot(1000, &app, *next);
    });
    replay.openBag(QUrl::fromLocalFile(bag)); ready->start();
    QTimer::singleShot(180000, &app, [&app] { app.exit(1); });
}
