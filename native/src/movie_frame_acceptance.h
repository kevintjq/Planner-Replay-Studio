#pragma once
#include <QString>
class QGuiApplication;
class QQuickWindow;
class ReplayController;
class MovieExporter;
void runMovieFrameAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                             MovieExporter &movie, const QString &directory, const QString &bag, const QString &video);
