#include "display_acceptance.h"
#include "cloud_view.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
#include <algorithm>

namespace {
int maximumChannelDelta(const QImage &first, const QImage &second)
{
    if (first.isNull() || first.size() != second.size()) return 255;
    const auto a = first.convertToFormat(QImage::Format_RGBA8888), b = second.convertToFormat(QImage::Format_RGBA8888);
    int maximum = 0;
    for (int row = 0; row < a.height(); ++row)
        for (int column = 0; column < a.width() * 4; ++column)
            maximum = std::max(maximum, std::abs(int(a.constScanLine(row)[column]) - int(b.constScanLine(row)[column])));
    return maximum;
}
}

void runDisplayAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                          const QString &directory, const QString &bag)
{
    struct State {
        int step = 0, loadSignals = 0;
        bool valid = true, keptPlaying = false;
        double time = 0.;
        QImage hidden, enabled, restored;
        std::shared_ptr<const replay::Recording> recording;
        CloudView *cloud = nullptr;
        QQuickItem *canvas = nullptr;
        QVariantList markers, layers, views;
    };
    auto state = std::make_shared<State>();
    QDir().mkpath(directory); window.resize(1440, 900);
    auto *timer = new QTimer(&app); timer->setInterval(350);
    const auto capture = [&, state] {
        const auto image = window.grabWindow();
        const auto dpr = window.devicePixelRatio();
        const auto origin = state->canvas->mapToScene(QPointF());
        return image.copy(QRect(QPoint(origin.x() * dpr, origin.y() * dpr),
            QSize(state->canvas->width() * dpr, state->canvas->height() * dpr)));
    };
    QObject::connect(&replay, &ReplayController::stateChanged, &app, [&, state] {
        if (state->step >= 2 && replay.loading()) ++state->loadSignals;
    });
    QObject::connect(timer, &QTimer::timeout, &app, [&, state, timer, capture, directory] {
        if (replay.loading()) return;
        if (state->step == 0) {
            if (replay.demo() || replay.cloudTopic().isEmpty()) { app.exit(1); return; }
            const auto path = QDir(directory).filePath("display-source.rviz");
            QFile file(path);
            const auto yaml = QByteArray("Visualization Manager:\n  Displays:\n    - Class: rviz/PointCloud2\n      Name: Cached hidden cloud\n      Enabled: false\n      Color: 60; 160; 240\n      Topic: '")
                + replay.cloudTopic().toUtf8().replace("'", "''") + "'\n";
            if (!file.open(QIODevice::WriteOnly) || file.write(yaml) != yaml.size()) { app.exit(1); return; }
            file.close(); state->step = 1; replay.openRviz(QUrl::fromLocalFile(path)); return;
        }
        if (state->step == 1) {
            state->recording = replay.recording();
            state->valid &= !state->recording->showMainCloud && replay.topicDisplays().size() == 1;
            state->cloud = window.findChild<CloudView *>("primaryCloudView");
            state->canvas = window.findChild<QQuickItem *>("previewCanvas");
            if (!state->cloud || !state->canvas) { app.exit(1); return; }
            state->cloud->setShowPath(false);
            replay.setCamera(4); state->cloud->orbit(35, 15); state->cloud->zoom(1);
            state->time = std::min(5., replay.duration() * .3);
            replay.seek(state->time); replay.addMarker(); replay.addSceneText("Display visibility acceptance"); replay.addCloudView(2);
            state->markers = replay.markers(); state->layers = replay.sceneLayers(); state->views = replay.cloudViews();
            state->step = 2; return;
        }
        state->valid &= replay.recording() == state->recording && !replay.loading() &&
            window.findChild<CloudView *>("primaryCloudView") == state->cloud &&
            window.findChild<QQuickItem *>("previewCanvas") == state->canvas;
        if (state->step == 2) {
            state->hidden = capture(); replay.togglePlayback();
            for (int i = 0; i < 20; ++i) { replay.setTopicDisplayEnabled(0, true); replay.setTopicDisplayEnabled(0, false); }
            state->valid &= replay.playing(); state->step = 3; return;
        }
        if (state->step == 3) {
            state->keptPlaying = replay.playing() && replay.position() > state->time;
            state->valid &= state->keptPlaying;
            replay.pause(); replay.seek(state->time); replay.setTopicDisplayEnabled(0, true); state->step = 4; return;
        }
        if (state->step == 4) {
            state->enabled = capture(); replay.setTopicDisplayEnabled(0, false); state->step = 5; return;
        }
        state->restored = capture();
        const bool visualChange = !state->hidden.isNull() && state->hidden != state->enabled;
        const bool exactRestore = state->hidden == state->restored;
        // Qt's text atlas can differ by one quantization level after playback;
        // report the actual delta instead of claiming byte-identical screenshots.
        const int restoredDelta = maximumChannelDelta(state->hidden, state->restored);
        state->valid &= visualChange && restoredDelta <= 1 && state->loadSignals == 0 && !replay.playing() &&
            replay.position() == state->time && replay.cameraIndex() == 4 && replay.markers() == state->markers &&
            replay.sceneLayers() == state->layers && replay.cloudViews() == state->views;
        state->valid &= state->hidden.save(QDir(directory).filePath("hidden.png")) &&
            state->enabled.save(QDir(directory).filePath("enabled.png")) && state->restored.save(QDir(directory).filePath("restored.png"));
        replay.saveProjectAs(QUrl::fromLocalFile(QDir(directory).filePath("display-state.slamfpv")));
        replay.exportRviz(QUrl::fromLocalFile(QDir(directory).filePath("display-restored.rviz")));
        const QJsonObject report{{"passed", state->valid}, {"real_bag", !replay.demo()},
            {"renderer", window.rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL ? "OpenGL" : "Other"},
            {"recording_preserved", replay.recording() == state->recording}, {"playback_preserved", state->keptPlaying},
            {"hidden_layer_changes_pixels", visualChange}, {"camera_pixels_restored", restoredDelta <= 1},
            {"pixel_exact_restore", exactRestore}, {"restored_max_channel_delta", restoredDelta},
            {"loading_signals_during_toggles", state->loadSignals}, {"visibility_cycles", 20},
            {"limitations", "Exercises controller actions and rendered output; does not certify manual checkbox interaction or complete RViz plugin parity."}};
        QFile file(QDir(directory).filePath("display-acceptance.json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0) state->valid = false;
        timer->stop(); app.exit(state->valid ? 0 : 1);
    });
    replay.openBag(QUrl::fromLocalFile(bag)); timer->start();
    QTimer::singleShot(120000, &app, [&] { app.exit(1); });
}
