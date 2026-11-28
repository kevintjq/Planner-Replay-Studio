#pragma once

#include "project_document.h"
#include "replay_data.h"
#include "canvas_layer_model.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QQuickItem>
#include <QUrl>
#include <QVariantList>
#include <atomic>

class QQuickItemGrabResult;

// Keep decoder-owning QML delegates alive when another clip is added or removed.
class VideoLayerListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { VideoLayer = Qt::UserRole + 1 };
    explicit VideoLayerListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return {{VideoLayer, "videoLayer"}}; }
    void setLayers(const QVariantList &layers);
private:
    QVariantList layers_;
};

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
    Q_PROPERTY(QVariantList speedMeters READ speedMeters NOTIFY recordingChanged)
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
    Q_PROPERTY(QStringList tfFrames READ tfFrames NOTIFY recordingChanged)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY positionChanged)
    Q_PROPERTY(bool replacing READ replacing NOTIFY recordingChanged)
    Q_PROPERTY(bool demo READ demo NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList topicDisplays READ topicDisplays NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList independentDisplays READ independentDisplays NOTIFY independentDisplaysChanged)
    Q_PROPERTY(QVariantList independentDisplayTypes READ independentDisplayTypes CONSTANT)
    Q_PROPERTY(QString selectedIndependentDisplayId READ selectedIndependentDisplayId NOTIFY independentDisplaySelectionChanged)
    Q_PROPERTY(QVariantMap selectedIndependentDisplay READ selectedIndependentDisplay NOTIFY independentDisplaysChanged)
    Q_PROPERTY(bool independentDisplayLoading READ independentDisplayLoading NOTIFY independentDisplaysChanged)
    Q_PROPERTY(QString independentFixedFrame READ independentFixedFrame NOTIFY independentDisplaysChanged)
    Q_PROPERTY(bool independentDisplayModelActive READ independentDisplayModelActive NOTIFY independentDisplaysChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap nativeReplaySource READ nativeReplaySource NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap nativeRvizConfig READ nativeRvizConfig NOTIFY nativeConfigChanged)
    Q_PROPERTY(QString renderBackend READ renderBackend WRITE setRenderBackend NOTIFY renderBackendChanged)
    Q_PROPERTY(QString experimentStampNs READ experimentStampNs NOTIFY positionChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editHistoryChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editHistoryChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    Q_PROPERTY(QString rvizCompatibility READ rvizCompatibility NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList planningEvents READ planningEvents NOTIFY recordingChanged)
    Q_PROPERTY(bool comparePlans READ comparePlans NOTIFY recordingChanged)
    Q_PROPERTY(QVariantMap planningLayers READ planningLayers NOTIFY recordingChanged)
    Q_PROPERTY(QStringList cameraLabels READ cameraLabels CONSTANT)
    Q_PROPERTY(int cameraIndex READ cameraIndex NOTIFY cameraChanged)
    Q_PROPERTY(bool transparentCanvas READ transparentCanvas NOTIFY transparentCanvasChanged)
    Q_PROPERTY(QColor sceneBackground READ sceneBackground WRITE setSceneBackground NOTIFY sceneStyleChanged)
    Q_PROPERTY(bool capturing READ capturing NOTIFY stateChanged)
    Q_PROPERTY(int captureMatte READ captureMatte NOTIFY stateChanged)
    Q_PROPERTY(QVariantList layoutCues READ layoutCues NOTIFY layoutsChanged)
    Q_PROPERTY(QVariantMap activeLayout READ activeLayout NOTIFY activeLayoutChanged)
    Q_PROPERTY(QVariantList sceneLayers READ sceneLayers NOTIFY sceneLayersChanged)
    Q_PROPERTY(QVariantMap mainView READ mainView NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList layerMotions READ layerMotions NOTIFY motionsChanged)
    Q_PROPERTY(QVariantList cloudViews READ cloudViews NOTIFY cloudViewsChanged)
    Q_PROPERTY(QVariantList videoLayers READ videoLayers NOTIFY videoLayersChanged)
    Q_PROPERTY(QAbstractItemModel *videoLayerModel READ videoLayerModel CONSTANT)
    Q_PROPERTY(CanvasLayerModel *canvas READ canvas CONSTANT)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantMap pendingSourceRemoval READ pendingSourceRemoval NOTIFY sourcesChanged)
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
    QVariantList speedMeters() const;
    Q_INVOKABLE QString addSpeedMeter();
    Q_INVOKABLE void editSpeedMeter(const QString &id, const QString &section, const QVariantMap &changes);
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
    QStringList tfFrames() const;
    int pointCount() const;
    bool replacing() const { return record_->replace; }
    bool demo() const { return path_.isEmpty() && (project_ ? project_->source() == "demo" : demoMode_); }
    QString projectPath() const { return project_ ? project_->path() : QString(); }
    QString bagPath() const { return path_; }
    QVariantMap nativeReplaySource() const;
    QVariantMap nativeRvizConfig() const;
    Q_INVOKABLE void setNativeRvizConfig(const QString &yaml, const QVariantMap &metadata);
    QString renderBackend() const;
    void setRenderBackend(const QString &backend);
    QString experimentStampNs() const;
    std::shared_ptr<const replay::Recording> recording() const { return record_; }

    const std::vector<replay::VideoClip> &videoClips() const { return clips_; }
    bool referencesFile(const QString &path) const;
    const QVariantList &videoLayers() const { return videoLayers_; }
    QAbstractItemModel *videoLayerModel() { return &videoLayerModel_; }
    CanvasLayerModel *canvas() const { return canvas_; }
    QVariantList sources() const { return project_ ? project_->sources() : projectSnapshot().sources(); }
    QVariantMap pendingSourceRemoval() const { return pendingSourceRemoval_; }
    Q_INVOKABLE void removeLayers(const QStringList &ids);
    Q_INVOKABLE void clearCanvas();
    Q_INVOKABLE void addSource(const QUrl &file, const QString &kind);
    Q_INVOKABLE bool removeSources(const QStringList &ids, bool removeReferences = false);
    Q_INVOKABLE void cancelSourceRemoval();
    Q_INVOKABLE void clearUnusedSources();
    Q_INVOKABLE void renameSource(const QString &id, const QString &name);
    Q_INVOKABLE void replaceSource(const QString &id, const QUrl &file);
    Q_INVOKABLE void addSourceLayer(const QString &id);
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
    QColor sceneBackground() const;
    void setSceneBackground(const QColor &color);
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
    Q_INVOKABLE void setMainView(const QVariantMap &settings);
    Q_INVOKABLE void restoreMainView();
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
    QVariantList independentDisplays() const;
    QVariantList independentDisplayTypes() const;
    Q_INVOKABLE QVariantList compatibleIndependentDisplays(const QString &messageType) const;
    std::vector<replay::TopicDisplay> independentDisplayStates() const;
    std::vector<std::shared_ptr<const replay::Recording>> independentDisplayRecordings() const;
    bool independentDisplayModelActive() const;
    QString independentFixedFrame() const;
    bool independentDisplayLoading() const { return independentWatcher_.isRunning(); }
    QString selectedIndependentDisplayId() const { return selectedIndependentDisplayId_; }
    QVariantMap selectedIndependentDisplay() const;
    Q_INVOKABLE void selectIndependentDisplay(const QString &id);
    Q_INVOKABLE QString addIndependentDisplay(const QString &pluginClass, const QString &topic = {}, const QString &parentId = {});
    Q_INVOKABLE QString duplicateIndependentDisplay(const QString &id);
    Q_INVOKABLE void removeIndependentDisplays(const QStringList &ids);
    Q_INVOKABLE bool editIndependentDisplay(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE QVariantList independentDisplayProperties(const QString &id) const;
    Q_INVOKABLE void retryIndependentDisplays();
    const QHash<QString, bool> &topicDisplayVisibility() const { return displayVisibility_; }
    Q_INVOKABLE void setTopicDisplayEnabled(int index, bool enabled);
    Q_INVOKABLE void setFixedFrame(const QString &frame);
    Q_INVOKABLE void openRviz(const QUrl &url);
    Q_INVOKABLE void exportRviz(const QUrl &url);
    QString rvizCompatibility() const;
    Q_INVOKABLE void openBag(const QUrl &url);
    Q_INVOKABLE void openDemo();
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void saveProjectAs(const QUrl &url);
    Q_INVOKABLE void exportProjectFolder(const QUrl &parent);
    void setTrajectorySettings(const QVariantMap &settings);
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
    void sceneStyleChanged();
    void cameraChanged();
    void activeLayoutChanged();
    void editHistoryChanged();
    void trajectorySettingsLoaded(const QVariantMap &settings);
    void sourcesChanged();
    void sourceRemoved(const QString &kind, const QUrl &file);
    void nativeConfigChanged();
    void nativeConfigLoaded(const QVariantMap &config);
    void renderBackendChanged();
    void independentDisplaysChanged();
    void independentDisplaySelectionChanged();

private:
    friend class CanvasLayerModel;
    void recordProjectEdit(const replay::ProjectDocument &before,
        std::shared_ptr<const replay::Recording> beforeRecord = {}, const QString &beforePath = {});
    void refreshProjectModels();
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
    void refreshDisplayVisibility();
    replay::RvizLayout independentLayout() const;
    void storeIndependentLayout(const replay::RvizLayout &layout, const QString &message = {});
    void refreshIndependentDisplayData();
    void ensureInitialDisplayLayout();
    QString independentCacheKey(const replay::TopicDisplay &display) const;
    struct IndependentDisplayLoad {
        QString source, fixedFrame, error;
        quint64 generation = 0;
        QStringList keys;
        QHash<QString, std::shared_ptr<const replay::Recording>> records;
    };
    QFutureWatcher<IndependentDisplayLoad> independentWatcher_;
    QTimer independentStatusTimer_;
    QVariantList independentTimeStatuses_;
    QHash<QString, std::shared_ptr<const replay::Recording>> independentData_;
    QHash<QString, QString> independentErrors_;
    QSet<QString> independentPending_;
    QString independentDataSource_, independentDataFrame_, selectedIndependentDisplayId_;
    quint64 independentDataGeneration_ = 0;
    quint64 independentSourceGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> independentCancel_;
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
        replay::ProjectDocument before, after;
        std::shared_ptr<const replay::Recording> beforeRecord, afterRecord;
        QString beforePath, afterPath;
    };
    void restoreVideoEdit(bool redo);

    QHash<QString, double> videoDurations_;
    QHash<QString, bool> displayVisibility_;
    std::vector<VideoEdit> undo_, redo_;
    CanvasLayerModel *canvas_ = nullptr;
    int projectEditDepth_ = 0;
    QVariantMap pendingSourceRemoval_;
    std::optional<replay::ProjectDocument> replacementBefore_;
    QVariantList replacementRemovedSources_;

    std::shared_ptr<const replay::Recording> record_;
    quint64 recordingGeneration_ = 0;
    QFutureWatcher<Result> watcher_;
    QFutureWatcher<QString> folderWatcher_;
    std::optional<replay::ProjectDocument> project_;
    std::vector<replay::VideoClip> clips_;
    VideoLayerListModel videoLayerModel_;
    QVariantList videoLayers_, sceneLayers_, cloudViews_;
    QJsonArray layouts_, motions_;
    QSharedPointer<QQuickItemGrabResult> frameGrab_;
    QTimer timer_;
    QElapsedTimer clock_;
    QString path_, status_;
    std::optional<replay::MapMode> mapMode_;
    std::atomic_bool cancelRequested_ = false;
    bool loading_ = false, capturing_ = false, demoMode_ = false;
    int captureMatte_ = 0, cameraIndex_ = 0;
    double position_ = 0., start_ = 0., progress_ = 0.;
};
