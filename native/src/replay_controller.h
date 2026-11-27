#pragma once

#include "project_document.h"
#include "replay_data.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <atomic>

class ReplayController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(double duration READ duration NOTIFY recordingChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString name READ name NOTIFY recordingChanged)
    Q_PROPERTY(QString speed READ speed NOTIFY positionChanged)
    Q_PROPERTY(QString cloudTopic READ cloudTopic NOTIFY recordingChanged)
    Q_PROPERTY(QString odometryTopic READ odometryTopic NOTIFY recordingChanged)
    Q_PROPERTY(QStringList cloudTopics READ cloudTopics NOTIFY recordingChanged)
    Q_PROPERTY(QStringList odometryTopics READ odometryTopics NOTIFY recordingChanged)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY positionChanged)
    Q_PROPERTY(bool replacing READ replacing NOTIFY recordingChanged)
    Q_PROPERTY(bool demo READ demo NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList topicDisplays READ topicDisplays NOTIFY recordingChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY recordingChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editHistoryChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editHistoryChanged)
    Q_PROPERTY(QVariantList videoLayers READ videoLayers NOTIFY videoLayersChanged)
public:
    explicit ReplayController(QObject *parent = nullptr);
    ~ReplayController() override;

    bool loading() const { return loading_; }
    bool playing() const { return timer_.isActive(); }
    double duration() const { return record_->duration(); }
    double position() const { return position_; }
    double progress() const { return progress_; }
    QString status() const { return status_; }
    QString name() const { return record_->name; }
    QString speed() const;
    QString cloudTopic() const { return record_->cloudTopic; }
    QString odometryTopic() const { return record_->odometryTopic; }
    QStringList cloudTopics() const { return record_->cloudTopics; }
    QStringList odometryTopics() const { return record_->odometryTopics; }
    int pointCount() const;
    bool replacing() const { return record_->replace; }
    bool demo() const { return path_.isEmpty() && (!project_ || project_->source() == "demo"); }
    QString projectPath() const { return project_ ? project_->path() : QString(); }
    std::shared_ptr<const replay::Recording> recording() const { return record_; }

    const QVariantList &videoLayers() const { return videoLayers_; }
    Q_INVOKABLE double videoSeconds(int index) const;
    Q_INVOKABLE void addVideo(const QUrl &url);
    Q_INVOKABLE void setVideoTrim(int index, const QString &trimIn, const QString &trimOut, const QString &offset);
    Q_INVOKABLE void setVideoAnchor(int index, int slot, const QString &seconds);
    Q_INVOKABLE void clearVideoAnchors(int index);
    Q_INVOKABLE void splitVideo(int index, double at);
    Q_INVOKABLE void deleteVideoSegment(int index, double at);
    Q_INVOKABLE void trimVideoSegment(int index, int segment, double first, double last);
    Q_INVOKABLE void setVideoDuration(int index, const QUrl &url, double seconds);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    QVariantList topicDisplays() const;
    Q_INVOKABLE void setTopicDisplayEnabled(int index, bool enabled);
    Q_INVOKABLE void openRviz(const QUrl &url);
    Q_INVOKABLE void openBag(const QUrl &url);
    Q_INVOKABLE void openDemo();
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void saveProjectAs(const QUrl &url);
    Q_INVOKABLE void exportProjectFolder(const QUrl &parent);
    Q_INVOKABLE void setCloudTopic(const QString &topic);
    Q_INVOKABLE void setOdometryTopic(const QString &topic);
    Q_INVOKABLE void setReplacing(bool replace);
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void playingChanged();
    void positionChanged();
    void recordingChanged();
    void videoLayersChanged();
    void editHistoryChanged();

private:
    struct Result {
        std::shared_ptr<const replay::Recording> record;
        QString path;
        QString issue;
        std::optional<replay::MapMode> mode;
        std::optional<replay::ProjectDocument> project;
        bool keepSession = false;
        QStringList skipped;
    };
    void load(const QString &path, const replay::LoadOptions &options,
              std::optional<replay::ProjectDocument> project = {}, bool keepSession = false,
              const QStringList &skipped = {});
    replay::ProjectDocument projectSnapshot() const;
    void install(std::shared_ptr<const replay::Recording> record, bool keepSession = false);
    void applyTopicLayout(const replay::RvizLayout &layout);
    void setPosition(double seconds);
    void refreshVideoLayers();
    void editVideo(int index, const QJsonObject &changes);
    void changeVideoSegment(int index, double at, bool remove);
    void storeVideoSpans(int index, const std::vector<replay::VideoTiming::Range> &ranges);
    struct VideoEdit {
        int index;
        QJsonObject before, after;
    };
    void restoreVideoEdit(bool redo);

    QHash<QString, double> videoDurations_;
    std::vector<VideoEdit> undo_, redo_;

    std::shared_ptr<const replay::Recording> record_;
    QFutureWatcher<Result> watcher_;
    QFutureWatcher<QString> folderWatcher_;
    std::optional<replay::ProjectDocument> project_;
    std::vector<replay::VideoClip> clips_;
    QVariantList videoLayers_;
    QTimer timer_;
    QElapsedTimer clock_;
    QString path_, status_;
    std::optional<replay::MapMode> mapMode_;
    std::atomic_bool cancelRequested_ = false;
    bool loading_ = false;
    double position_ = 0., start_ = 0., progress_ = 0.;
};
