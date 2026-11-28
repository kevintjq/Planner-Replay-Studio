#pragma once
#include <QString>
class QGuiApplication;
class QQuickWindow;
class ReplayController;
class NativeRvizClient;
void runNativeReplayAcceptance(QGuiApplication &, QQuickWindow &, ReplayController &, NativeRvizClient &,
                               const QString &directory, const QString &bag, const QString &config,
                               const QString &launcher, const QString &distribution);
