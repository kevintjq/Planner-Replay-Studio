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
public:
    MovieExporter(ReplayController &replay, MovieImages &images);
    ~MovieExporter() override;

    bool exporting() const { return exporting_; }
    double progress() const;
    QString message() const { return message_; }
    const QVariantList &frames() const { return frames_; }
    Q_INVOKABLE void start(const QUrl &destination, QQuickItem *canvas, double begin, double end,
                           double speed, int audioIndex);
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void framesChanged();

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
    QStringList encoderArguments() const;

    ReplayController &replay_;
    MovieImages &images_;
    QProcess process_;
    QFutureWatcher<DecodedFrame> watcher_;
    QPointer<QQuickItem> canvas_;
    QSharedPointer<QQuickItemGrabResult> grab_;
    std::shared_ptr<Decoder> decoder_;
    std::unique_ptr<QTemporaryDir> staging_;
    std::optional<replay::VideoClip> audio_;
    std::atomic_bool cancelled_ = false;
    QVariantList frames_;
    QString destination_, executable_, message_, pendingError_;
    QByteArray diagnostic_;
    double begin_ = 0., speed_ = 1., previous_ = 0., audioOrigin_ = 0.;
    qint64 index_ = 0, count_ = 0;
    bool exporting_ = false, awaitingWrite_ = false;
    quint64 revision_ = 0;
};
