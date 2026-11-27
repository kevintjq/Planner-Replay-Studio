#pragma once
#include <QString>
class QGuiApplication;
class QQuickWindow;
class ReplayController;
class MovieExporter;
class FrameBatchExporter;
class TrajectoryController;
class AppearanceManager;
void runFrameAcceptance(QGuiApplication &, QQuickWindow &, ReplayController &, MovieExporter &, FrameBatchExporter &, TrajectoryController &, AppearanceManager &,
                        const QString &directory, const QString &bag, const QString &video);
