#pragma once
#include <QString>
class QGuiApplication;
class QQuickWindow;
class AppearanceManager;
class ReplayController;
void runAppearanceAcceptance(QGuiApplication &app, QQuickWindow &window, AppearanceManager &appearance,
                             ReplayController &replay, const QString &directory, const QString &bag);
