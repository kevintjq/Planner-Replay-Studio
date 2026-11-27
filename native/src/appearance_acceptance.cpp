#include "appearance_acceptance.h"
#include "appearance_manager.h"
#include "cloud_view.h"
#include "replay_controller.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QTimer>
#include <memory>

void runAppearanceAcceptance(QGuiApplication &app, QQuickWindow &window, AppearanceManager &appearance,
                             ReplayController &replay, const QString &directory, const QString &bag)
{
    struct State {
        int step = -2;
        bool valid = true;
        bool keptPlaying = false;
        double position = 0;
        std::shared_ptr<const replay::Recording> record;
        QQuickItem *canvas = nullptr;
        CloudView *cloud = nullptr;
        QImage output;
        QJsonArray captures;
    };
    QDir().mkpath(directory);
    window.resize(1440, 900);
    auto state = std::make_shared<State>();
    auto *timer = new QTimer(&app);
    timer->setInterval(600);
    QObject::connect(timer, &QTimer::timeout, &app, [&, state, timer, directory, bag] {
        if (replay.loading()) return;
        if (state->step == -2) {
            state->valid = replay.duration() > 0 && (bag.isEmpty() || !replay.demo());
            state->valid &= window.rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL;
            state->record = replay.recording();
            state->canvas = window.findChild<QQuickItem *>("previewCanvas");
            state->cloud = window.findChild<CloudView *>("primaryCloudView");
            if (!state->canvas || !state->cloud) { app.exit(1); return; }
            replay.seek(std::min(5., replay.duration() * .3));
            state->position = replay.position();
            replay.togglePlayback();
            appearance.setMode(AppearanceManager::Dark);
            ++state->step;
            return;
        }
        if (state->step == -1) {
            state->keptPlaying = replay.playing() && replay.position() > state->position;
            state->valid &= state->keptPlaying;
            replay.pause();
            replay.seek(std::min(5., replay.duration() * .3));
            appearance.setMode(AppearanceManager::Light);
            state->step = 0;
            window.setProperty("workspaceIndex", 0);
            return;
        }
        const int step = state->step;
        const auto name = QString("%1-%2.png").arg(step < 4 ? "light" : "dark").arg(step % 4 + 1);
        const auto screenshot = window.grabWindow();
        const auto origin = state->canvas->mapToScene(QPointF()).toPoint();
        const auto dpr = window.devicePixelRatio();
        const auto output = screenshot.copy(QRect(origin * dpr, QSize(state->canvas->width() * dpr, state->canvas->height() * dpr)));
        if (step == 0) state->output = output;
        bool unchanged = replay.recording() == state->record &&
            window.findChild<QQuickItem *>("previewCanvas") == state->canvas &&
            window.findChild<CloudView *>("primaryCloudView") == state->cloud && !replay.loading();
        // 场景像素只在可见工作区比较；截图来自真实运行窗口。
        if (step % 4 < 3)
            unchanged &= !state->output.isNull() && output == state->output;
        state->valid &= unchanged && !screenshot.isNull() && screenshot.save(QDir(directory).filePath(name));
        state->captures.append(QJsonObject{{"file", name}, {"state_preserved", unchanged},
            {"width", screenshot.width()}, {"height", screenshot.height()}});
        if (++state->step == 8) {
            const auto mode = appearance.mode();
            AppearanceManager restored(nullptr, QDir(directory).filePath("appearance.ini"));
            state->valid &= restored.mode() == mode;
            QJsonObject report{{"passed", state->valid}, {"recording", replay.name()},
                {"load_status", replay.status()}, {"point_count", replay.pointCount()},
                {"real_bag", !replay.demo()}, {"theme_kept_playing", state->keptPlaying},
                {"screenshots", state->captures}, {"renderer", window.rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL ? "OpenGL" : "Other"},
                {"trajectory_workspace", "Source not loaded; does not certify foreground extraction"}};
            QFile file(QDir(directory).filePath("appearance-acceptance.json"));
            if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0) state->valid = false;
            timer->stop();
            app.exit(state->valid ? 0 : 1);
        } else {
            appearance.setMode(state->step < 4 ? AppearanceManager::Light : AppearanceManager::Dark);
            window.setProperty("workspaceIndex", state->step % 4);
        }
    });
    if (!bag.isEmpty()) replay.openBag(QUrl::fromLocalFile(bag));
    timer->start();
    QTimer::singleShot(120000, &app, [&app] { app.exit(1); });
}
