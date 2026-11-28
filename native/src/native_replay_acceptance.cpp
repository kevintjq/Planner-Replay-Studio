#include "native_replay_acceptance.h"
#include "native_rviz_client.h"
#include "replay_controller.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QTimer>

void runNativeReplayAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                               NativeRvizClient &client, const QString &directory, const QString &bag, const QString &config,
                               const QString &launcher, const QString &distribution)
{
    struct State {
        int phase = 0, target = 0, loads = 0, seekRequests = 0, seekRequestsBeforePlayback = 0;
        bool finished = false, playbackUnlinked = false;
        QPointer<QObject> panel;
        std::shared_ptr<const replay::Recording> recording;
        QElapsedTimer deadline, playback;
        QString lastNativeTarget, error;
        QJsonArray cases;
        QVariantList layers, views, markers;
    };
    auto state = std::make_shared<State>();
    state->deadline.start();
    QDir().mkpath(directory);
    window.resize(1440, 900);
    auto *timer = new QTimer(&app);
    timer->setInterval(150);
    const auto finish = [&, state, timer, directory](const QString &error) {
        if (state->finished) return;
        state->finished = true; timer->stop();
        const bool preserved = state->recording && replay.recording() == state->recording && state->loads == 0
            && replay.sceneLayers() == state->layers && replay.cloudViews() == state->views && replay.markers() == state->markers;
        const bool passed = error.isEmpty() && preserved && state->cases.size() == 5 && state->playbackUnlinked;
        const QJsonObject report{{"passed", passed}, {"real_bag", !replay.demo()}, {"cases", state->cases},
            {"source", QJsonObject::fromVariantMap(replay.nativeReplaySource())},
            {"playback_did_not_trigger_native_seek", state->playbackUnlinked}, {"source_and_edits_preserved", preserved},
            {"native_seek_requests", state->seekRequests},
            {"seek_requests_at_playback_start", state->seekRequestsBeforePlayback},
            {"loading_signals", state->loads}, {"error", error},
            {"limitations", "Real main QML bindings, paused Header seeking, actual librviz/OGRE PNG. Does not certify manual GUI interaction, continuous native playback, third-party asynchronous plugins, native main-canvas composition or high-resolution native export."}};
        QFile file(QDir(directory).filePath("native-playhead-acceptance.json"));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        client.stop();
        QTimer::singleShot(0, &app, [&, saved, passed] { app.exit(saved && passed ? 0 : 1); });
    };
    QObject::connect(&client, &NativeRvizClient::commandFinished, &app,
        [state](const QString &command, bool ok, const QString &error) {
            if (!ok && (command == "bag.index" || command == "bag.seek" || command == "load")) state->error = error;
        });
    QObject::connect(&client, &NativeRvizClient::commandSent, &app, [state](const QString &command) {
        if (command == "bag.seek") ++state->seekRequests;
    });
    QObject::connect(&replay, &ReplayController::stateChanged, &app, [&, state] {
        if (state->recording && replay.loading()) ++state->loads;
    });
    QObject::connect(timer, &QTimer::timeout, &app, [&, state, directory, finish, bag, config, launcher, distribution] {
        if (!state->error.isEmpty()) { finish(state->error); return; }
        if (state->deadline.elapsed() > 180000) { finish("Native playhead workflow timed out: " + client.status()); return; }
        if (state->phase == 0) {
            replay.openBag(QUrl::fromLocalFile(bag));
            auto *section = window.findChild<QObject *>("settingsSection");
            state->panel = window.findChild<QObject *>("nativeRvizInspector");
            if (!section || !state->panel) { finish("Native inspector is missing from the real application"); return; }
            section->setProperty("currentIndex", 5);
            client.start(QUrl::fromLocalFile(launcher), distribution, QUrl::fromLocalFile(config));
            state->phase = 1; return;
        }
        if (state->phase == 1) {
            if (!client.running() && !client.connected()) { finish(client.status() + "\n" + client.diagnostic()); return; }
            if (replay.loading() || !client.connected()) return;
            if (replay.demo() || replay.bagPath().isEmpty()) { finish(replay.status()); return; }
            state->recording = replay.recording();
            state->layers = replay.sceneLayers(); state->views = replay.cloudViews(); state->markers = replay.markers();
            if (state->panel->property("experimentTargetNs").toString() != replay.experimentStampNs()) {
                finish("Main playhead binding lost integer timestamp precision"); return;
            }
            if (!QMetaObject::invokeMethod(state->panel, "indexExperiment")) { finish("Cannot invoke actual inline index action"); return; }
            state->phase = 2; return;
        }
        if (state->phase == 2) {
            if (client.busy()) return;
            if (!state->panel->property("experimentIndexed").toBool()) { finish("Inline source identity did not bind the actual index"); return; }
            state->panel->setProperty("linkPausedFrame", true);
            replay.seek(8.); state->phase = 3; return;
        }
        if (state->phase == 3 || state->phase == 5) {
            if (client.busy() || !state->panel->property("matchesPlayhead").toBool() || client.preview().isEmpty()) return;
            if (client.targetNs() != replay.experimentStampNs() || client.clockNs() != replay.experimentStampNs()) {
                finish("Native target/clock does not match current integer Header time"); return;
            }
            const auto encoded = client.preview().section(',', 1).toLatin1();
            const QImage image = QImage::fromData(QByteArray::fromBase64(encoded), "PNG");
            const auto name = QString("native-%1.png").arg(state->cases.size());
            if (image.isNull() || !image.save(QDir(directory).filePath(name))) { finish("Actual native image was not saved"); return; }
            state->cases.append(QJsonObject{{"experiment_seconds", replay.position()}, {"target_ns", client.targetNs()},
                {"clock_ns", client.clockNs()}, {"width", image.width()}, {"height", image.height()},
                {"file", name}, {"result", QJsonObject::fromVariantMap(client.replayResult())}, {"passed", true}});
            window.grabWindow().save(QDir(directory).filePath("main-native-inspector.png"));
            if (state->phase == 5) { finish({}); return; }
            const double targets[]{2., 8., 8.025};
            if (state->target < 3) { replay.seek(targets[state->target++]); return; }
            state->lastNativeTarget = client.targetNs();
            state->seekRequestsBeforePlayback = state->seekRequests;
            replay.togglePlayback(); state->playback.start(); state->phase = 4; return;
        }
        if (state->phase == 4 && state->playback.elapsed() > 600) {
            state->playbackUnlinked = replay.playing() && !client.busy() && client.targetNs() == state->lastNativeTarget
                && state->seekRequests == state->seekRequestsBeforePlayback
                && !state->panel->property("matchesPlayhead").toBool();
            replay.pause(); state->phase = 5;
        }
    });
    timer->start();
}
