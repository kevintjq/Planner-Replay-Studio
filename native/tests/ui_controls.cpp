#include "appearance_manager.h"
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>

// The native API uses the valid C++ property name "synchronized", a QML keyword.
// Declare that one fixture property in C++ so tests exercise the real spelling.
class NativeReplayTestState : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool synchronized MEMBER inSync_ NOTIFY syncChanged)
public:
    using QObject::QObject;
    bool inSync_ = false;
signals:
    void syncChanged();
};

class ControlTestSetup : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;
public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        qmlRegisterType<NativeReplayTestState>("UAVStudioTest", 1, 0, "NativeReplayTestState");
        engine->rootContext()->setContextProperty("appearance",
            new AppearanceManager(engine, directory_.filePath("appearance.ini")));
        // Feed the actual production MouseArea to the isolated pointer fixture;
        // the test supplies only editor/image metrics, never a copied algorithm.
        QFile trajectoryPane(QStringLiteral(QUICK_TEST_SOURCE_DIR) + "/../../qml/TrajectoryPane.qml");
        if (!trajectoryPane.open(QIODevice::ReadOnly)) qFatal("Cannot read production trajectory pointer input.");
        engine->rootContext()->setContextProperty("trajectoryPaneSource", QString::fromUtf8(trajectoryPane.readAll()));
        QFile mainSource(QStringLiteral(QUICK_TEST_SOURCE_DIR "/../../qml/Main.qml"));
        if (!mainSource.open(QIODevice::ReadOnly)) qFatal("Cannot read production Main.qml for UI routing checks");
        engine->rootContext()->setContextProperty("mainUiSource", QString::fromUtf8(mainSource.readAll()));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(core_ui, ControlTestSetup)
#include "ui_controls.moc"
