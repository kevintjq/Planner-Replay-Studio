#pragma once

#include "trajectory_image.h"

#include <QFutureWatcher>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QUrl>
#include <QVariantList>
#include <atomic>

class PreviewImages final : public QQuickImageProvider {
public:
    PreviewImages() : QQuickImageProvider(QQuickImageProvider::Image) {}
    void setImage(const QImage &image);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    QMutex lock_;
    QImage image_;
};

class TrajectoryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool working READ working NOTIFY changed)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(double startTime READ startTime NOTIFY changed)
    Q_PROPERTY(QUrl image READ image NOTIFY changed)
    Q_PROPERTY(QVariantList frames READ frames NOTIFY changed)
public:
    explicit TrajectoryController(PreviewImages &images);
    ~TrajectoryController() override;

    bool working() const { return working_; }
    bool hasResult() const { return bool(trajectory_); }
    double progress() const { return progress_; }
    QString status() const { return status_; }
    QString fileName() const;
    double startTime() const { return start_; }
    QUrl image() const;
    QVariantList frames() const { return frames_; }

    Q_INVOKABLE void open(const QUrl &file);
    Q_INVOKABLE void preview(double seconds);
    Q_INVOKABLE void track(const QRectF &selection, const QString &endSeconds);
    Q_INVOKABLE void configure(double interval, double opacity);
    Q_INVOKABLE void toggleFrame(int row);
    Q_INVOKABLE void restoreFrames();
    Q_INVOKABLE void resetPreview();
    Q_INVOKABLE void save(const QUrl &target);
    Q_INVOKABLE void cancel();

signals:
    void changed();

private:
    struct Result {
        QImage image;
        std::shared_ptr<const replay::FixedCameraTrajectory> trajectory;
        QString message;
    };

    void begin(const QString &status, std::function<Result()> job);
    void setImage(const QImage &image);
    void updatePreview();

    PreviewImages &images_;
    QFutureWatcher<Result> watcher_;
    std::shared_ptr<const replay::FixedCameraTrajectory> trajectory_;
    std::atomic_bool cancelled_ = false;
    QImage initial_;
    QString file_, status_;
    QSet<int> excluded_;
    QVariantList frames_;
    std::vector<size_t> sampled_;
    quint64 revision_ = 0;
    double start_ = 0., interval_ = 0., opacity_ = .9, progress_ = 0.;
    bool working_ = false;
};
