#include "native_scene_acceptance.h"
#include "cloud_view.h"
#include "movie_exporter.h"
#include "native_rviz_client.h"
#include "replay_controller.h"
#include "video_reader.h"
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJSValue>
#include <QMap>
#include <QQuickWindow>
#include <QSet>
#include <QTimer>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>

namespace {
QVariantMap optionValues(const QVariant &value)
{
    return value.metaType() == QMetaType::fromType<QJSValue>()
        ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
}
QList<QQuickItem *> visualItems(QQuickItem *parent)
{
    QList<QQuickItem *> result;
    if (!parent) return result;
    result.append(parent);
    for (auto *child : parent->childItems()) result.append(visualItems(child));
    return result;
}
QQuickItem *namedItem(QQuickWindow &window, const QString &name)
{
    for (auto *item : visualItems(window.contentItem())) if (item->objectName() == name) return item;
    return nullptr;
}
QString pixelHash(const QImage &image)
{
    const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int row = 0; row < rgba.height(); ++row)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(rgba.constScanLine(row)), rgba.width() * 4));
    return QString::fromLatin1(hash.result().toHex());
}
int colorPixels(const QImage &image, const QColor &wanted)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (std::abs(color.red() - wanted.red()) < 8 && std::abs(color.green() - wanted.green()) < 8
                && std::abs(color.blue() - wanted.blue()) < 8 && color.alpha() > 240) ++count;
        }
    return count;
}
bool nativeHasContent(const QImage &image, const QColor &background)
{
    if (image.isNull() || !background.isValid()) return false;
    int different = 0;
    const int step = std::max(1, image.width() / 960);
    for (int y = 0; y < image.height(); y += step)
        for (int x = 0; x < image.width(); x += step) {
            const auto color = image.pixelColor(x, y);
            if (std::abs(color.red() - background.red()) + std::abs(color.green() - background.green())
                + std::abs(color.blue() - background.blue()) > 30 && ++different >= 100) return true;
        }
    return false;
}
QJsonObject imageDifference(const QImage &first, const QImage &second)
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
    return {{"valid", true}, {"maximum", maximum}, {"mae", absolute / samples},
        {"psnr_db", mse > 0. ? 10. * std::log10(255. * 255. / mse) : 100.}};
}
}

void runNativeSceneAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                              NativeRvizClient &client, MovieExporter &movie, const QString &directory,
                              const QString &bag, const QString &config, const QString &launcher,
                              const QString &distribution)
{
    struct State {
        int phase = 0, step = 0, loads = 0, frameCount = 0, nativeFrames = 0, heartbeats = 0;
        int syncAcks = 0, initialSyncAcks = 0, caseNativeFrames = 0, successfulNativeFrames = 0;
        int indexRequests = 0;
        bool finished = false, waiting = false, valid = true, previousPlaying = false, cancelQueued = false;
        bool multiview = false, caseValid = true;
        double time = 8.025, previousPosition = 0.;
        QPointer<QQuickItem> canvas, nativeView, panel;
        QPointer<QObject> coordinator;
        QMap<QString, QPointer<QQuickItem>> nativeViews;
        QStringList viewIds{"main"}, caseViewIds;
        QSet<QString> nativeKeys;
        QSize outputSize;
        std::shared_ptr<const replay::Recording> recording;
        QVariantList layers, views, markers;
        QVariantMap mainView, gauge;
        QVariant captureOptions;
        QList<QPair<QPointer<CloudView>, QSize>> captureSizes;
        QString currentFile, textId, targetPropertyId, error, caseSceneRevision;
        QVariant originalTarget;
        QJsonArray cases, nativeOutputs, frameInspections;
        QJsonArray previewDiagnostics;
        QList<QImage> movieFrames;
        QList<double> movieTimes;
        QImage canvasReference;
        QElapsedTimer deadline, jobTime;
        qint64 lastProgressAt = -5000;
    };
    auto state = std::make_shared<State>(); state->deadline.start();
    state->multiview = app.arguments().contains("--native-multiview");
    QDir().mkpath(directory); window.resize(1440, 900);
    auto *timer = new QTimer(&app); timer->setInterval(100);
    auto *heartbeat = new QTimer(&app); heartbeat->setInterval(10);
    QObject::connect(heartbeat, &QTimer::timeout, &app, [state] { if (state->waiting) ++state->heartbeats; });
    heartbeat->start();
    const auto intact = [&, state] {
        bool viewsIntact = state->nativeViews.size() == state->viewIds.size();
        for (auto it = state->nativeViews.cbegin(); it != state->nativeViews.cend(); ++it)
            viewsIntact &= it.value() && it.value()->property("nativeSceneImage").toBool()
                && it.value()->property("exportViewId").toString() == it.key()
                && namedItem(window, it.key() == "main" ? QStringLiteral("nativeMainView") : "nativeView-" + it.key()) == it.value();
        if (state->multiview) {
            viewsIntact &= state->coordinator && client.independentCameras() && client.renderViews().size() == 2;
            for (const auto &entry : client.renderViews()) {
                const auto view = entry.toMap(); const auto id = view.value("view_id").toString();
                viewsIntact &= state->viewIds.contains(id) && id != "main"
                    && view.value("class").toString() == (id == state->viewIds.value(2) ? "rviz/TopDownOrtho" : "rviz/Orbit");
            }
            for (auto *item : visualItems(state->canvas))
                if (qobject_cast<CloudView *>(item) && item->isVisible()) viewsIntact = false;
        }
        return viewsIntact && state->recording && replay.recording() == state->recording && state->loads == 0 && !replay.loading()
            && state->canvas && state->nativeView && namedItem(window, "previewCanvas") == state->canvas
            && namedItem(window, "nativeMainView") == state->nativeView
            && replay.sceneLayers() == state->layers && replay.cloudViews() == state->views
            && replay.markers() == state->markers && replay.mainView() == state->mainView && replay.speedGauge() == state->gauge;
    };
    const auto finish = [&, state, timer, heartbeat, directory, intact](const QString &error) {
        if (state->finished) return;
        state->finished = true; timer->stop(); heartbeat->stop();
        if (!error.isEmpty()) { state->error = error; state->valid = false; }
        const int expectedSuccessfulFrames = state->multiview ? 21 : 8;
        for (const auto &id : state->viewIds) {
            const auto image = state->nativeViews.value(id);
            const auto *view = image ? image->parentItem() : nullptr;
            auto local = view ? view->property("previewFrame").toMap() : QVariantMap{};
            auto actual = client.viewPreviews().value(id).toMap();
            local.remove("source"); actual.remove("source");
            state->previewDiagnostics.append(QJsonObject{{"view_id", id},
                {"view_target_ns", view ? view->property("targetNs").toString() : QString()},
                {"canvas_target_ns", state->canvas ? state->canvas->property("nativeTargetNs").toString() : QString()},
                {"matched", view && view->property("matchesPlayhead").toBool()},
                {"local_frame", QJsonObject::fromVariantMap(local)}, {"client_frame", QJsonObject::fromVariantMap(actual)}});
        }
        state->valid &= intact() && !replay.demo() && state->cases.size() == 10
            && state->successfulNativeFrames == expectedSuccessfulFrames;
        if (!state->multiview) state->valid &= state->nativeFrames == 8;
        const QJsonObject report{{"passed", state->valid}, {"real_bag", !replay.demo()},
            {"native_multiview", state->multiview}, {"actual_view_ids", QJsonArray::fromStringList(state->viewIds)},
            {"native_view_sync_acks", state->syncAcks}, {"native_view_descriptors", QJsonArray::fromVariantList(client.renderViews())},
            {"cases", state->cases}, {"native_outputs", state->nativeOutputs},
            {"preview_diagnostics", state->previewDiagnostics},
            {"loading_signals", state->loads}, {"source_views_and_edits_preserved", intact()},
            {"native_frame_count", state->nativeFrames}, {"error", state->error},
            {"native_status", client.status()}, {"native_diagnostic", client.diagnostic()},
            {"index_context", state->panel ? QJsonObject{
                {"bag", state->panel->property("experimentBag").toUrl().toString()},
                {"time_basis", state->panel->property("experimentTimeBasis").toString()},
                {"topics", QJsonArray::fromVariantList(state->panel->property("indexTopics").toList())},
                {"sync_error", state->panel->property("syncError").toString()},
                {"signature", state->panel->property("experimentSignature").toString()},
                {"pending_signature", state->panel->property("pendingSignature").toString()}} : QJsonObject{}},
            {"successful_native_frame_count", state->successfulNativeFrames}, {"expected_successful_native_frames", expectedSuccessfulFrames},
            {"limitations", state->multiview
                ? "Actual main application canvas, three native camera Images, OGRE RenderTexture and short composed MP4. Programmatic controls do not certify manual interaction, long-run speed, per-view Display visibility/materials, transparent native output, tiling or third-party worker completion."
                : "Actual native OGRE RenderTexture, real application canvas and short MP4 output. Programmatic controls do not certify manual interaction, long-run speed, native independent multi-view, transparent native output or third-party worker completion."}};
        QFile file(QDir(directory).filePath("native-scene-acceptance.json"));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        client.stop();
        QTimer::singleShot(0, &app, [&, state, saved] { app.exit(saved && state->valid ? 0 : 1); });
    };
    const auto beginCase = [&, state, directory](int step) {
        replay.pause(); replay.seek(step % 2 ? 2.125 : 6.4);
        if (step == 0 || step == 6 || step == 7) replay.togglePlayback();
        state->previousPosition = replay.position(); state->previousPlaying = replay.playing();
        state->captureOptions = state->canvas->property("captureOptions");
        state->captureSizes.clear();
        for (auto *item : visualItems(state->canvas))
            if (auto *cloud = qobject_cast<CloudView *>(item)) state->captureSizes.append({cloud, cloud->captureSize()});
        state->frameCount = state->heartbeats = 0; state->cancelQueued = false; state->jobTime.start(); state->waiting = true;
        state->caseNativeFrames = 0; state->caseValid = true; state->nativeKeys.clear(); state->frameInspections = {};
        state->caseSceneRevision.clear();
        state->caseViewIds = step == 0 ? QStringList{state->viewIds.last()} : state->viewIds;
        if (state->multiview && step == 4) state->caseViewIds.removeLast();
        state->outputSize = QSize(step == 2 ? 3840 : step == 3 ? 7680 : 1920,
                                 step == 2 ? 2160 : step == 3 ? 4320 : 1080);
        const QStringList names{"selected-1080.png", "canvas-1080.png", "canvas-4k.png", "canvas-8k.png", "layers-1080.png",
            "native-two-frames.mp4", "cancelled.png", "missing-target.png", "transparent-rejected.png", "restored.png"};
        state->currentFile = names[step];
        const auto destination = QUrl::fromLocalFile(QDir(directory).filePath(state->currentFile));
        if (step == 5) {
            state->movieFrames.clear(); state->movieTimes.clear();
            movie.start(destination, state->canvas, state->time, state->time + .2, 1., -1, 1920, 1080, 10, 0);
            return;
        }
        QVariantMap options{{"width", state->outputSize.width()}, {"height", state->outputSize.height()},
            {"dpi", step == 3 ? 1200 : 300}, {"scope", step == 0 ? "view" : step == 4 ? "layers" : "canvas"},
            {"viewId", state->caseViewIds.first()}};
        if (step == 4) options.insert("layerIds", state->caseViewIds + QStringList{state->textId, "speed"});
        if (step == 8) options.insert("background", "transparent");
        movie.startStill(destination, state->canvas, state->time, options);
    };
    QObject::connect(&replay, &ReplayController::stateChanged, &app, [&, state] {
        if (state->recording && replay.loading()) ++state->loads;
    });
    QObject::connect(&client, &NativeRvizClient::commandSent, &app, [&, state](const QString &command) {
        if (command == "bag.index") ++state->indexRequests;
        if (!state->multiview && state->waiting && state->step == 6 && !state->cancelQueued && command == "bag.seek") {
            state->cancelQueued = true;
            QTimer::singleShot(0, &app, [&movie] { movie.cancel(); });
        }
    });
    QObject::connect(&client, &NativeRvizClient::commandFinished, &app,
        [state](const QString &command, bool success, const QString &error) {
        if (command != "views.sync") return;
        if (success) ++state->syncAcks;
        else if (state->multiview) state->error = "Actual native view synchronization failed: " + error;
    });
    QObject::connect(&client, &NativeRvizClient::outputViewFramePrepared, &app,
        [&, state, directory](const QString &, const QString &viewId, const QString &target, const QImage &frame, const QVariantMap &metadata) {
        if (!state->waiting) return;
        const int frameIndex = state->caseViewIds.isEmpty() ? 0 : state->caseNativeFrames / state->caseViewIds.size();
        const double expectedTime = state->time + (state->step == 5 ? frameIndex / 10. : 0.);
        const auto expectedTarget = state->recording ? QString::number(state->recording->stampAt(expectedTime)) : QString();
        auto *item = state->nativeViews.value(viewId).data();
        auto *captureTarget = state->step == 0 ? state->nativeViews.value(state->caseViewIds.first()).data() : state->canvas.data();
        const double scale = captureTarget && captureTarget->width() > 0.
            ? double(captureTarget->size().toSize().scaled(state->outputSize, Qt::KeepAspectRatio).width()) / captureTarget->width() : 0.;
        const QSize expectedSize = item ? QSize(qCeil(item->width() * scale), qCeil(item->height() * scale)) : QSize();
        const auto revision = metadata.value("scene_revision").toString();
        const auto key = target + '/' + viewId;
        const bool content = nativeHasContent(frame, QColor(metadata.value("background_color").toString()));
        const bool exact = item && item->isVisible() && item->property("nativeSceneImage").toBool()
            && state->caseViewIds.contains(viewId) && !state->nativeKeys.contains(key)
            && !expectedTarget.isEmpty() && target == expectedTarget && replay.experimentStampNs() == expectedTarget
            && metadata.value("view_id").toString() == viewId && metadata.value("target_ns").toString() == expectedTarget
            && metadata.value("clock_ns").toString() == expectedTarget && metadata.value("native_render_texture").toBool()
            && metadata.value("native_scene_ready").toBool() && metadata.value("required_tf_complete").toBool()
            && metadata.value("experiment_playhead_synchronized").toBool() && !revision.isEmpty()
            && (state->caseSceneRevision.isEmpty() || revision == state->caseSceneRevision)
            && scale > 0. && frame.size() == expectedSize && frame.width() == metadata.value("width").toInt()
            && frame.height() == metadata.value("height").toInt()
            && metadata.value("gpu_max_texture_size").toInt() >= std::max(frame.width(), frame.height());
        state->caseSceneRevision = revision; state->nativeKeys.insert(key);
        ++state->nativeFrames; ++state->caseNativeFrames; state->caseValid &= content && exact;
        state->nativeOutputs.append(QJsonObject{{"case", state->currentFile}, {"success", true}, {"view_id", viewId}, {"target_ns", target},
            {"metadata", QJsonObject::fromVariantMap(metadata)}, {"actual_width", frame.width()}, {"actual_height", frame.height()},
            {"expected_width", expectedSize.width()}, {"expected_height", expectedSize.height()},
            {"expected_target_ns", expectedTarget}, {"exact_identity_time_geometry", exact},
            {"actual_pixel_sha256", pixelHash(frame)}, {"non_background_content", content}});
        if (state->step == 0) state->caseValid &= frame.save(QDir(directory).filePath("native-source-1080.png"));
        if (state->multiview && state->step == 1)
            state->caseValid &= frame.save(QDir(directory).filePath("native-source-" + viewId + "-canvas-1080.png"));
        // Cancel the actual three-camera batch after the first real offscreen
        // frame, before it can publish a composed source or render another view.
        if (state->multiview && state->step == 6 && !state->cancelQueued) {
            state->cancelQueued = true; movie.cancel();
        }
    });
    QObject::connect(&movie, &MovieExporter::frameCaptured, &app, [&, state](const QImage &image, double time) {
        if (!state->waiting) return;
        ++state->frameCount;
        const double expected = state->time + (state->step == 5 ? (state->frameCount - 1) / 10. : 0.);
        const auto expectedTarget = state->recording ? QString::number(state->recording->stampAt(expected)) : QString();
        const auto sources = movie.nativeFrameSources();
        const auto metrics = movie.captureMetrics();
        bool exact = movie.nativeFrameActive() && std::abs(time - expected) < 1.e-9
            && sources.size() == state->caseViewIds.size() && metrics.size() == state->caseViewIds.size()
            && state->caseNativeFrames == state->frameCount * state->caseViewIds.size();
        QJsonArray sourceInspection; QSet<QString> metricIds;
        for (const auto &id : state->viewIds) {
            const auto item = state->nativeViews.value(id);
            const auto source = sources.value(id).toUrl();
            const bool included = state->caseViewIds.contains(id);
            const bool matched = item && (included
                ? !source.isEmpty() && source.scheme() == "image" && source.host() == "movie"
                    && item->property("source").toUrl() == source && item->property("status").toInt() == 1
                : !sources.contains(id) && item->property("source").toUrl().isEmpty());
            exact &= matched;
            sourceInspection.append(QJsonObject{{"view_id", id}, {"included", included}, {"matched_exact_source", matched},
                {"source", source.toString()}, {"actual_source", item ? item->property("source").toUrl().toString() : QString()},
                {"image_status", item ? item->property("status").toInt() : -1}});
        }
        for (const auto &entry : metrics) {
            const auto metric = entry.toMap(); const auto id = metric.value("view").toString();
            exact &= state->caseViewIds.contains(id) && !metricIds.contains(id)
                && metric.value("view_id").toString() == id && metric.value("target_ns").toString() == expectedTarget
                && metric.value("clock_ns").toString() == expectedTarget
                && metric.value("scene_revision").toString() == state->caseSceneRevision
                && metric.value("native_render_texture").toBool() && metric.value("required_tf_complete").toBool()
                && metric.value("experiment_playhead_synchronized").toBool();
            metricIds.insert(id);
        }
        state->caseValid &= exact;
        state->frameInspections.append(QJsonObject{{"experiment_time", time}, {"target_ns", expectedTarget},
            {"exact_sources_and_metrics", exact}, {"sources", sourceInspection}, {"metrics", QJsonArray::fromVariantList(metrics)}});
        if (state->step == 5) { state->movieFrames.append(image); state->movieTimes.append(time); }
    });
    QObject::connect(&movie, &MovieExporter::finished, &app,
        [&, state, directory, intact](bool success, const QString &message) {
        if (!state->waiting) return;
        bool restored = intact() && std::abs(replay.position() - state->previousPosition) < 1.e-9
            && replay.playing() == state->previousPlaying && optionValues(state->canvas->property("captureOptions")) == optionValues(state->captureOptions)
            && !movie.nativeFrameActive() && movie.nativeFrameUrl().isEmpty() && movie.nativeFrameSources().isEmpty();
        for (const auto &[view, size] : state->captureSizes) restored &= view && view->captureSize() == size;
        const auto path = QDir(directory).filePath(state->currentFile);
        const bool failure = state->step >= 6 && state->step <= 8;
        bool valid = state->caseValid && restored
            && (failure ? !success && !QFileInfo::exists(path) && state->frameCount == 0
                        : success && state->caseNativeFrames == state->caseViewIds.size() * (state->step == 5 ? 2 : 1));
        QJsonObject extra;
        extra.insert("restoration", QJsonObject{{"source_and_edits", intact()},
            {"position", replay.position()}, {"expected_position", state->previousPosition},
            {"playing", replay.playing()}, {"expected_playing", state->previousPlaying},
            {"capture_options_equal", state->canvas->property("captureOptions") == state->captureOptions},
            {"capture_options_values_equal", optionValues(state->canvas->property("captureOptions")) == optionValues(state->captureOptions)},
            {"capture_options_before", QJsonObject::fromVariantMap(optionValues(state->captureOptions))},
            {"capture_options_after", QJsonObject::fromVariantMap(optionValues(state->canvas->property("captureOptions")))},
            {"capture_options_before_type", QString::fromLatin1(state->captureOptions.metaType().name())},
            {"capture_options_after_type", QString::fromLatin1(state->canvas->property("captureOptions").metaType().name())},
            {"native_frame_active", movie.nativeFrameActive()}, {"native_frame_url_empty", movie.nativeFrameUrl().isEmpty()},
            {"native_frame_sources_empty", movie.nativeFrameSources().isEmpty()}});
        if (state->step == 6) valid &= state->cancelQueued
            && (message.contains("cancel", Qt::CaseInsensitive) || message.contains("取消"))
            && state->caseNativeFrames == (state->multiview ? 1 : 0);
        if (state->step == 7) valid &= (message.contains("TF", Qt::CaseInsensitive) || message.contains("target", Qt::CaseInsensitive)
            || message.contains("frame", Qt::CaseInsensitive));
        if (state->step == 8) valid &= message.contains("Transparent native", Qt::CaseInsensitive) && state->caseNativeFrames == 0;
        if (!failure && state->jobTime.elapsed() >= 100) valid &= state->heartbeats > 0;
        if (!failure && state->step != 5) {
            const QImage image(path);
            const auto expected = state->outputSize;
            const int dpi = state->step == 3 ? 1200 : 300;
            const int magenta = colorPixels(image, QColor("#ed58d8"));
            valid &= image.size() == expected && state->frameCount == 1
                && std::abs(image.dotsPerMeterX() * .0254 - dpi) < .1
                && (state->step == 0 ? magenta < 5 : magenta > 20);
            extra.insert("width", image.width()); extra.insert("height", image.height()); extra.insert("text_color_pixels", magenta);
            extra.insert("actual_pixel_sha256", pixelHash(image));
            if (state->step == 1) state->canvasReference = image;
            if (state->step == 4 || state->step == 9) {
                const auto difference = imageDifference(state->canvasReference, image);
                valid &= difference.value("valid").toBool() && (state->multiview && state->step == 4
                    ? difference.value("maximum").toInt() > 1 : difference.value("maximum").toInt(255) <= 1);
                extra.insert("baseline_difference", difference);
            }
        }
        if (state->step == 5 && success) {
            QJsonArray encoded;
            try {
                replay::VideoReader reader(path); int index = 0;
                while (reader.advance() && index < 3) {
                    const auto image = replay::imageOf(reader.frame);
                    const auto difference = index < state->movieFrames.size()
                        ? imageDifference(state->movieFrames[index], image) : QJsonObject();
                    valid &= difference.value("valid").toBool() && difference.value("mae").toDouble(255.) <= 3.
                        && difference.value("psnr_db").toDouble() >= 30.;
                    encoded.append(difference); ++index;
                }
                valid &= index == 2 && state->movieFrames.size() == 2 && state->frameCount == 2;
                extra.insert("decoded_frames", index); extra.insert("encoded_differences", encoded);
                if (!state->movieFrames.isEmpty()) {
                    const auto sameTime = imageDifference(state->canvasReference, state->movieFrames.front());
                    valid &= sameTime.value("valid").toBool() && sameTime.value("maximum").toInt(255) <= 1;
                    extra.insert("still_movie_same_time_difference", sameTime);
                }
            } catch (const std::exception &error) { valid = false; extra.insert("decode_error", QString::fromUtf8(error.what())); }
        }
        if (!failure && success) state->successfulNativeFrames += state->caseNativeFrames;
        state->valid &= valid;
        state->cases.append(QJsonObject{{"file", state->currentFile}, {"passed", valid}, {"success", success},
            {"restored", restored}, {"experiment_time", state->time}, {"elapsed_ms", state->jobTime.elapsed()},
            {"ui_heartbeats", state->heartbeats}, {"frame_callbacks", state->frameCount}, {"message", message},
            {"expected_native_view_ids", QJsonArray::fromStringList(state->caseViewIds)}, {"native_frames", state->caseNativeFrames},
            {"frame_inspections", state->frameInspections},
            {"render_metrics", QJsonArray::fromVariantList(movie.captureMetrics())}, {"inspection", extra}});
        state->waiting = false;
        if (!valid) state->error = "Native canvas export failed: " + state->currentFile + ": " + message;
        ++state->step;
    });
    QObject::connect(timer, &QTimer::timeout, &app,
        [&, state, directory, bag, config, launcher, distribution, finish, beginCase] {
        if (state->deadline.elapsed() - state->lastProgressAt >= 5000) {
            state->lastProgressAt = state->deadline.elapsed();
            QFile progress(QDir(directory).filePath("progress.json"));
            if (progress.open(QIODevice::WriteOnly)) progress.write(QJsonDocument(QJsonObject{
                {"phase", state->phase}, {"step", state->step}, {"elapsed_ms", state->deadline.elapsed()},
                {"connected", client.connected()}, {"running", client.running()}, {"busy", client.busy()},
                {"status", client.status()}, {"diagnostic", client.diagnostic()},
                {"playhead_ns", replay.experimentStampNs()}, {"native_target_ns", client.targetNs()}}).toJson());
        }
        if (state->finished || state->waiting || movie.exporting()) return;
        if (!state->error.isEmpty()) { finish(state->error); return; }
        if (state->deadline.elapsed() > 900000) { finish("Native scene workflow timed out: " + client.status()); return; }
        if (state->phase == 0) {
            state->panel = namedItem(window, "nativeRvizInspector");
            if (!state->panel) { finish("Actual native inspector not found"); return; }
            state->panel->setProperty("autoAttachSource", false); state->panel->setProperty("linkPausedFrame", false);
            if (auto *section = namedItem(window, "settingsSection")) section->setProperty("currentIndex", 5);
            replay.openBag(QUrl::fromLocalFile(bag));
            client.start(QUrl::fromLocalFile(launcher), distribution, QUrl::fromLocalFile(config));
            state->phase = 1; return;
        }
        if (state->phase == 1) {
            if (state->deadline.elapsed() > 2000 && !client.running() && !client.connected()) {
                finish("Native host did not remain running: " + client.status()); return;
            }
            if (replay.loading() || !client.connected() || client.properties().isEmpty()) return;
            replay.setRenderBackend("native-rviz");
            if (replay.demo() || replay.duration() < state->time + .2) { finish("Actual bag and sufficient experiment coverage are required"); return; }
            state->canvas = namedItem(window, "previewCanvas"); state->nativeView = namedItem(window, "nativeMainView");
            if (!state->canvas || !state->nativeView || !state->canvas->property("nativeSceneActive").toBool()) {
                finish("Actual main QML canvas has no connected native scene"); return;
            }
            if (!replay.cloudViews().isEmpty()) { finish("Acceptance requires a fresh project without pre-existing inset views"); return; }
            replay.pause(); replay.setTransparentCanvas(false);
            replay.setMainView({{"width", .68}, {"x", 1.}, {"y", .05}});
            replay.setSpeedGauge({{"visible", true}, {"x", .015}, {"y", .4}, {"scale", 1.}});
            replay.addSceneText("Native scene · 原生场景 8.025 s");
            state->textId = replay.sceneLayers().last().toMap().value("id").toString();
            replay.editSceneLayer(state->textId, {{"color", "#ed58d8"}, {"position", QJsonArray{.01, .01}},
                {"start", 0.}, {"end", replay.duration()}});
            state->nativeViews.insert("main", state->nativeView);
            if (state->multiview) {
                state->coordinator = state->canvas->property("nativeViewCoordinator").value<QObject *>();
                if (!state->coordinator || !client.independentCameras()) {
                    finish("The actual canvas coordinator and independent native cameras are required"); return;
                }
                replay.addCloudView(1); replay.addCloudView(2);
                if (replay.cloudViews().size() != 2) { finish("Actual project did not create two independent camera layers"); return; }
                for (int index = 0; index < 2; ++index) {
                    const auto id = replay.cloudViews()[index].toMap().value("id").toString();
                    if (id.isEmpty() || state->viewIds.contains(id)) { finish("Actual camera layers have invalid identities"); return; }
                    state->viewIds.append(id);
                    replay.setCloudView(id, {{"width", index == 0 ? .29 : .31}, {"x", .015}, {"y", index == 0 ? .07 : .83}});
                }
            }
            state->recording = replay.recording(); state->layers = replay.sceneLayers(); state->views = replay.cloudViews();
            state->markers = replay.markers(); state->mainView = replay.mainView(); state->gauge = replay.speedGauge();
            state->phase = 20; return;
        }
        if (state->phase == 20) {
            if (client.busy()) return;
            if (!state->panel->property("syncError").toString().isEmpty()) {
                finish("Actual native index refused its experiment mapping: " + state->panel->property("syncError").toString()); return;
            }
            if (state->panel->property("experimentBag").toUrl().isEmpty()
                || state->panel->property("indexTopics").toList().isEmpty()) return;
            if (!QMetaObject::invokeMethod(state->panel, "indexExperiment")) { finish("Actual inline index action is unavailable"); return; }
            if (state->indexRequests > 0) state->phase = 2;
            return;
        }
        if (state->phase == 2) {
            if (client.busy()) return;
            if (!client.indexed()) { finish("Actual native Header index did not become ready: " + client.status()); return; }
            state->phase = state->multiview ? 10 : 3;
        }
        if (client.busy()) return;
        if (state->phase == 10 || state->phase == 11) {
            if (!state->coordinator) { finish("Actual native camera coordinator was deleted"); return; }
            if (!state->coordinator->property("syncError").toString().isEmpty()) {
                finish("Actual native camera synchronization failed: " + state->coordinator->property("syncError").toString()); return;
            }
            if (state->coordinator->property("syncPending").toBool() || client.renderViews().size() != 2 || state->syncAcks == 0) return;
            QSet<QString> actualIds;
            bool classesMatch = true;
            for (const auto &entry : client.renderViews()) {
                const auto view = entry.toMap(); const auto id = view.value("view_id").toString();
                actualIds.insert(id);
                classesMatch &= view.value("class").toString() == (state->phase == 11 && id == state->viewIds[2]
                    ? "rviz/TopDownOrtho" : "rviz/Orbit");
            }
            if (!classesMatch || actualIds != QSet<QString>{state->viewIds[1], state->viewIds[2]}) return;
            if (state->phase == 10) {
                // Exercise the actual UI coordinator and wait for a second
                // successful host acknowledgement of the real plugin change.
                state->initialSyncAcks = state->syncAcks;
                if (!QMetaObject::invokeMethod(state->coordinator, "setViewClass", Q_ARG(QVariant, QVariant(state->viewIds[2])),
                                               Q_ARG(QVariant, QVariant(QStringLiteral("rviz/TopDownOrtho"))))) {
                    finish("Actual native coordinator cannot change the additional camera plugin"); return;
                }
                state->phase = 11; return;
            }
            if (state->syncAcks <= state->initialSyncAcks) return;
            QMap<QString, QPointer<QQuickItem>> nativeViews;
            for (auto *item : visualItems(state->canvas)) {
                if (!item->isVisible()) continue;
                if (qobject_cast<CloudView *>(item)) { finish("Actual native canvas contains a visible legacy camera substitute"); return; }
                if (!item->property("nativeSceneImage").toBool()) continue;
                const auto id = item->property("exportViewId").toString();
                if (!state->viewIds.contains(id) || nativeViews.contains(id)) { finish("Actual native canvas has an invalid or duplicate camera Image"); return; }
                nativeViews.insert(id, item);
            }
            if (nativeViews.size() != 3) return;
            state->nativeViews = nativeViews; state->phase = 3;
        }
        if (state->phase == 4) {
            client.setSearch("Target Frame");
            for (const auto &entry : client.properties()) {
                const auto property = entry.toMap();
                if (property.value("name").toString() == "Target Frame" && property.value("display_id").toString().isEmpty()
                    && property.value("view_id").toString() == (state->multiview ? state->viewIds[1] : QStringLiteral("main"))) {
                    state->targetPropertyId = property.value("id").toString(); state->originalTarget = property.value("value"); break;
                }
            }
            if (state->targetPropertyId.isEmpty()) { finish("Actual native camera Target Frame property is unavailable"); return; }
            client.setPropertyValue(state->targetPropertyId, "__uav_native_missing_target__"); state->phase = 5; return;
        }
        if (state->phase == 5 || state->phase == 6) {
            bool matched = false;
            for (const auto &entry : client.properties()) {
                const auto property = entry.toMap();
                if (property.value("id").toString() == state->targetPropertyId)
                    matched = property.value("value").toString() == (state->phase == 5
                        ? QStringLiteral("__uav_native_missing_target__") : state->originalTarget.toString());
            }
            if (!matched) return;
            state->phase = 3;
        }
        if (state->step >= 10) {
            if (state->phase != 7) {
                client.setSearch({}); replay.pause(); replay.seek(state->time);
                client.seekAbsolute(replay.experimentStampNs()); state->phase = 7; return;
            }
            if (!client.synchronized() || !client.sceneReady() || client.preview().isEmpty()
                || client.targetNs() != replay.experimentStampNs() || client.clockNs() != replay.experimentStampNs()
                || state->nativeView->property("source").toUrl() != QUrl(client.preview())
                || state->nativeView->property("status").toInt() != 1) return;
            if (state->multiview) for (const auto &id : state->viewIds) {
                const auto image = state->nativeViews.value(id);
                const auto preview = client.viewPreviews().value(id).toMap();
                if (!image || image->property("status").toInt() != 1 || !preview.value("synchronized").toBool()
                    || preview.value("view_id").toString() != id || preview.value("clock_ns").toString() != replay.experimentStampNs()
                    || preview.value("synchronized_target_ns").toString() != replay.experimentStampNs()
                    || image->property("source").toUrl() != preview.value("source").toUrl()) return;
            }
            bool localPreviewsMatch = true;
            for (const auto &id : state->viewIds) {
                const auto image = state->nativeViews.value(id);
                const auto *view = image ? image->parentItem() : nullptr;
                auto preview = client.viewPreviews().value(id).toMap();
                preview.remove("source");
                QVariantMap localPreview = view ? view->property("previewFrame").toMap() : QVariantMap{};
                localPreview.remove("source");
                const bool matched = view && view->property("matchesPlayhead").toBool();
                localPreviewsMatch &= matched;
                state->previewDiagnostics.append(QJsonObject{{"view_id", id}, {"matched", matched},
                    {"experiment_ns", replay.experimentStampNs()},
                    {"canvas_target_ns", state->canvas->property("nativeTargetNs").toString()},
                    {"view_target_ns", view ? view->property("targetNs").toString() : QString()},
                    {"local_frame", QJsonObject::fromVariantMap(localPreview)},
                    {"client_frame", QJsonObject::fromVariantMap(preview)}});
            }
            if (!localPreviewsMatch) { finish("Actual native view did not match its current playhead metadata"); return; }
            state->valid &= window.grabWindow().save(QDir(directory).filePath("native-main-canvas.png"));
            finish({}); return;
        }
        if (state->step == 7 && state->targetPropertyId.isEmpty()) { state->phase = 4; return; }
        if (state->step == 8 && !state->targetPropertyId.isEmpty() && state->phase == 3) {
            // Restore the real camera before the separate transparency rejection.
            bool original = false;
            for (const auto &entry : client.properties()) {
                const auto property = entry.toMap();
                if (property.value("id").toString() == state->targetPropertyId) original = property.value("value") == state->originalTarget;
            }
            if (!original) { client.setPropertyValue(state->targetPropertyId, state->originalTarget); state->phase = 6; return; }
        }
        beginCase(state->step);
    });
    timer->start();
    QTimer::singleShot(900000, &app, [finish] { finish("Native scene acceptance exceeded its deadline"); });
}
