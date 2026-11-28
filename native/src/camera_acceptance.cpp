#include "camera_acceptance.h"

#include "cloud_view.h"
#include "movie_exporter.h"
#include "transform_tree.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
int maximumChannelDelta(const QImage &first, const QImage &second)
{
    if (first.isNull() || first.size() != second.size()) return 255;
    const auto a = first.convertToFormat(QImage::Format_RGBA8888);
    const auto b = second.convertToFormat(QImage::Format_RGBA8888);
    int maximum = 0;
    for (int row = 0; row < a.height(); ++row)
        for (int column = 0; column < a.width() * 4; ++column)
            maximum = std::max(maximum, std::abs(int(a.constScanLine(row)[column]) - int(b.constScanLine(row)[column])));
    return maximum;
}

int colorPixels(const QImage &image, const QColor &color)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - color.red()) < 6 && std::abs(pixel.green() - color.green()) < 6
                && std::abs(pixel.blue() - color.blue()) < 6 && pixel.alpha() > 240) ++count;
        }
    return count;
}

bool different(const QImage &first, const QImage &second)
{
    return !first.isNull() && !second.isNull() && maximumChannelDelta(first, second) > 4;
}

template <typename Predicate>
QQuickItem *visualItemFor(QQuickItem *root, const Predicate &matches)
{
    if (!root) return nullptr;
    if (matches(root)) return root;
    // Repeater delegates belong to the visual tree, but need not be QObject
    // descendants of the window. Use the same tree as actual frame export.
    for (auto *child : root->childItems())
        if (auto *found = visualItemFor(child, matches)) return found;
    return nullptr;
}

CloudView *viewFor(QQuickWindow &window, const QString &id)
{
    return qobject_cast<CloudView *>(visualItemFor(window.contentItem(), [&id](QQuickItem *item) {
        return qobject_cast<CloudView *>(item) && item->property("exportViewId").toString() == id;
    }));
}

QQuickItem *namedItem(QQuickWindow &window, const QString &name)
{
    return visualItemFor(window.contentItem(), [&name](QQuickItem *item) { return item->objectName() == name; });
}

QJsonObject viewState(const CloudView *view)
{
    if (!view) return {{"exists", false}};
    return {{"exists", true}, {"visible", view->isVisible()}, {"width", view->width()}, {"height", view->height()},
        {"target_frame", view->targetFrame()}, {"camera", view->camera()}, {"follow_mode", view->followMode()},
        {"following", view->followingTarget()}, {"status", view->cameraStatus()}, {"texture_limit", view->textureLimit()},
        {"framebuffer_width", view->framebufferSize().width()}, {"framebuffer_height", view->framebufferSize().height()}};
}
}

void runCameraAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                         MovieExporter &movie, const QString &directory, const QString &bag)
{
    struct State {
        int phase = 0, loadSignals = 0, frameCaptures = 0, renderAttempts = 0, rejectedExports = 0;
        bool valid = true, awaiting = false, completed = false, diagnosticsHidden = true;
        bool targetFollowing = false, missingDiagnosed = false, playbackPreserved = false;
        bool expectedSuccess = true, previousPlaying = false;
        double time = 0, requestTime = 0, previousPosition = 0;
        std::array<QSize, 3> captureSizes;
        QString target, firstId, secondId, currentFile, error;
        QPointer<CloudView> main, first, second;
        QPointer<QQuickItem> canvas;
        std::shared_ptr<const replay::Recording> recording;
        QVariantList markers, layers, views;
        QVariantMap mainSettings;
        QHash<QString, QImage> images;
        QJsonArray cases, targetChecks;
    };
    auto state = std::make_shared<State>();
    QDir().mkpath(directory);
    window.resize(1440, 900);
    auto *timer = new QTimer(&app);
    timer->setInterval(350);
    const auto intact = [&, state] {
        return state->recording && replay.recording() == state->recording && !replay.loading()
            && state->main && state->canvas
            && viewFor(window, "main") == state->main
            && namedItem(window, "previewCanvas") == state->canvas
            && (!state->first || viewFor(window, state->firstId) == state->first)
            && (!state->second || viewFor(window, state->secondId) == state->second);
    };
    const auto finish = [&, state, timer, directory, intact](const QString &error) {
        if (state->completed) return;
        state->completed = true;
        timer->stop();
        if (!error.isEmpty()) { state->error = error; state->valid = false; }
        const int restoreDelta = maximumChannelDelta(state->images.value("target-main.png"), state->images.value("restored-main.png"));
        const int seekDelta = maximumChannelDelta(state->images.value("target-main.png"), state->images.value("random-restored-main.png"));
        const int isolatedDelta = maximumChannelDelta(state->images.value("view-b-blue.png"), state->images.value("view-b-after-a-hidden.png"));
        const int insetRestoreDelta = maximumChannelDelta(state->images.value("view-a-red.png"), state->images.value("view-a-restored.png"));
        const int red = colorPixels(state->images.value("view-a-red.png"), QColor("#e0654f"));
        const int blue = colorPixels(state->images.value("view-b-blue.png"), QColor("#4f91e0"));
        const bool targetChangesPixels = different(state->images.value("target-main.png"), state->images.value("world-main.png"));
        const bool independentColor = red > 10 && blue > 10;
        const bool independentVisibility = different(state->images.value("view-a-red.png"), state->images.value("view-a-hidden.png"))
            && isolatedDelta <= 1 && insetRestoreDelta <= 1;
        const bool snapshots = replay.markers() == state->markers && replay.sceneLayers() == state->layers
            && replay.cloudViews() == state->views && replay.mainView() == state->mainSettings;
        state->valid &= state->error.isEmpty() && intact() && !replay.demo() && state->loadSignals == 0
            && state->targetFollowing && state->missingDiagnosed && state->diagnosticsHidden
            && state->playbackPreserved && targetChangesPixels && independentColor && independentVisibility
            && restoreDelta <= 1 && seekDelta <= 1 && snapshots && state->cases.size() == 16 && state->frameCaptures == 9
            && state->rejectedExports == 7
            && window.rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL;
        const QJsonObject report{
            {"passed", state->valid}, {"real_bag", !replay.demo()}, {"point_count", replay.pointCount()},
            {"renderer", window.rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL ? "OpenGL" : "Other"},
            {"window_visible", window.isVisible()}, {"window_exposed", window.isExposed()},
            {"render_attempts", state->renderAttempts},
            {"auxiliary_view_count", replay.cloudViews().size()},
            {"view_a_id", state->firstId}, {"view_b_id", state->secondId},
            {"main_view", viewState(state->main)}, {"view_a", viewState(state->first)}, {"view_b", viewState(state->second)},
            {"target_frame", state->target}, {"fixed_frame", replay.worldFrame()}, {"experiment_time", state->time},
            {"duration", replay.duration()}, {"body_frame", replay.bodyFrame()},
            {"transform_frames", QJsonArray::fromStringList(state->recording && state->recording->transforms
                ? state->recording->transforms->frames() : QStringList())},
            {"odometry_pose_count", state->recording ? int(state->recording->poses.size()) : 0},
            {"target_checks", state->targetChecks},
            {"experiment_stamp_ns", state->recording ? QString::number(state->recording->stampAt(state->time)) : QString()},
            {"target_following", state->targetFollowing}, {"target_changes_rendered_pixels", targetChangesPixels},
            {"missing_tf_diagnosed", state->missingDiagnosed}, {"diagnostics_hidden_during_export", state->frameCaptures > 0 && state->diagnosticsHidden},
            {"restore_max_channel_delta", restoreDelta}, {"random_seek_restore_max_channel_delta", seekDelta},
            {"independent_color", independentColor}, {"red_cloud_pixels", red}, {"blue_cloud_pixels", blue},
            {"independent_visibility", independentVisibility}, {"other_view_max_channel_delta", isolatedDelta},
            {"inset_restore_max_channel_delta", insetRestoreDelta}, {"recording_and_view_objects_preserved", intact()},
            {"editing_state_preserved", snapshots}, {"playback_preserved", state->playbackPreserved},
            {"loading_signals_during_camera_edits", state->loadSignals}, {"export_frame_callbacks", state->frameCaptures},
            {"missing_target_exports_rejected", state->rejectedExports},
            {"cases", state->cases}, {"error", state->error},
            {"limitations", "Programmatic controller actions and real OpenGL/offscreen output; not manual GUI interaction, native RViz parity, long-run performance or video PTS acceptance."}
        };
        QFile file(QDir(directory).filePath("camera-acceptance.json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0) state->valid = false;
        app.exit(state->valid ? 0 : 1);
    };
    const auto capture = [&, state, directory](const QString &name, const QString &viewId, bool canvas = false,
                                              bool expectedSuccess = true, double time = -1., bool asMovie = false) {
        state->currentFile = name;
        state->expectedSuccess = expectedSuccess;
        state->requestTime = time < 0. ? state->time : time;
        state->previousPosition = replay.position(); state->previousPlaying = replay.playing();
        state->captureSizes = {state->main->captureSize(), state->first->captureSize(), state->second->captureSize()};
        state->awaiting = true;
        if (asMovie) {
            movie.start(QUrl::fromLocalFile(QDir(directory).filePath(name)), state->canvas,
                state->requestTime, std::min(replay.duration(), state->requestTime + .1), 1., -1, 320, 240);
            return;
        }
        movie.startStill(QUrl::fromLocalFile(QDir(directory).filePath(name)), state->canvas, state->requestTime,
            {{"width", 960}, {"height", 540}, {"dpi", 300}, {"background", replay.sceneBackground().name()},
             {"scope", canvas ? "canvas" : "view"}, {"viewId", viewId}, {"includeText", false}, {"includeGauge", false}});
    };
    QObject::connect(&replay, &ReplayController::stateChanged, &app, [&, state] {
        if (state->phase > 0 && replay.loading()) ++state->loadSignals;
    });
    QObject::connect(&movie, &MovieExporter::frameCaptured, &app, [&, state](const QImage &, double time) {
        if (!state->awaiting) return;
        ++state->frameCaptures;
        if (!state->expectedSuccess) state->valid = false;
        const auto *mainStatus = namedItem(window, "cameraStatusMessage");
        const auto *insetStatus = namedItem(window, "insetCameraStatus-" + state->firstId);
        state->diagnosticsHidden &= mainStatus && insetStatus && !mainStatus->isVisible() && !insetStatus->isVisible()
            && std::abs(time - state->requestTime) < 1.e-9;
    });
    QObject::connect(&movie, &MovieExporter::finished, &app, [&, state, directory, intact](bool success, const QString &message) {
        if (!state->awaiting) return;
        const auto path = QDir(directory).filePath(state->currentFile);
        const QImage image(path);
        const bool restored = std::abs(replay.position() - state->previousPosition) < 1.e-9
            && replay.playing() == state->previousPlaying && state->canvas->property("captureOptions").toMap().isEmpty()
            && state->main->captureSize() == state->captureSizes[0] && state->first->captureSize() == state->captureSizes[1]
            && state->second->captureSize() == state->captureSizes[2];
        const bool rejected = !success && !QFileInfo::exists(path) && message.contains("Camera target unavailable");
        const bool valid = intact() && restored && (state->expectedSuccess ? success && image.size() == QSize(960, 540) : rejected);
        if (!state->expectedSuccess && rejected) ++state->rejectedExports;
        state->valid &= valid;
        state->images.insert(state->currentFile, image);
        state->cases.append(QJsonObject{{"file", state->currentFile}, {"passed", valid}, {"message", message},
            {"width", image.width()}, {"height", image.height()}, {"time", state->requestTime},
            {"expected_success", state->expectedSuccess}, {"actual_success", success}, {"capture_state_restored", restored},
            {"framebuffers", QJsonArray::fromVariantList(movie.captureMetrics())}});
        state->awaiting = false;
    });
    QObject::connect(timer, &QTimer::timeout, &app,
        [&, state, directory, intact, finish, capture] {
        if (state->completed || state->awaiting || movie.exporting() || replay.loading()) return;
        if (state->phase > 0 && !intact()) { finish("Recording or viewport changed during camera edits"); return; }
        switch (state->phase++) {
        case 0: {
            state->recording = replay.recording();
            state->canvas = namedItem(window, "previewCanvas");
            state->main = viewFor(window, "main");
            if (replay.demo() || !state->canvas || !state->main || !state->recording->transforms) {
                finish("Real bag, actual viewports and recorded TF are required"); return;
            }
            // Use an actual non-world TF target with valid data at a sampled time.
            const auto checkTarget = [&, state](const QString &frame, double seconds) {
                const auto stamp = state->recording->stampAt(seconds);
                const auto target = state->recording->transforms->lookup(replay.worldFrame(), frame, stamp);
                const bool nonIdentity = target && (target->translation.lengthSquared() >= .001f
                    || std::abs(QQuaternion::dotProduct(target->rotation, QQuaternion())) <= .999f);
                state->targetChecks.append(QJsonObject{{"frame", frame}, {"time", seconds},
                    {"stamp_ns", QString::number(stamp)}, {"available", bool(target)}, {"non_identity", nonIdentity}});
                if (!nonIdentity) return false;
                state->time = seconds; state->target = frame; return true;
            };
            for (const double fraction : {.3, .5, .1, .7}) {
                const auto seconds = replay.duration() * fraction;
                for (const auto &frame : state->recording->transforms->frames()) {
                    if (frame == replay.worldFrame()) continue;
                    if (checkTarget(frame, seconds)) break;
                }
                if (!state->target.isEmpty()) break;
            }
            // Sparse TF need not cover any fractional timestamp. Try real recorded
            // TF samples inside the point-cloud experiment, without extrapolation.
            if (state->target.isEmpty() && !state->recording->frames.empty()) {
                const auto first = state->recording->frames.front().stamp;
                const auto last = state->recording->frames.back().stamp;
                for (const auto &frame : state->recording->transforms->frames()) {
                    if (frame == replay.worldFrame()) continue;
                    const auto samples = state->recording->transforms->sampleTimes(frame);
                    for (const auto stamp : samples) {
                        if (stamp < first || stamp > last) continue;
                        if (checkTarget(frame, double(stamp - first) / 1.e9)) break;
                    }
                    if (!state->target.isEmpty()) break;
                }
            }
            if (state->target.isEmpty()) { finish("No non-identity TF target is available in this real bag"); return; }
            replay.pause(); replay.seek(state->time); replay.setCamera(2);
            replay.setMainView({{"targetFrame", state->target}, {"followMode", 2}, {"smoothing", .25}, {"followLag", 0.}, {"cloudVisible", true}});
            replay.addCloudView(2);
            state->firstId = replay.cloudViews().last().toMap().value("id").toString();
            replay.addCloudView(2);
            state->secondId = replay.cloudViews().last().toMap().value("id").toString();
            const QVariantMap common{{"targetFrame", state->target}, {"followMode", 2}, {"smoothing", .25},
                {"followLag", 0.}, {"zoom", 1.}, {"width", .36}, {"pointSize", 2.}, {"transparent", false},
                {"overlays", QStringList()}, {"cloudVisible", true}, {"viewBackground", replay.sceneBackground().name()}};
            auto first = common; first.insert("x", 0.); first.insert("y", 0.); first.insert("cloudColor", "#e0654f");
            auto second = common; second.insert("x", 1.); second.insert("y", 1.); second.insert("cloudColor", "#4f91e0");
            replay.setCloudView(state->firstId, first); replay.setCloudView(state->secondId, second);
            state->markers = replay.markers(); state->layers = replay.sceneLayers();
            state->views = replay.cloudViews(); state->mainSettings = replay.mainView();
            window.setProperty("workspaceIndex", 0);
            return;
        }
        case 1:
            state->first = viewFor(window, state->firstId); state->second = viewFor(window, state->secondId);
            // QQuickFramebufferObject publishes status through a queued GUI update.
            // Ask Qt for a real frame, then allow that update a bounded time. This
            // neither unlocks the desktop nor substitutes a software/mock renderer.
            window.requestUpdate();
            if (const auto frame = window.grabWindow(); !frame.isNull())
                state->valid &= frame.save(QDir(directory).filePath("target-window.png"));
            ++state->renderAttempts;
            state->targetFollowing = state->first && state->second && state->main->followingTarget()
                && state->first->followingTarget() && state->second->followingTarget()
                && state->main->targetFrame() == state->target;
            if (!state->targetFollowing) {
                if (state->renderAttempts < 6) { state->phase = 1; return; }
                finish(!window.isExposed() && state->main->textureLimit() == 0
                    ? "Native OpenGL window is not exposed and no render context initialized; desktop/session rendering unavailable"
                    : "Real target did not reach rendered following state");
                return;
            }
            state->valid &= window.grabWindow().save(QDir(directory).filePath("editing-real-target.png"));
            capture("target-main.png", "main"); return;
        case 2:
            replay.setMainView({{"targetFrame", replay.worldFrame()}});
            capture("world-main.png", "main"); return;
        case 3:
            replay.setMainView({{"targetFrame", "__uav_studio_missing_tf__"}});
            replay.setCloudView(state->firstId, {{"targetFrame", "__uav_studio_missing_tf__"}});
            return;
        case 4: {
            const auto *mainStatus = namedItem(window, "cameraStatusMessage");
            const auto *insetStatus = namedItem(window, "insetCameraStatus-" + state->firstId);
            state->missingDiagnosed = !state->main->followingTarget() && !state->first->followingTarget()
                && state->main->cameraStatus().contains("No valid") && state->first->cameraStatus().contains("No valid")
                && mainStatus && mainStatus->isVisible() && insetStatus && insetStatus->isVisible();
            state->valid &= window.grabWindow().save(QDir(directory).filePath("editing-missing-tf.png"));
            capture("missing-tf-canvas.png", "main", true, false); return;
        }
        case 5:
            replay.setMainView({{"targetFrame", state->target}});
            replay.setCloudView(state->firstId, {{"targetFrame", state->target}}); return;
        case 6: capture("restored-main.png", "main"); return;
        case 7: replay.seek(replay.duration() * .8); return;
        case 8: replay.seek(replay.duration() * .12); return;
        case 9: replay.seek(state->time); return;
        case 10: capture("random-restored-main.png", "main"); return;
        case 11: capture("view-a-red.png", state->firstId); return;
        case 12: capture("view-b-blue.png", state->secondId); return;
        case 13: replay.setCloudView(state->firstId, {{"cloudVisible", false}}); return;
        case 14: capture("view-a-hidden.png", state->firstId); return;
        case 15: capture("view-b-after-a-hidden.png", state->secondId); return;
        case 16: replay.setCloudView(state->firstId, {{"cloudVisible", true}}); return;
        case 17: capture("view-a-restored.png", state->firstId); return;
        case 18:
            replay.togglePlayback();
            for (int i = 0; i < 5; ++i) {
                replay.setCloudView(state->firstId, {{"cloudVisible", false}});
                replay.setCloudView(state->firstId, {{"cloudVisible", true}});
            }
            return;
        case 19:
            state->playbackPreserved = replay.playing() && replay.position() > state->time;
            replay.pause(); replay.seek(state->time); return;
        case 20:
            replay.setMainView({{"targetFrame", "__uav_studio_missing_tf__"}});
            capture("rejected-main.png", "main", false, false); return;
        case 21:
            replay.setMainView({{"targetFrame", state->target}});
            replay.setCloudView(state->firstId, {{"targetFrame", "__uav_studio_missing_tf__"}});
            capture("rejected-inset.png", state->firstId, false, false); return;
        case 22:
            capture("rejected-inset-canvas.png", "main", true, false); return;
        case 23:
            replay.setCloudView(state->firstId, {{"targetFrame", state->target}});
            replay.setMainView({{"targetFrame", "__uav_studio_missing_tf__"}});
            capture("rejected-movie.mp4", "main", true, false, -1., true); return;
        case 24:
            replay.seek(replay.duration() * .8); replay.seek(replay.duration() * .12); replay.seek(state->time);
            replay.setMainView({{"targetFrame", state->target}, {"followLag", 10.}});
            if (state->main->exportValidationError(0.).isEmpty()) { finish("Real target has no tested before-coverage boundary"); return; }
            capture("rejected-coverage-main.png", "main", false, false, 0.); return;
        case 25:
            replay.setMainView({{"followLag", 0.}});
            replay.setCloudView(state->firstId, {{"followLag", 10.}});
            if (state->first->exportValidationError(0.).isEmpty()) { finish("Inset target has no tested before-coverage boundary"); return; }
            capture("rejected-coverage-inset.png", state->firstId, false, false, 0.); return;
        case 26:
            replay.setCloudView(state->firstId, {{"followLag", 0.}}); replay.seek(state->time); return;
        default:
            state->valid &= window.grabWindow().save(QDir(directory).filePath("editing-restored.png"));
            finish(QString()); return;
        }
    });
    replay.openBag(QUrl::fromLocalFile(bag));
    timer->start();
    QTimer::singleShot(180000, &app, [finish] { finish("Camera acceptance timed out"); });
}
