#pragma once
#include "frame_export_plan.h"
#include <QFutureWatcher>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QUrl>
class QQuickItem;
class MovieExporter;
class ReplayController;

class FrameBatchExporter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString outputDirectory READ outputDirectory NOTIFY changed)
public:
    FrameBatchExporter(ReplayController &replay, MovieExporter &movie);
    ~FrameBatchExporter() override;
    bool active() const { return active_; }
    double progress() const;
    QString message() const { return message_; }
    QString outputDirectory() const { return directory_; }
    Q_INVOKABLE void start(const QUrl &parentDirectory, QQuickItem *canvas, const QVariantMap &options);
    Q_INVOKABLE void cancel();
signals:
    void changed();
    void finished(bool success, const QString &message);
private:
    void next();
    void finish(const QString &error);
    bool saveManifest(const QString &status, const QString &error = {});
    ReplayController &replay_;
    MovieExporter &movie_;
    QPointer<QQuickItem> canvas_;
    QVector<replay::FrameExportJob> jobs_;
    QVariantMap options_;
    QJsonArray results_;
    QString directory_, message_;
    int index_ = 0;
    double previous_ = 0;
    bool active_ = false, cancelled_ = false, wasPlaying_ = false, waiting_ = false;
    QFutureWatcher<QString> figureWriter_;
};
