#pragma once

#include "video_timing.h"

#include <QFutureWatcher>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QQuickImageProvider>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>

class QQuickItem;
class QQuickItemGrabResult;
class ReplayController;

class MovieImages final : public QQuickImageProvider {
public:
    MovieImages() : QQuickImageProvider(QQuickImageProvider::Image) {}
    void prepare(const std::vector<QImage> &images);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    QMutex lock_;
    std::vector<QImage> images_;
};

class MovieExporter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool exporting READ exporting NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantList frames READ frames NOTIFY framesChanged)
    Q_PROPERTY(QVariantList captureMetrics READ captureMetrics NOTIFY changed)
public:
    MovieExporter(ReplayController &replay, MovieImages &images);
    ~MovieExporter() override;

    bool exporting() const { return exporting_; }
    double progress() const;
    QString message() const { return message_; }
    const QVariantList &frames() const { return frames_; }
    QVariantList captureMetrics() const { return captureMetrics_; }
    Q_INVOKABLE void start(const QUrl &destination, QQuickItem *canvas, double begin, double end,
                           double speed, int audioIndex, int width = 1920, int height = 1080, int fps = 60, int quality = 18, int supersample = 1, bool nvenc = false);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void startStill(const QUrl &destination, QQuickItem *canvas, double time, const QVariantMap &options = {});

signals:
    void changed();
    void framesChanged();
    void finished(bool success, const QString &message);
    void frameCaptured(const QImage &image, double experimentTime);

private:
    struct Decoder;
    struct DecodedFrame {
        std::vector<QImage> images;
        QString error;
    };

    void requestNext();
    void collectFrame();
    void grabFrame();
    void finish(const QString &error);
    void saveStill(const QImage &image);
    QString prepareCapture(QQuickItem *canvas, QQuickItem *target, int width, int height, int supersample, const QVariantMap &options);
    QString validateCapture();
    QString updateCaptureSizes();
    void restoreCapture();
    QStringList encoderArguments() const;

    ReplayController &replay_;
    MovieImages &images_;
    QProcess process_;
    QFutureWatcher<DecodedFrame> watcher_;
    QFutureWatcher<QString> stillWriter_;
    QPointer<QQuickItem> canvas_;
    QPointer<QQuickItem> captureItem_;
    QVariant previousCaptureOptions_;
    QList<QPair<QPointer<QQuickItem>, QSize>> captureViews_;
    QSharedPointer<QQuickItemGrabResult> grab_;
    std::shared_ptr<Decoder> decoder_;
    std::unique_ptr<QTemporaryDir> staging_;
    std::optional<replay::VideoClip> audio_;
    std::atomic_bool cancelled_ = false;
    QVariantList frames_;
    QVariantList captureMetrics_;
    QString destination_, executable_, message_, pendingError_;
    QByteArray diagnostic_;
    double begin_ = 0., speed_ = 1., previous_ = 0., audioOrigin_ = 0.;
    qint64 index_ = 0, count_ = 0;
    int width_ = 1920, height_ = 1080, fps_ = 60, quality_ = 18, supersample_ = 1;
    int dpi_ = 300;
    bool previousPlaying_ = false;
    bool nvenc_ = false;
    bool exporting_ = false, awaitingWrite_ = false, still_ = false;
    quint64 revision_ = 0;
};
