#pragma once

#include "project_document.h"
#include "replay_data.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QQuickItem>
#include <QUrl>
#include <QVariantList>
#include <atomic>

class QQuickItemGrabResult;

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
    Q_PROPERTY(QVariantMap speedGauge READ speedGauge NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap moviePreferences READ moviePreferences NOTIFY recordingChanged)
    Q_PROPERTY(QString cloudTopic READ cloudTopic NOTIFY recordingChanged)
    Q_PROPERTY(QString odometryTopic READ odometryTopic NOTIFY recordingChanged)
    Q_PROPERTY(QStringList cloudTopics READ cloudTopics NOTIFY recordingChanged)
    Q_PROPERTY(QStringList odometryTopics READ odometryTopics NOTIFY recordingChanged)
    Q_PROPERTY(QStringList speedTopics READ speedTopics NOTIFY recordingChanged)
    Q_PROPERTY(QString speedTopic READ speedTopic NOTIFY recordingChanged)
    Q_PROPERTY(QStringList scanTopics READ scanTopics NOTIFY recordingChanged)
    Q_PROPERTY(QString scanTopic READ scanTopic NOTIFY recordingChanged)
    Q_PROPERTY(double scanMaximumAge READ scanMaximumAge NOTIFY recordingChanged)
    Q_PROPERTY(QString speedSelection READ speedSelection NOTIFY recordingChanged)
    Q_PROPERTY(bool tfPose READ tfPose NOTIFY recordingChanged)
    Q_PROPERTY(QString worldFrame READ worldFrame NOTIFY recordingChanged)
    Q_PROPERTY(QString bodyFrame READ bodyFrame NOTIFY recordingChanged)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY positionChanged)
    Q_PROPERTY(bool replacing READ replacing NOTIFY recordingChanged)
    Q_PROPERTY(bool demo READ demo NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList topicDisplays READ topicDisplays NOTIFY recordingChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY recordingChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editHistoryChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editHistoryChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    Q_PROPERTY(QVariantList planningEvents READ planningEvents NOTIFY recordingChanged)
    Q_PROPERTY(bool comparePlans READ comparePlans NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap planningLayers READ planningLayers NOTIFY recordingChanged)
    Q_PROPERTY(QStringList cameraLabels READ cameraLabels CONSTANT)
    Q_PROPERTY(int cameraIndex READ cameraIndex NOTIFY cameraChanged)
    Q_PROPERTY(bool transparentCanvas READ transparentCanvas NOTIFY transparentCanvasChanged)
    Q_PROPERTY(bool capturing READ capturing NOTIFY stateChanged)
    Q_PROPERTY(int captureMatte READ captureMatte NOTIFY stateChanged)
    Q_PROPERTY(QVariantList layoutCues READ layoutCues NOTIFY layoutsChanged)
    Q_PROPERTY(QVariantMap activeLayout READ activeLayout NOTIFY activeLayoutChanged)
    Q_PROPERTY(QVariantList sceneLayers READ sceneLayers NOTIFY sceneLayersChanged)
    Q_PROPERTY(QVariantMap mainView READ mainView NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList layerMotions READ layerMotions NOTIFY motionsChanged)
    Q_PROPERTY(QVariantList cloudViews READ cloudViews NOTIFY cloudViewsChanged)
    Q_PROPERTY(QVariantList videoLayers READ videoLayers NOTIFY videoLayersChanged)
public:
    explicit ReplayController(QObject *parent = nullptr);
    ~ReplayController() override;

    bool loading() const { return loading_; }
    bool playing() const { return timer_.isActive(); }
    double duration() const;
    double position() const { return position_; }
    double progress() const { return progress_; }
    QString status() const { return status_; }
    QString name() const { return record_->name; }
    QString speed() const;
    QVariantMap speedGauge() const;
    QVariantMap moviePreferences() const;
    Q_INVOKABLE void setSpeedGauge(const QVariantMap &settings);
    Q_INVOKABLE void setMoviePreferences(double speed, int audioIndex);
    QString cloudTopic() const { return record_->cloudTopic; }
    QString odometryTopic() const { return record_->odometryTopic; }
    QStringList cloudTopics() const { return record_->cloudTopics; }
    QStringList odometryTopics() const { return record_->odometryTopics; }
    QStringList speedTopics() const { return record_->speedTopics; }
    QString speedTopic() const { return record_->speedTopic; }
    QStringList scanTopics() const { return record_->scanTopics; }
    QString scanTopic() const { return record_->scanTopic; }
    double scanMaximumAge() const { return double(record_->scanMaximumAge) / 1.e9; }
    Q_INVOKABLE void setScanOptions(const QString &topic, double maximumAge);
    QString speedSelection() const;
    bool tfPose() const { return record_->tfPose; }
    QString worldFrame() const { return record_->frameId; }
    QString bodyFrame() const { return record_->bodyFrame; }
    int pointCount() const;
    bool replacing() const { return record_->replace; }
    bool demo() const { return path_.isEmpty() && (!project_ || project_->source() == "demo"); }
    QString projectPath() const { return project_ ? project_->path() : QString(); }
    QString bagPath() const { return path_; }
    std::shared_ptr<const replay::Recording> recording() const { return record_; }

    const std::vector<replay::VideoClip> &videoClips() const { return clips_; }
    bool referencesFile(const QString &path) const;
    const QVariantList &videoLayers() const { return videoLayers_; }
    QVariantList markers() const;
    QVariantList planningEvents() const;
    Q_INVOKABLE QVariantList alignmentCandidates() const;
    QVariantMap planningLayers() const;
    bool comparePlans() const { return project_ && project_->comparePlans(); }
    Q_INVOKABLE void setComparePlans(bool compare);
    bool planningLayerEnabled(const QString &role) const;
    Q_INVOKABLE void setPlanningLayerEnabled(const QString &role, bool isEnabled);
    QVariantList layoutCues() const { return layouts_.toVariantList(); }
    QVariantMap activeLayout() const;
    QStringList cameraLabels() const;
    int cameraIndex() const { return cameraIndex_; }
    Q_INVOKABLE void setCamera(int index);
    bool transparentCanvas() const { return project_ && project_->transparentCanvas(); }
    bool capturing() const { return capturing_; }
    int captureMatte() const { return captureMatte_; }
    Q_INVOKABLE void setTransparentCanvas(bool transparent);
    Q_INVOKABLE void exportFrame(const QUrl &url, QQuickItem *canvas);
    Q_INVOKABLE void setLayoutCue(const QString &mode, int videoIndex, double transition);
    Q_INVOKABLE void removeLayoutCue(double time);
    Q_INVOKABLE void addMarker();
    Q_INVOKABLE void renameMarker(double time, const QString &label);
    Q_INVOKABLE void removeMarker(double time);
    QVariantMap mainView() const;
    QVariantList layerMotions() const;
    Q_INVOKABLE QRectF animatedLayer(const QString &kind, const QString &id, double time,
                                    const QRectF &base, const QSizeF &canvas) const;
    Q_INVOKABLE void addLayerMotion(const QString &kind, const QString &id, double begin, double duration,
                                    double width, double x, double y, double normalWidth);
    Q_INVOKABLE void removeLayerMotion(int index);
    const QVariantList &cloudViews() const { return cloudViews_; }
    Q_INVOKABLE void addCloudView(int camera);
    Q_INVOKABLE void setCloudView(const QString &id, const QVariantMap &settings);
    Q_INVOKABLE void removeCloudView(const QString &id);
    const QVariantList &sceneLayers() const { return sceneLayers_; }
    Q_INVOKABLE void addSceneImage(const QUrl &file);
    Q_INVOKABLE void addSceneText(const QString &text);
    Q_INVOKABLE void editSceneLayer(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE void removeSceneLayer(const QString &id);
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
    Q_INVOKABLE void setFixedFrame(const QString &frame);
    Q_INVOKABLE void openRviz(const QUrl &url);
    Q_INVOKABLE void openBag(const QUrl &url);
    Q_INVOKABLE void openDemo();
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void saveProjectAs(const QUrl &url);
    Q_INVOKABLE void exportProjectFolder(const QUrl &parent);
    Q_INVOKABLE void setCloudTopic(const QString &topic);
    Q_INVOKABLE void setOdometryTopic(const QString &topic);
    Q_INVOKABLE void setSpeedTopic(const QString &topic);
    Q_INVOKABLE void setPoseSource(bool tf, const QString &world, const QString &body);
    Q_INVOKABLE void setReplacing(bool replace);
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void frameExported(bool success);
    void playingChanged();
    void positionChanged();
    void recordingChanged();
    void videoLayersChanged();
    void sceneLayersChanged();
    void cloudViewsChanged();
    void motionsChanged();
    void markersChanged();
    void layoutsChanged();
    void transparentCanvasChanged();
    void cameraChanged();
    void activeLayoutChanged();
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
    void refreshSceneLayers();
    void refreshCloudViews();
    void refreshMotions();
    void grabFrame(QQuickItem *canvas, const QString &path, int matte = 0, const QImage &black = {});
    void finishFrame(const QString &path, const QImage &image);
    void refreshLayoutCues();
    void addSceneLayer(const QString &kind, const QString &content);
    void changeMarker(double time, const std::optional<QString> &label);
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
    QVariantList videoLayers_, sceneLayers_, cloudViews_;
    QJsonArray layouts_, motions_;
    QSharedPointer<QQuickItemGrabResult> frameGrab_;
    QTimer timer_;
    QElapsedTimer clock_;
    QString path_, status_;
    std::optional<replay::MapMode> mapMode_;
    std::atomic_bool cancelRequested_ = false;
    bool loading_ = false, capturing_ = false;
    int captureMatte_ = 0, cameraIndex_ = 0;
    double position_ = 0., start_ = 0., progress_ = 0.;
};
