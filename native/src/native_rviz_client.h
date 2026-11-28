#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>
#include <QJsonObject>
#include <QSet>
#include <QHash>
#include <QImage>
#include <QUrl>
#include <QElapsedTimer>
#include <QJsonArray>

// Session undo data only: never retain a RenderPanel, controller or GPU object.
// 4096 identities cover the controller's 200 edits of at most 16 view slots.
class NativeViewArchive {
public:
    explicit NativeViewArchive(int maximumEntries = 4096, qsizetype maximumBytes = 64 * 1024 * 1024)
        : maximumEntries_(maximumEntries), maximumBytes_(maximumBytes) {}
    void remember(const QJsonArray &views);
    QJsonObject value(const QString &id) const { return entries_.value(id); }
    QVariantMap classes() const;
    QVariantMap state() const;
    void clear();
private:
    QHash<QString, QJsonObject> entries_;
    QStringList order_;
    qsizetype bytes_ = 0;
    int maximumEntries_;
    qsizetype maximumBytes_;
};

// One shared model for both property panels; values come only from librviz.
class NativeRvizClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantList properties READ properties NOTIFY modelChanged)
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY changed)
    Q_PROPERTY(QVariantList tools READ tools NOTIFY changed)
    Q_PROPERTY(QVariantList viewControllers READ viewControllers NOTIFY changed)
    Q_PROPERTY(QString activeToolClass READ activeToolClass NOTIFY changed)
    Q_PROPERTY(QVariantList topics READ topics NOTIFY changed)
    Q_PROPERTY(QStringList displayTopics READ displayTopics NOTIFY modelChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY modelChanged)
    Q_PROPERTY(QString selectedId READ selectedId WRITE select NOTIFY modelChanged)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY modelChanged)
    Q_PROPERTY(QString defaultLauncher READ defaultLauncher CONSTANT)
    Q_PROPERTY(QString preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(QString previewTargetNs READ previewTargetNs NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap viewPreviews READ viewPreviews NOTIFY previewChanged)
    Q_PROPERTY(QVariantList renderViews READ renderViews NOTIFY modelChanged)
    Q_PROPERTY(QVariantMap archivedViewClasses READ archivedViewClasses NOTIFY modelChanged)
    Q_PROPERTY(QVariantMap viewArchiveState READ viewArchiveState NOTIFY modelChanged)
    Q_PROPERTY(bool independentCameras READ independentCameras NOTIFY modelChanged)
    Q_PROPERTY(QString clockNs READ clockNs NOTIFY previewChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY changed)
    Q_PROPERTY(QString distribution READ distribution WRITE setDistribution NOTIFY preferencesChanged)
    Q_PROPERTY(QString launcher READ launcher WRITE setLauncher NOTIFY preferencesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool indexed READ indexed NOTIFY changed)
    Q_PROPERTY(bool synchronized READ synchronized NOTIFY previewChanged)
    Q_PROPERTY(bool sceneReady READ sceneReady NOTIFY previewChanged)
    Q_PROPERTY(QString targetNs READ targetNs NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap replayResult READ replayResult NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(bool sourceAvailable READ sourceAvailable NOTIFY changed)
    Q_PROPERTY(QVariantMap nativeResources READ nativeResources NOTIFY changed)
    Q_PROPERTY(bool detecting READ detecting NOTIFY changed)
    Q_PROPERTY(QString runtimeState READ runtimeState NOTIFY changed)
    Q_PROPERTY(QVariantMap runtimeInfo READ runtimeInfo NOTIFY changed)
    Q_PROPERTY(int reconnectAttempt READ reconnectAttempt NOTIFY changed)
public:
    explicit NativeRvizClient(QObject *parent = nullptr);
    ~NativeRvizClient() override;
    bool running() const { return process_.state() != QProcess::NotRunning; }
    bool connected() const { return connected_; }
    QString status() const { return status_; }
    QVariantList properties() const { return rows_; }
    QVariantList plugins() const { return plugins_; }
    QVariantList tools() const { return tools_; }
    QVariantList viewControllers() const { return viewControllers_; }
    QString activeToolClass() const { return activeToolClass_; }
    QVariantList topics() const { return topics_; }
    QStringList displayTopics() const { return displayTopics_; }
    QVariantMap selected() const;
    QString selectedId() const { return selected_; }
    QString search() const { return search_; }
    void setSearch(const QString &text);
    QString defaultLauncher() const;
    QString preview() const { return preview_; }
    QString previewTargetNs() const { return previewTarget_; }
    QVariantMap viewPreviews() const { return viewPreviews_; }
    QVariantList renderViews() const;
    QVariantMap archivedViewClasses() const { return viewArchive_.classes(); }
    QVariantMap viewArchiveState() const { return viewArchive_.state(); }
    bool independentCameras() const { return tree_.value("independent_cameras").toBool(); }
    QString clockNs() const { return clock_; }
    QString diagnostic() const { return diagnostic_; }
    QString distribution() const { return distribution_; }
    void setDistribution(const QString &value);
    QString launcher() const { return launcher_; }
    void setLauncher(const QString &value);
    bool busy() const;
    bool indexed() const { return indexed_; }
    bool synchronized() const { return synchronized_; }
    bool sceneReady() const { return sceneReady_; }
    QString targetNs() const { return targetNs_; }
    QVariantMap replayResult() const { return replayResult_; }
    bool canUndo() const { return canUndo_; }
    bool canRedo() const { return canRedo_; }
    bool sourceAvailable() const { return sourceAvailable_; }
    QVariantMap nativeResources() const { return nativeResources_; }
    bool detecting() const { return detecting_; }
    QString runtimeState() const { return runtimeState_; }
    QVariantMap runtimeInfo() const { return runtimeInfo_; }
    int reconnectAttempt() const { return reconnectAttempt_; }
    Q_INVOKABLE void ensureStarted(const QUrl &config = {});
    Q_INVOKABLE void retryConnection();
    Q_INVOKABLE void start(const QUrl &launcher, const QString &distribution, const QUrl &config = {});
    Q_INVOKABLE void stop();
    Q_INVOKABLE void select(const QString &id);
    Q_INVOKABLE void toggleExpanded(const QString &id);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setPropertyValue(const QString &id, const QVariant &value);
    Q_INVOKABLE void loadConfig(const QUrl &file);
    Q_INVOKABLE void loadConfigYaml(const QString &yaml);
    Q_INVOKABLE void clearPendingConfig();
    Q_INVOKABLE void saveConfig(const QUrl &file);
    Q_INVOKABLE QString snapshotConfig();
    Q_INVOKABLE void addDisplay(const QString &plugin, const QString &name, const QString &topic = {}, const QString &parentId = {});
    Q_INVOKABLE void moveDisplay(const QString &id, const QString &parentId, int index = -1);
    Q_INVOKABLE void cameraMouse(const QString &type, double x, double y, int button, int buttons, int modifiers, int wheelDelta = 0);
    Q_INVOKABLE void cameraMouseForView(const QString &viewId, const QString &type, double x, double y, int button, int buttons, int modifiers, int wheelDelta = 0);
    Q_INVOKABLE void syncViews(const QVariantList &views);
    Q_INVOKABLE void activateTool(const QString &plugin);
    Q_INVOKABLE void setViewController(const QString &plugin);
    Q_INVOKABLE void resetCamera();
    Q_INVOKABLE void resetCameraForView(const QString &viewId);
    Q_INVOKABLE void displayAction(const QString &command, const QString &id, const QString &name = {});
    Q_INVOKABLE void removeDisplays(const QStringList &ids);
    Q_INVOKABLE void undoNative();
    Q_INVOKABLE void redoNative();
    Q_INVOKABLE void refreshResources();
    Q_INVOKABLE void openBag(const QUrl &file);
    Q_INVOKABLE void closeBag();
    Q_INVOKABLE void playBag(double rate);
    Q_INVOKABLE void stopBag();
    Q_INVOKABLE void requestPreview();
    Q_INVOKABLE void requestViewPreview(const QString &viewId);
    Q_INVOKABLE QVariantList compatiblePlugins(const QString &messageType) const;
    Q_INVOKABLE void indexBag(const QUrl &file, const QString &timeBasis,
                              const QString &untimedPolicy, const QVariantMap &options = {});
    Q_INVOKABLE void seekAbsolute(const QString &nanoseconds);
    Q_INVOKABLE void advanceAbsolute(const QString &nanoseconds);
    Q_INVOKABLE void cancelReplay();
    Q_INVOKABLE void cancelPendingSeek();
    Q_INVOKABLE QString prepareOutputFrame(const QString &targetNs, int width, int height);
    Q_INVOKABLE QString prepareViewOutputFrame(const QString &viewId, const QString &targetNs, int width, int height);
    QString prepareOutputFrames(const QString &targetNs, const QVariantList &views);
    Q_INVOKABLE void cancelOutputFrame(const QString &jobId);
signals:
    void changed();
    void modelChanged();
    void previewChanged();
    void preferencesChanged();
    void commandFinished(const QString &command, bool success, const QString &error);
    void commandSent(const QString &command);
    void outputFramePrepared(const QString &jobId, bool success, const QString &targetNs,
                             const QImage &frame, const QVariantMap &metadata, const QString &error);
    void outputViewFramePrepared(const QString &jobId, const QString &viewId, const QString &targetNs,
                                 const QImage &frame, const QVariantMap &metadata);
    void outputFramesPrepared(const QString &jobId, bool success, const QString &targetNs,
                              const QVariantList &metadata, const QString &error);
    void configSnapshotReady(const QString &jobId, bool success, const QString &yaml,
                             const QVariantMap &metadata, const QString &error);
private:
    QString nativePath(const QUrl &url) const;
    bool send(const QString &command, QJsonObject arguments = {}, QJsonObject context = {});
    void receive();
    void rebuild();
    void failProtocol(const QString &error);
    void clearConnection();
    void expireRequests();
    void stopOwnedSession();
    void finishOutput(bool success, const QImage &frame = {}, const QVariantMap &metadata = {}, const QString &error = {});
    QString prepareFrames(const QString &targetNs, const QVariantList &views, bool batch);
    void completeOutputFrame(const QImage &frame, const QVariantMap &metadata);
    void requestTime(const QString &nanoseconds, bool incremental);
    void queuePropertyPreview(const QString &viewId);
    void beginDetection();
    void detectNextRuntime();
    void detectionFinished(int exitCode);
    void runtimeUnavailable(const QString &code, const QString &message);
    void launchHost(const QUrl &launcher, const QString &distribution, const QUrl &config, bool automatic);
    void becameConnected();
    void scheduleReconnect();
    void cacheConfiguration();
    QProcess process_;
    QProcess cleanupProcess_;
    QProcess detectionProcess_;
    QTimer detectionDeadline_, reconnectTimer_;
    bool detecting_ = false, automatic_ = false, desiredRunning_ = false, reconnecting_ = false;
    bool listingDistributions_ = false, restoringConfiguration_ = false;
    bool cachePending_ = false, cacheDirty_ = false;
    bool restartRequested_ = false;
    int reconnectAttempt_ = 0, runtimeCandidate_ = -1;
    QString runtimeState_ = "idle", runtimeProgram_, recoveryYaml_;
    QString pendingInitialYaml_;
    QUrl pendingInitialFile_;
    QVariantMap runtimeInfo_;
    QList<QPair<QString, QString>> runtimeCandidates_;
    QStringList runtimeLaunchers_;
    QUrl initialConfig_;
    QByteArray input_;
    bool connected_ = false;
    QString status_, selected_, search_, preview_, clock_, diagnostic_, distribution_, launcher_;
    QVariantList rows_, plugins_, topics_;
    QVariantList tools_, viewControllers_;
    QString activeToolClass_;
    QStringList displayTopics_;
    QJsonObject tree_;
    QHash<QString, QVariantMap> nodes_;
    QHash<qint64, QJsonObject> requests_;
    QHash<qint64, QJsonObject> contexts_;
    QHash<qint64, qint64> sentAt_;
    QHash<QString, QVariantList> options_;
    QSet<QString> expanded_;
    qint64 nextRequest_ = 0;
    bool treePending_ = false, framePending_ = false, refreshQueued_ = false;
    QTimer deadline_;
    QTimer requestDeadline_, stopDeadline_;
    QTimer propertyPreviewTimer_;
    QSet<QString> propertyPreviews_;
    QElapsedTimer elapsed_;
    bool stopping_ = false;
    QString sessionToken_;
    bool replayBusy_ = false, indexed_ = false, dataSynchronized_ = false, synchronized_ = false, sceneReady_ = false;
    bool sourceClosing_ = false, canUndo_ = false, canRedo_ = false, sourceAvailable_ = true;
    int topicSetupsPending_ = 0;
    QString targetNs_, queuedTarget_;
    QString previewTarget_;
    bool queuedAdvance_ = false;
    QString outputJob_, outputTarget_, outputView_;
    int outputWidth_ = 0, outputHeight_ = 0;
    bool outputBatch_ = false;
    QVariantList outputViews_, outputMetadata_;
    qsizetype outputViewIndex_ = 0;
    QString outputSceneRevision_;
    QVariantMap replayResult_;
    QVariantMap nativeResources_;
    QVariantMap viewPreviews_;
    NativeViewArchive viewArchive_;
};
