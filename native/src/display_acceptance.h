#pragma once
#include <QString>
class QGuiApplication;
class QQuickWindow;
class ReplayController;
void runDisplayAcceptance(QGuiApplication &app, QQuickWindow &window, ReplayController &replay,
                          const QString &directory, const QString &bag);
