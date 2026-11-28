#pragma once
#include <QString>
class QGuiApplication;
class QQuickWindow;
class ReplayController;
class NativeRvizClient;
class MovieExporter;
void runNativeSceneAcceptance(QGuiApplication &, QQuickWindow &, ReplayController &, NativeRvizClient &,
                              MovieExporter &, const QString &directory, const QString &bag,
                              const QString &config, const QString &launcher, const QString &distribution);
