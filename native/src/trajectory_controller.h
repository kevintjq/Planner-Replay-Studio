#pragma once

#include "trajectory_frame_model.h"
#include "trajectory_frame_export.h"
#include <QCache>
#include <QFutureWatcher>
#include <QMutex>
#include <QQuickImageProvider>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <atomic>

class PreviewImages final : public QQuickImageProvider {
public:
    PreviewImages() : QQuickImageProvider(QQuickImageProvider::Image, ForceAsynchronousImageLoading), thumbnails_(16 * 1024) {}
    void setImage(const QImage &image);
    quint64 setTrajectory(std::shared_ptr<const replay::FixedCameraTrajectory> trajectory);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
private:
    QMutex lock_;
    QImage image_;
    std::shared_ptr<const replay::FixedCameraTrajectory> trajectory_;
    QCache<QString, QImage> thumbnails_;
    quint64 sourceRevision_ = 0;
};

class TrajectoryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool working READ working NOTIFY changed)
    Q_PROPERTY(bool previewing READ previewing NOTIFY changed)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(double startTime READ startTime NOTIFY changed)
    Q_PROPERTY(double lastTime READ lastTime NOTIFY changed)
    Q_PROPERTY(QUrl image READ image NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel *frames READ frames CONSTANT)
    Q_PROPERTY(int sampleCount READ sampleCount NOTIFY changed)
    Q_PROPERTY(QVariantList excludedRanges READ excludedRanges NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY changed)
    Q_PROPERTY(QString exportDirectory READ exportDirectory NOTIFY changed)
public:
    explicit TrajectoryController(PreviewImages &images);
    ~TrajectoryController() override;
    bool working() const { return working_ || exporting_; }
    bool previewing() const { return previewWatcher_.isRunning() || previewTimer_.isActive(); }
    bool hasResult() const { return bool(trajectory_); }
    double progress() const { return progress_; }
    QString status() const { return status_; }
    QString fileName() const;
    double startTime() const { return start_; }
    double lastTime() const;
    QUrl image() const;
    QAbstractItemModel *frames() { return &frames_; }
    int sampleCount() const { return frames_.rowCount(); }
    QVariantList excludedRanges() const;
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    QVariantMap settings() const;
    QString exportDirectory() const { return exportDirectory_; }
    Q_INVOKABLE void open(const QUrl &file);
    Q_INVOKABLE void preview(double seconds);
    Q_INVOKABLE void track(const QRectF &selection, const QString &endSeconds);
    Q_INVOKABLE void configure(double interval, double opacity, int mode = 0, int count = 12);
    Q_INVOKABLE void toggleFrame(int row);
    Q_INVOKABLE void selectFrame(int row, bool extend, bool toggle);
    Q_INVOKABLE void editSelectedFrames(bool excluded, double opacity = -1.);
    Q_INVOKABLE void addManualFrame(double seconds);
    Q_INVOKABLE void addExcludedRange(double begin, double end, int parts);
    Q_INVOKABLE void editExcludedRange(const QString &id, double begin, double end, int parts);
    Q_INVOKABLE void removeExcludedRange(const QString &id);
    Q_INVOKABLE void splitExcludedRange(const QString &id, double at);
    Q_INVOKABLE void setGenerationRange(double begin, double end);
    Q_INVOKABLE void setShowLine(bool enabled);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void restoreFrames();
    Q_INVOKABLE void resetPreview();
    Q_INVOKABLE void save(const QUrl &target);
    Q_INVOKABLE void exportFrame(const QUrl &target, const QVariantMap &options);
    Q_INVOKABLE void exportFrames(const QUrl &parentDirectory, const QVariantMap &options);
    Q_INVOKABLE void cancel();
    void restoreSettings(const QVariantMap &settings);
signals:
    void changed();
    void settingsEdited(const QVariantMap &settings);
    void frameExported(bool success, const QString &message);
    void batchExported(bool success, const QString &message);
private:
    struct State {
        replay::TrajectoryEdits edits;
        double interval = .5, opacity = .9;
        int mode = 0, count = 12;
        bool operator==(const State &) const = default;
    } state_;
    struct Result {
        QImage image;
        std::shared_ptr<const replay::FixedCameraTrajectory> trajectory;
        QString message;
    };
    struct Preview { QImage image; QString error; quint64 generation = 0; };
    void begin(const QString &status, std::function<Result()> job);
    void setImage(const QImage &image);
    void updatePreview();
    void renderPreview();
    void commit(State next);
    void clearResult();
    void reportError(const std::exception &error);
    PreviewImages &images_;
    TrajectoryFrameModel frames_;
    QFutureWatcher<Result> watcher_;
    QFutureWatcher<Preview> previewWatcher_;
    QFutureWatcher<QString> exportWatcher_;
    QFutureWatcher<replay::TrajectoryBatchResult> batchWatcher_;
    QTimer previewTimer_;
    std::shared_ptr<const replay::FixedCameraTrajectory> trajectory_;
    std::shared_ptr<std::atomic_bool> previewCancelled_;
    std::atomic_bool cancelled_ = false;
    QImage initial_;
    QString file_, status_, end_, exportDirectory_;
    QRectF selectionBox_;
    QSet<qint64> selected_;
    std::vector<State> undo_, redo_;
    std::vector<size_t> sampled_;
    quint64 revision_ = 0, sourceRevision_ = 0, generation_ = 0;
    double start_ = 0., progress_ = 0.;
    int selectionAnchor_ = -1;
    bool working_ = false, exporting_ = false, previewDirty_ = false, resumeTracking_ = false;
    std::optional<QVariantMap> pendingSettings_;
};
