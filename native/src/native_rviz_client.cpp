#include "native_rviz_client.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QRegularExpression>
#include <QImageReader>
#include <QBuffer>
#include <QSet>
#include <QUuid>
#include <QStandardPaths>
#include <cmath>
#include <algorithm>
#include <functional>

namespace {
constexpr qsizetype ResponseLimit = 32 * 1024 * 1024;
constexpr qsizetype RequestLimit = 4 * 1024 * 1024;
constexpr qint64 RequestTimeout = 20000;
bool isConfigurationRequest(const QString &command)
{
    return command == "load" || command == "load_yaml" || command == "undo" || command == "redo";
}
bool validateTree(const QJsonObject &tree)
{
    QSet<QString> ids;
    qsizetype count = 0;
    std::function<bool(const QJsonObject &, int)> check = [&](const QJsonObject &node, int depth) {
        const auto id = node.value("id").toString();
        if (depth > 64 || ++count > 100000 || id.isEmpty() || ids.contains(id) ||
            !node.value("children").isArray()) return false;
        ids.insert(id);
        for (const auto &child : node.value("children").toArray())
            if (!child.isObject() || !check(child.toObject(), depth + 1)) return false;
        return true;
    };
    for (const auto &root : {"displays", "views", "tools"})
        if (!tree.value(root).isObject() || !check(tree.value(root).toObject(), 0)) return false;
    const auto views=tree.value("render_views");QSet<QString> viewIds;
    if(!views.isUndefined()&&!views.isArray())return false;
    if(views.toArray().size()>16)return false;
    for(const auto &entry:views.toArray()) {
        const auto view=entry.toObject();const auto viewId=view.value("view_id").toString();
        if(!entry.isObject()||viewId.isEmpty()||viewId=="main"||viewIds.contains(viewId)||!check(view.value("controller").toObject(),0))return false;
        viewIds.insert(viewId);
    }
    return true;
}
QJsonObject findTopic(const QJsonObject &node, const QString &type, int &count)
{
    QJsonObject result;
    if (node.value("native_type").toString().endsWith("RosTopicProperty") &&
        (type.isEmpty() || node.value("message_type").toString() == type)) {
        ++count; result = node;
    }
    for (const auto &child : node.value("children").toArray()) {
        const auto candidate = findTopic(child.toObject(), type, count);
        if (!candidate.isEmpty()) result = candidate;
    }
    return result;
}
}

void NativeViewArchive::remember(const QJsonArray &views)
{
    for (const auto &entry : views) {
        const auto view = entry.toObject();
        const auto id = view.value("view_id").toString(), plugin = view.value("class").toString();
        const auto yaml = view.value("controller_yaml").toString();
        if (id.isEmpty() || id == "main" || id.size() > 100 || plugin.isEmpty() || plugin.size() > 512
            || yaml.isEmpty() || yaml.toUtf8().size() > RequestLimit) continue;
        bytes_ -= entries_.value(id).value("controller_yaml").toString().size() * qsizetype(sizeof(QChar));
        entries_.insert(id, QJsonObject{{"view_id", id}, {"class", plugin}, {"controller_yaml", yaml}});
        bytes_ += yaml.size() * qsizetype(sizeof(QChar));
        order_.removeAll(id); order_.append(id);
    }
    while (order_.size() > std::max(0, maximumEntries_)) {
        const auto oldest = order_.takeFirst();
        bytes_ -= entries_.take(oldest).value("controller_yaml").toString().size() * qsizetype(sizeof(QChar));
    }
    for (const auto &id : order_) {
        if (bytes_ <= std::max(qsizetype(0), maximumBytes_)) break;
        auto &view = entries_[id];
        bytes_ -= view.value("controller_yaml").toString().size() * qsizetype(sizeof(QChar));
        // Keep a bounded class tombstone so an exhausted snapshot budget fails
        // explicitly on Undo instead of silently constructing a default camera.
        view.insert("controller_yaml", QString());
    }
}
QVariantMap NativeViewArchive::classes() const
{
    QVariantMap result;
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) result.insert(it.key(), it.value().value("class").toString());
    return result;
}
QVariantMap NativeViewArchive::state() const
{
    int incomplete = 0;
    for (const auto &entry : entries_) if (entry.value("controller_yaml").toString().isEmpty()) ++incomplete;
    return {{"entries", entries_.size()}, {"yaml_storage_bytes", bytes_}, {"incomplete_snapshots", incomplete},
        {"maximum_entries", maximumEntries_}, {"maximum_bytes", maximumBytes_}};
}
void NativeViewArchive::clear() { entries_.clear(); order_.clear(); bytes_ = 0; }

NativeRvizClient::NativeRvizClient(QObject *parent) : QObject(parent)
{
    propertyPreviewTimer_.setSingleShot(true);
    connect(&propertyPreviewTimer_,&QTimer::timeout,this,[this]{
        if(!connected_||stopping_||propertyPreviews_.isEmpty())return;
        if(framePending_||busy()){propertyPreviewTimer_.start(25);return;}
        const auto viewId=*propertyPreviews_.cbegin();propertyPreviews_.remove(viewId);
        framePending_=send("frame",{{"view_id",viewId},{"max_width",960}},{{"property_preview",true}});
        if(!propertyPreviews_.isEmpty())propertyPreviewTimer_.start(25);
    });
    QSettings settings;
    distribution_ = settings.value("nativeRviz/distribution").toString();
    launcher_ = settings.value("nativeRviz/launcher").toString();
    if (launcher_.isEmpty()) launcher_ = defaultLauncher();
    runtimeInfo_ = {{"optional", true}, {"independentBackendAvailable", true},
        {"affectedCapabilities", QStringList{"nativeDisplayPlugins", "nativeRvizProperties", "rvizNativeRendering"}}};
#ifdef Q_OS_WIN
    for (auto *process : {&process_, &cleanupProcess_, &detectionProcess_})
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; });
#endif
    status_ = tr("未连接原生 RViz。 / Native RViz disconnected.");
    elapsed_.start();
    connect(&process_, &QProcess::started, this, [this] {
        runtimeInfo_.insert("clientProcessId", QVariant::fromValue<qint64>(process_.processId())); emit changed();
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &NativeRvizClient::receive);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        diagnostic_ = (diagnostic_ + QString::fromUtf8(process_.readAllStandardError())).right(16000); emit changed();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        status_ = process_.errorString();
        if (error == QProcess::FailedToStart) { deadline_.stop(); clearConnection(); scheduleReconnect(); }
        emit changed();
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        deadline_.stop(); stopDeadline_.stop(); clearConnection();
        if (stopping_) status_ = tr("原生 RViz 已退出（%1）。 / Native RViz stopped.").arg(code);
        else status_ += tr("\n原生进程已退出（%1）。 / Native process exited.").arg(code);
        if (!stopping_) stopOwnedSession();
        stopping_ = false;
        if (desiredRunning_ && automatic_) scheduleReconnect();
        else { if (runtimeState_ != "unavailable") runtimeState_ = "idle"; emit changed(); }
    });
    deadline_.setSingleShot(true); deadline_.setInterval(30000);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        if (!connected_) {
            status_ = tr("可选原生 RViz 启动超时；独立 C++ 功能仍可使用。 / Optional native RViz startup timed out.");
            diagnostic_ += '\n' + status_;
            if (automatic_ && desiredRunning_) { process_.terminate(); QTimer::singleShot(2000, this, [this] { if (running() && !connected_) process_.kill(); }); }
            else stop();
        }
    });
    stopDeadline_.setSingleShot(true); stopDeadline_.setInterval(8000);
    connect(&stopDeadline_, &QTimer::timeout, this, [this] {
        if (running()) {
            diagnostic_ += tr("\n正常关闭超时，正在清理本客户端创建的原生 ROS 会话。\n");
            stopOwnedSession();
            QTimer::singleShot(8000, this, [this] {
                if (stopping_ && running()) {
                    process_.terminate();
                    QTimer::singleShot(2000, this, [this] { if (stopping_ && running()) process_.kill(); });
                }
            });
            emit changed();
        }
    });
    requestDeadline_.setInterval(500);
    connect(&requestDeadline_, &QTimer::timeout, this, &NativeRvizClient::expireRequests);
    detectionDeadline_.setSingleShot(true); detectionDeadline_.setInterval(20000);
    connect(&detectionDeadline_, &QTimer::timeout, this, [this] {
        diagnostic_ = (diagnostic_ + tr("\n原生环境检测超时。 / Native runtime detection timed out.\n")).right(16000);
        detectionProcess_.kill();
    });
    connect(&detectionProcess_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { detectionFinished(code); });
    connect(&detectionProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && detecting_) runtimeUnavailable("runtime_process_failed", detectionProcess_.errorString());
    });
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, [this] {
        if (!desiredRunning_ || !automatic_) return;
        if (cleanupProcess_.state() != QProcess::NotRunning) { reconnectTimer_.start(500); return; }
        beginDetection();
    });
}
NativeRvizClient::~NativeRvizClient()
{
    desiredRunning_ = false; reconnectTimer_.stop(); detectionDeadline_.stop();
    if (detectionProcess_.state() != QProcess::NotRunning) { detectionProcess_.kill(); detectionProcess_.waitForFinished(1000); }
    if (running()) {
        if (process_.state() == QProcess::Running) send("shutdown");
        process_.closeWriteChannel();
        if (!process_.waitForFinished(8000)) {
            stopOwnedSession();
            cleanupProcess_.waitForFinished(3000);
            if (!process_.waitForFinished(8000)) {
                process_.terminate();
                if (!process_.waitForFinished(2000)) { process_.kill(); process_.waitForFinished(1000); }
            }
        }
    }
}
void NativeRvizClient::stopOwnedSession()
{
    if (sessionToken_.isEmpty() || cleanupProcess_.state() != QProcess::NotRunning) return;
    const auto script = nativePath(QUrl(launcher_));
    if (script.isEmpty()) return;
    QStringList arguments{script, "--stop-session", sessionToken_};
#ifdef Q_OS_WIN
    arguments = QStringList{"-d", distribution_, "--", "bash"} + arguments;
    cleanupProcess_.start("wsl.exe", arguments);
#else
    cleanupProcess_.start("bash", arguments);
#endif
}
void NativeRvizClient::clearConnection()
{
    if(!outputJob_.isEmpty())finishOutput(false,{},{},tr("原生 RViz 已断开，无法完成图片输出。"));
    for(const auto &context:contexts_)if(context.contains("snapshot_job"))
        emit configSnapshotReady(context.value("snapshot_job").toString(),false,{},{},tr("原生 RViz 已断开，未取得最新配置快照。"));
    connected_ = treePending_ = framePending_ = refreshQueued_ = false;
    cachePending_ = cacheDirty_ = restoringConfiguration_ = false;
    replayBusy_ = indexed_ = dataSynchronized_ = synchronized_ = sceneReady_ = false; targetNs_.clear(); queuedTarget_.clear(); replayResult_.clear(); displayTopics_.clear();
    propertyPreviewTimer_.stop();propertyPreviews_.clear();
    topicSetupsPending_=0;
    sourceClosing_=canUndo_=canRedo_=false;sourceAvailable_=true;nativeResources_.clear();
    requests_.clear(); contexts_.clear(); sentAt_.clear(); requestDeadline_.stop();
    tree_ = {}; rows_.clear(); nodes_.clear(); options_.clear(); selected_.clear(); expanded_.clear(); plugins_.clear(); topics_.clear(); preview_.clear(); previewTarget_.clear(); clock_.clear();
    tools_.clear(); viewControllers_.clear(); activeToolClass_.clear();viewPreviews_.clear();
    emit modelChanged(); emit previewChanged();
}
QString NativeRvizClient::defaultLauncher() const
{
    QDir directory(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 6; ++level) {
        for (const auto &path : {QStringLiteral("native/rviz_host/run_host.sh"), QStringLiteral("rviz_host/run_host.sh")}) {
            const auto candidate = directory.filePath(path);
            if (QFileInfo(candidate).isFile()) return QUrl::fromLocalFile(candidate).toString();
        }
        directory.cdUp();
    }
    return {};
}
void NativeRvizClient::ensureStarted(const QUrl &config)
{
    if (!config.isEmpty() && (!config.isLocalFile() || !QFileInfo(config.toLocalFile()).isFile())) {
        if(!running()&&!detecting_)runtimeUnavailable("configuration_missing", tr("指定的原生配置文件不可读取；未用默认配置替代。 / Native configuration file is unavailable.")); return;
    }
    if (!connected_ && !config.isEmpty() && config != initialConfig_) {
        initialConfig_ = config; recoveryYaml_.clear(); pendingInitialYaml_.clear(); viewArchive_.clear();
        if(running())pendingInitialFile_=config;
    }
    if (connected_ || running() || detecting_ || reconnectTimer_.isActive()) return;
    if (runtimeState_ == "unavailable") return;
    automatic_ = desiredRunning_ = true; reconnecting_ = !recoveryYaml_.isEmpty(); reconnectAttempt_ = 0;
    beginDetection();
}
void NativeRvizClient::retryConnection()
{
    automatic_ = desiredRunning_ = true; reconnectAttempt_ = 0;
    if (detecting_) return;
    if (connected_) {
        // Capture acknowledged edits before an explicit restart; never replace
        // them with the original file or the host's default configuration.
        restartRequested_ = true; cacheConfiguration(); return;
    }
    if (running()) return;
    reconnectTimer_.stop(); reconnecting_ = !recoveryYaml_.isEmpty(); beginDetection();
}
void NativeRvizClient::runtimeUnavailable(const QString &code, const QString &message)
{
    detecting_ = false; detectionDeadline_.stop(); runtimeState_ = "unavailable";
    runtimeInfo_.insert("available", false); runtimeInfo_.insert("code", code); runtimeInfo_.insert("message", message);
    status_ = tr("可选原生 RViz 不可用：%1\n独立 C++ 录包、点云、视频和制图功能仍可使用。 / Optional native RViz unavailable; standalone features remain available.").arg(message);
    diagnostic_ = (diagnostic_ + '\n' + message).right(16000); emit changed();
}
void NativeRvizClient::beginDetection()
{
    if (!desiredRunning_ || running() || detectionProcess_.state() != QProcess::NotRunning) return;
    const auto manifestPath = QDir(QCoreApplication::applicationDirPath()).filePath("native-rviz-backend.json");
    if (QFileInfo::exists(manifestPath)) {
        QFile manifest(manifestPath);
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 65536) {
            runtimeUnavailable("package_manifest_invalid", tr("可选原生后端包清单不可读取；独立功能仍可使用。 / Native runtime package manifest is unreadable.")); return;
        }
        const auto metadata=QJsonDocument::fromJson(manifest.readAll()).object();
        if (metadata.isEmpty()) { runtimeUnavailable("package_manifest_invalid", tr("可选原生后端包清单无效。 / Native runtime package manifest is invalid.")); return; }
        const auto profile=metadata.value("package_profile").toString(metadata.value("profile").toString());
        runtimeInfo_.insert("packageProfile",profile);
        if (!metadata.value("included").toBool(true) || profile.compare("Standalone",Qt::CaseInsensitive)==0
            || (metadata.contains("enabled")&&!metadata.value("enabled").toBool()) || metadata.value("mode").toString()=="standalone") {
            runtimeUnavailable("native_not_packaged",tr("此独立版安装包未启用可选原生 RViz；无需 WSL 或 ROS 即可使用核心功能。 / Standalone package: optional native RViz is disabled.")); return;
        }
    }
    detecting_ = true; runtimeState_ = reconnecting_ ? "reconnecting" : "detecting";
    status_ = tr("正在自动检测可选原生 RViz 环境… / Detecting optional native RViz runtime…");
    runtimeInfo_.insert("available", false); runtimeLaunchers_.clear(); runtimeCandidates_.clear(); runtimeCandidate_ = -1;
    for (const auto &candidate : {defaultLauncher(), launcher_}) {
        const QUrl url(candidate);
        if (!candidate.isEmpty() && url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            if (!runtimeLaunchers_.contains(candidate)) runtimeLaunchers_.append(candidate);
        } else if (!candidate.isEmpty()) diagnostic_ = (diagnostic_ + tr("\n旧启动路径不可用，自动尝试随包路径。 / Stale launcher preference; trying bundled runtime.\n")).right(16000);
    }
    if (runtimeLaunchers_.isEmpty()) { runtimeUnavailable("launcher_missing", tr("安装中未找到可选原生 RViz 宿主启动组件。 / Native host payload was not found.")); return; }
#ifdef Q_OS_WIN
    runtimeProgram_ = QStandardPaths::findExecutable("wsl.exe");
    if (runtimeProgram_.isEmpty()) { runtimeUnavailable("wsl_missing", tr("系统未安装 WSL；仅原生 RViz 增强功能需要它。 / WSL is not installed.")); return; }
    listingDistributions_ = true;
    detectionProcess_.start(runtimeProgram_, {"--list", "--quiet"});
    detectionDeadline_.start(); emit changed();
#else
    runtimeProgram_ = QStandardPaths::findExecutable("bash");
    if (runtimeProgram_.isEmpty()) { runtimeUnavailable("bash_missing", tr("未找到原生 ROS 启动所需的 Bash。 / Bash is unavailable.")); return; }
    listingDistributions_ = false;
    for (const auto &script : runtimeLaunchers_) runtimeCandidates_.append(qMakePair(script, QString()));
    detectNextRuntime();
#endif
}
void NativeRvizClient::detectNextRuntime()
{
    if (!detecting_ || !desiredRunning_) return;
    if (++runtimeCandidate_ >= runtimeCandidates_.size()) {
        const auto code = runtimeInfo_.value("code", "ros_runtime_missing").toString();
        runtimeUnavailable(code, runtimeInfo_.value("message", tr("已安装环境中未找到兼容的 ROS Noetic / RViz 及宿主依赖。 / No compatible ROS Noetic/RViz runtime was found.")).toString()); return;
    }
    const auto candidate = runtimeCandidates_[runtimeCandidate_];
    distribution_ = candidate.second;
    const auto script = nativePath(QUrl(candidate.first));
    if (script.isEmpty()) { detectNextRuntime(); return; }
    QStringList args{script, "--hidden", "--probe"};
#ifdef Q_OS_WIN
    args = QStringList{"-d", distribution_, "--", "bash"} + args;
#endif
    detectionProcess_.start(runtimeProgram_, args); detectionDeadline_.start(); emit changed();
}
void NativeRvizClient::detectionFinished(int exitCode)
{
    detectionDeadline_.stop();
    const auto bytes = detectionProcess_.readAllStandardOutput();
    diagnostic_ = (diagnostic_ + QString::fromUtf8(detectionProcess_.readAllStandardError())).right(16000);
    if (!detecting_ || !desiredRunning_) return;
    if (listingDistributions_) {
        listingDistributions_ = false;
        QString output;
        if (bytes.contains('\0')) {
            for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) output += QChar(uchar(bytes[i]) | (ushort(uchar(bytes[i+1])) << 8));
        } else output = QString::fromUtf8(bytes);
        output.remove(QChar(0xfeff));
        auto distributions = output.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
        for (auto &name : distributions) name = name.trimmed();
        distributions.removeAll(QString());
        if (exitCode != 0 || distributions.isEmpty()) { runtimeUnavailable("wsl_distribution_missing", tr("WSL 未就绪或没有已安装的 Linux 发行版；独立功能无需 WSL。 / No usable WSL distribution is installed.")); return; }
        if (distributions.removeAll(distribution_)) distributions.prepend(distribution_);
        for (const auto &distribution : distributions) {
            if (distribution.startsWith('-') || distribution.contains(QChar::Null)) continue;
            for (const auto &script : runtimeLaunchers_) runtimeCandidates_.append(qMakePair(script, distribution));
        }
        detectNextRuntime(); return;
    }
    const auto probe = QJsonDocument::fromJson(bytes.trimmed()).object();
    if (probe.isEmpty()) {
        runtimeInfo_.insert("code", "probe_failed");
        runtimeInfo_.insert("message", tr("原生环境检测未返回有效结果，请查看诊断。 / Native runtime probe did not return a valid result."));
        detectNextRuntime(); return;
    }
    for (auto it = probe.begin(); it != probe.end(); ++it) runtimeInfo_.insert(it.key(), it.value().toVariant());
    if (exitCode != 0 || !probe.value("available").toBool()) { detectNextRuntime(); return; }
    const auto candidate = runtimeCandidates_[runtimeCandidate_];
    detecting_ = false;
    runtimeInfo_.insert("distribution", candidate.second); runtimeInfo_.insert("launcher", candidate.first);
    emit preferencesChanged();
    // Recovery YAML is authoritative after a disconnect. On initial startup,
    // a caller's explicit configuration is passed directly to the real host.
    launchHost(QUrl(candidate.first), candidate.second, reconnecting_ && !recoveryYaml_.isEmpty() ? QUrl() : initialConfig_, true);
}
void NativeRvizClient::scheduleReconnect()
{
    if (!automatic_ || !desiredRunning_ || reconnectTimer_.isActive()) return;
    if (reconnectAttempt_ >= 3) { runtimeUnavailable("reconnect_exhausted", tr("原生后端重连3次仍失败；可重试检测，独立功能不受影响。 / Native reconnect attempts exhausted.")); return; }
    ++reconnectAttempt_; reconnecting_ = true; runtimeState_ = "reconnecting";
    runtimeInfo_.insert("available", false);
    status_ = tr("原生连接中断，正在恢复（%1/3）；旧画面已清除。 / Native connection lost; reconnecting.").arg(reconnectAttempt_);
    reconnectTimer_.start(500 * (1 << (reconnectAttempt_ - 1))); emit changed();
}
void NativeRvizClient::becameConnected()
{
    connected_ = true; reconnecting_ = false; runtimeState_ = "connected";
    runtimeInfo_.insert("available", true); runtimeInfo_.insert("code", "connected");
    status_ = tr("已连接真实 ROS 1 Noetic / librviz；正在等待主播放头精确索引与同步。 / Native RViz connected; awaiting exact timeline synchronization.");
    send("plugins"); send("topics"); refresh(); cacheConfiguration(); emit changed();
}
void NativeRvizClient::cacheConfiguration()
{
    if (!connected_ || stopping_) return;
    if (cachePending_) { cacheDirty_ = true; return; }
    cacheDirty_ = false; cachePending_ = send("save", {}, {{"runtime_snapshot", true}});
}
void NativeRvizClient::setDistribution(const QString &value)
{
    const auto text=value.trimmed(); if (distribution_ == text || running()) return;
    distribution_=text; QSettings().setValue("nativeRviz/distribution", text); emit preferencesChanged();
}
void NativeRvizClient::setLauncher(const QString &value)
{
    if (launcher_ == value || running()) return;
    launcher_=value; QSettings().setValue("nativeRviz/launcher", value); emit preferencesChanged();
}
QString NativeRvizClient::nativePath(const QUrl &url) const
{
    if (!url.isLocalFile()) return {};
    auto path = QDir::fromNativeSeparators(url.toLocalFile());
#ifdef Q_OS_WIN
    const QRegularExpression drive("^([A-Za-z]):/(.*)$"); const auto match = drive.match(path);
    if (match.hasMatch()) return "/mnt/" + match.captured(1).toLower() + '/' + match.captured(2);
    const auto prefix = "//wsl.localhost/" + distribution_ + '/';
    if (path.startsWith(prefix, Qt::CaseInsensitive)) return '/' + path.mid(prefix.size());
    const auto legacy = "//wsl$/" + distribution_ + '/';
    if (path.startsWith(legacy, Qt::CaseInsensitive)) return '/' + path.mid(legacy.size());
    return {};
#else
    return path;
#endif
}
void NativeRvizClient::start(const QUrl &launcher, const QString &distribution, const QUrl &config)
{
    if (detecting_ || running()) return;
    automatic_ = false; desiredRunning_ = false; reconnectTimer_.stop(); reconnectAttempt_ = 0;
    recoveryYaml_.clear(); initialConfig_ = config; viewArchive_.clear();
    launchHost(launcher, distribution, config, false);
}
void NativeRvizClient::launchHost(const QUrl &launcher, const QString &distribution, const QUrl &config, bool automatic)
{
    if (running()) return;
    setDistribution(distribution); setLauncher(launcher.toString());
    const auto script=nativePath(launcher), initial=config.isEmpty()?QString():nativePath(config);
    if (script.isEmpty() || (!config.isEmpty() && initial.isEmpty()) || !QFileInfo(launcher.toLocalFile()).isFile()) {
        status_=tr("请选择可读取的本地启动脚本及该 ROS 环境可访问的配置。");
        if (automatic) runtimeUnavailable("configuration_path_unavailable", status_); else emit changed(); return;
    }
#ifdef Q_OS_WIN
    if (distribution_.isEmpty() || distribution_.contains(QChar::Null) || distribution_.startsWith('-')) {status_=tr("请输入已安装 ROS Noetic 的 WSL 发行版名称。");emit changed();return;}
#endif
    clearConnection(); input_.clear(); if (!automatic) diagnostic_.clear(); stopping_=false;
    sessionToken_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList arguments{script,"--hidden","--session-token",sessionToken_}; if(!initial.isEmpty())arguments<<"--config"<<initial;
    if (automatic) arguments << "--port" << "auto";
#ifdef Q_OS_WIN
    arguments=QStringList{"-d",distribution_,"--","bash"}+arguments;
    process_.start(automatic ? runtimeProgram_ : QStringLiteral("wsl.exe"),arguments);
#else
    process_.start(automatic ? runtimeProgram_ : QStringLiteral("bash"),arguments);
#endif
    runtimeInfo_.insert("clientProcessId", QVariant::fromValue<qint64>(process_.processId()));
    runtimeState_ = reconnecting_ ? "reconnecting" : "starting";
    status_=tr("正在启动可选原生 RViz… / Starting optional native RViz…"); deadline_.start();emit changed();
}
void NativeRvizClient::stop()
{
    desiredRunning_ = false; reconnectTimer_.stop();
    restartRequested_ = false;
    if (detecting_) { detecting_ = false; detectionDeadline_.stop(); detectionProcess_.kill(); runtimeState_ = "idle"; emit changed(); }
    if(!running() || stopping_)return;
    stopping_=true; deadline_.stop();
    if(process_.state()==QProcess::Running)send("shutdown");
    process_.closeWriteChannel();stopDeadline_.start();emit changed();
}
bool NativeRvizClient::busy() const
{
    if (replayBusy_ || sourceClosing_ || topicSetupsPending_ > 0 || !outputJob_.isEmpty()) return true;
    return std::any_of(requests_.cbegin(), requests_.cend(), [](const QJsonObject &request) {
        return isConfigurationRequest(request.value("command").toString());
    });
}
bool NativeRvizClient::send(const QString &command,QJsonObject arguments,QJsonObject context)
{
    if(process_.state()!=QProcess::Running || (stopping_ && command!="shutdown"))return false;
    if(!outputJob_.isEmpty()&&context.value("output_job").toString()!=outputJob_
       &&QStringList{"set","add","remove","remove_many","rename","duplicate","move","reset","view","view.reset","views.sync","tool","undo","redo","pointer","load","load_yaml","bag.open","bag.close","bag.index","bag.seek","bag.advance","bag.play_from_start"}.contains(command)) {
        status_=tr("原生准确画面正在输出，请等待完成后再修改场景或时间。");emit changed();return false;
    }
    arguments.insert("command",command);arguments.insert("request_id",++nextRequest_);
    const auto bytes=QJsonDocument(arguments).toJson(QJsonDocument::Compact)+'\n';
    if(bytes.size()>RequestLimit || requests_.size()>=64){status_=tr("原生请求超过大小或数量上限。");emit changed();emit commandFinished(command,false,status_);return false;}
    requests_.insert(nextRequest_,arguments);contexts_.insert(nextRequest_,context);sentAt_.insert(nextRequest_,elapsed_.elapsed());requestDeadline_.start();
    if(process_.write(bytes)!=bytes.size()){requests_.remove(nextRequest_);contexts_.remove(nextRequest_);sentAt_.remove(nextRequest_);status_=process_.errorString();emit changed();emit commandFinished(command,false,status_);return false;}
    if (isConfigurationRequest(command)) emit changed();
    emit commandSent(command);
    return true;
}
void NativeRvizClient::failProtocol(const QString &error)
{
    status_=error;diagnostic_+='\n'+error;emit changed();
    const bool recover = automatic_ && desiredRunning_;
    stop(); desiredRunning_ = recover;
}
void NativeRvizClient::expireRequests()
{
    const auto now=elapsed_.elapsed(); bool expired=false;
    for(auto it=sentAt_.begin();it!=sentAt_.end();++it) {
        const auto command=requests_.value(it.key()).value("command").toString();
        const qint64 timeout=command=="bag.seek"||command=="bag.advance"||command=="bag.index"?300000:(command=="bag.open"||command=="bag.close"||isConfigurationRequest(command)||command=="render_frame"?60000:RequestTimeout);
        if(now-it.value()>timeout){expired=true;break;}
    }
    if(expired&&!stopping_)failProtocol(tr("原生 RViz 请求超时；已停止连接以防使用过期状态。"));
    if(requests_.isEmpty())requestDeadline_.stop();
}
void NativeRvizClient::receive()
{
    input_+=process_.readAllStandardOutput();
    bool outputPending=!outputJob_.isEmpty();for(const auto &request:requests_)outputPending=outputPending||request.value("command").toString()=="render_frame";
    const qsizetype responseLimit=outputPending?192*1024*1024:ResponseLimit;
    if(input_.size()>responseLimit){input_.clear();failProtocol(tr("原生 RViz 响应超出当前传输内存上限。"));return;}
    for(;;){
        const auto end=input_.indexOf('\n');if(end<0)break;
        const auto line=input_.left(end);input_.remove(0,end+1);if(line.trimmed().isEmpty())continue;
        QJsonParseError parse;const auto doc=QJsonDocument::fromJson(line,&parse);
        if(parse.error!=QJsonParseError::NoError||!doc.isObject()){failProtocol(tr("原生 RViz 返回无效 JSON 协议；请查看诊断日志。"));return;}
        const auto value=doc.object();const auto event=value.value("event").toString();
        if(!event.isEmpty()){
            if(event=="ready"){
                if(connected_||value.value("protocol").toInt()!=1||value.value("backend").toString()!="ROS1/Noetic/librviz"){failProtocol(tr("不兼容的原生 RViz 协议。"));return;}
                deadline_.stop();
                if (!pendingInitialYaml_.isEmpty() || !pendingInitialFile_.isEmpty() || (reconnecting_ && !recoveryYaml_.isEmpty())) {
                    restoringConfiguration_ = true;
                    status_ = tr("正在恢复中断前的完整原生配置… / Restoring the last native configuration…");
                    if(!pendingInitialYaml_.isEmpty())send("load_yaml",{{"yaml",pendingInitialYaml_}},{{"runtime_restore",true}});
                    else if(!pendingInitialFile_.isEmpty())send("load",{{"file",nativePath(pendingInitialFile_)}},{{"runtime_restore",true}});
                    else send("load_yaml", {{"yaml", recoveryYaml_}}, {{"runtime_restore", true}, {"preserve_view_archive", true}});
                    emit changed();
                } else becameConnected();
            }else if(event=="changed")refresh();
            else if(event=="replay_progress") {
                status_=tr("原生历史恢复 %1 / %2；正在按明确实验时间处理真实消息。")
                    .arg(value.value("processed").toString(),value.value("total").toString());emit changed();
            }
            else if(event=="bag_playback_finished"){status_=value.value("exit_code").toInt()==0?tr("原生录包播放结束；使用记录时间基准。"):tr("原生录包播放失败，请查看诊断日志。");emit changed();refresh();}
            else if(event=="fatal"){failProtocol(value.value("error").toString());return;}
            continue;
        }
        const auto requestId=value.value("request_id");
        if(!requestId.isDouble()||!requests_.contains(requestId.toInteger())){failProtocol(tr("原生 RViz 返回未知请求编号。"));return;}
        const auto key=requestId.toInteger();const auto request=requests_.take(key),context=contexts_.take(key);sentAt_.remove(key);
        const auto command=request.value("command").toString();if(command=="tree")treePending_=false;if(command=="frame")framePending_=false;
        if(context.value("topic_setup").toBool()&&(command=="set"||!value.value("ok").toBool())) {
            topicSetupsPending_=std::max(0,topicSetupsPending_-1);emit changed();
        }
        if(!value.value("ok").isBool()){failProtocol(tr("原生 RViz 响应缺少成功状态。"));return;}
        const auto responseResult=value.value("result").toObject();
        if(command=="frame"&&context.value("property_preview").toBool()&&responseResult.value("throttled").toBool()) {
            propertyPreviews_.insert(request.value("view_id").toString("main"));
            propertyPreviewTimer_.start(qBound(1,responseResult.value("retry_after_ms").toInt(250),1000));
        }
        if(value.value("ok").toBool()&&!responseResult.value("configuration_yaml").toString().isEmpty()) {
            recoveryYaml_=responseResult.value("configuration_yaml").toString();
            runtimeInfo_.insert("configurationSnapshotSerial",runtimeInfo_.value("configurationSnapshotSerial").toULongLong()+1);
        }
        if(responseResult.contains("active_tool")){activeToolClass_=responseResult.value("active_tool").toString();emit changed();}
        if (context.value("runtime_snapshot").toBool()) {
            cachePending_ = false;
            const auto yaml = responseResult.value("yaml").toString();
            if (value.value("ok").toBool() && !yaml.isEmpty()) {
                recoveryYaml_ = yaml;
                runtimeInfo_.insert("configurationSnapshotSerial", runtimeInfo_.value("configurationSnapshotSerial").toULongLong() + 1);
                emit changed();
                if (cacheDirty_) cacheConfiguration();
                else if (restartRequested_) {
                    restartRequested_ = false; reconnecting_ = true; stopping_ = true;
                    send("shutdown"); process_.closeWriteChannel(); stopDeadline_.start(); emit changed();
                }
            } else {
                restartRequested_ = false;
                diagnostic_ = (diagnostic_ + tr("\n未取得最新原生配置快照；中断后只能恢复最后成功快照。 / Native configuration snapshot failed.\n")).right(16000);
                emit changed();
            }
            continue;
        }
        if (context.value("runtime_restore").toBool() && !value.value("ok").toBool()) {
            runtimeUnavailable("configuration_restore_failed", tr("原生编辑配置恢复失败，未用默认场景替代。 / Native configuration restore failed; default scene was not substituted."));
            stop(); continue;
        }
        if(responseResult.contains("history")){const auto history=responseResult.value("history").toObject();canUndo_=history.value("can_undo").toBool();canRedo_=history.value("can_redo").toBool();emit changed();}
        if(context.contains("snapshot_job")) {
            const auto result=value.value("result").toObject();auto metadata=result.toVariantMap();metadata.remove("yaml");
            emit configSnapshotReady(context.value("snapshot_job").toString(),value.value("ok").toBool(),result.value("yaml").toString(),metadata,value.value("error").toString());
            emit commandFinished(command,value.value("ok").toBool(),value.value("error").toString());continue;
        }
        if(context.contains("output_job")) {
            if(command=="bag.seek"||command=="bag.advance")replayBusy_=false;
            if(context.value("output_job").toString()!=outputJob_){emit changed();continue;}
            if(!value.value("ok").toBool()) {
                const auto error=value.value("error").toString();finishOutput(false,{},{},error);emit commandFinished(command,false,error);continue;
            }
            const auto result=value.value("result").toObject();
            if(command=="bag.seek"||command=="bag.advance") {
                replayResult_=result.toVariantMap();targetNs_=result.value("target_ns").toString();clock_=result.value("clock_ns").toString();
                sceneReady_=result.value("native_scene_ready").toBool();
                dataSynchronized_=targetNs_==outputTarget_&&clock_==outputTarget_&&result.value("native_data_ready").toBool(sceneReady_)
                    &&result.value("native_queues_drained").toBool()&&result.value("tf_coverage_complete").toBool();
                synchronized_=dataSynchronized_&&sceneReady_;
                emit previewChanged();emit changed();
                if(!dataSynchronized_)finishOutput(false,{},result.toVariantMap(),tr("准确目标处的必需 TF 或启用 Display 尚未就绪，未输出图片。"));
                else if(!send("render_frame",{{"view_id",outputView_},{"target_ns",outputTarget_},{"width",outputWidth_},{"height",outputHeight_}},{{"output_job",outputJob_}}))
                    finishOutput(false,{},{},tr("无法发送原生离屏渲染请求。"));
            } else if(command=="render_frame") {
                auto metadata=result.toVariantMap();metadata.remove("data_base64");const auto encoded=result.value("data_base64").toString();
                const bool valid=result.value("native_render_texture").toBool()&&result.value("width").toInt()==outputWidth_&&result.value("height").toInt()==outputHeight_
                    &&result.value("view_id").toString("main")==outputView_
                    &&result.value("target_ns").toString()==outputTarget_&&result.value("clock_ns").toString()==outputTarget_
                    &&result.value("native_scene_ready").toBool()&&result.value("required_tf_complete").toBool()&&result.value("experiment_playhead_synchronized").toBool()
                    &&encoded.size()<=180*1024*1024;
                QImage frame;
                if(valid) {
                    auto decoded=QByteArray::fromBase64Encoding(encoded.toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
                    if(decoded){QBuffer buffer(&decoded.decoded);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"PNG");
                        const int previousLimit=QImageReader::allocationLimit();QImageReader::setAllocationLimit(300);
                        if(reader.size()==QSize(outputWidth_,outputHeight_))frame=reader.read();QImageReader::setAllocationLimit(previousLimit);}
                }
                if(frame.isNull())finishOutput(false,{},{},tr("原生输出的时间、就绪状态、尺寸或 PNG 内容未通过校验。"));
                else completeOutputFrame(frame,metadata);
            }
            emit commandFinished(command,true,{});continue;
        }
        if(!value.value("ok").toBool()){
            if(command=="bag.close"){sourceClosing_=false;emit changed();}
            if(command=="bag.index"||command=="bag.seek"||command=="bag.advance") {
                replayBusy_=false;dataSynchronized_=synchronized_=sceneReady_=false;queuedTarget_.clear();emit previewChanged();
                if(command=="bag.index")indexed_=false;
            }
            status_=value.value("error").toString();
            if(context.value("rollback_display").isString()){send("remove",{{"property_id",context["rollback_display"]}});status_=tr("按话题添加失败；正在移除未配置的 Display：%1").arg(status_);}
            emit changed();emit commandFinished(command,false,status_);refresh();continue;
        }
        const auto result=value.value("result").toObject();
        if(command=="tree"||command=="load"||command=="load_yaml"||command=="undo"||command=="redo"){
            if(!validateTree(result)){failProtocol(tr("原生属性树结构、深度或属性身份无效。"));return;}
            tree_=result;sourceAvailable_=result.value("source_available").toBool(true);
            if((command=="load"||command=="load_yaml")&&!context.value("preserve_view_archive").toBool())viewArchive_.clear();
            if(command!="tree"){options_.clear();indexed_=dataSynchronized_=synchronized_=sceneReady_=false;targetNs_.clear();preview_.clear();previewTarget_.clear();viewPreviews_.clear();emit previewChanged();emit changed();}rebuild();
        }else if(command=="bag.close") {
            const auto closedTree=result.value("tree").toObject();if(!validateTree(closedTree)){failProtocol(tr("移除数据源后的原生属性树无效。"));return;}
            tree_=closedTree;sourceClosing_=replayBusy_=indexed_=dataSynchronized_=synchronized_=sceneReady_=sourceAvailable_=false;
            topicSetupsPending_=0;queuedTarget_.clear();targetNs_.clear();clock_.clear();preview_.clear();previewTarget_.clear();viewPreviews_.clear();replayResult_.clear();topics_.clear();options_.clear();
            nativeResources_=result.value("resources").toObject().toVariantMap();rebuild();
            status_=tr("已移除录包源并释放原生消息、TF、播放和渲染资源；Display 配置保留为无源状态。原始文件未修改。");emit previewChanged();emit changed();
        }else if(command=="resources") {
            nativeResources_=result.toVariantMap();emit changed();
        }else if(command=="bag.index") {
            replayBusy_=false;indexed_=true;replayResult_=result.toVariantMap();
            status_=tr("原生实验时间索引已就绪；%1 基准，未定时消息策略 %2。")
                .arg(result.value("time_basis").toString(),result.value("untimed_policy").toString());emit changed();
        }else if(command=="bag.seek"||command=="bag.advance") {
            replayBusy_=false;replayResult_=result.toVariantMap();
            targetNs_=result.value("target_ns").toString();clock_=result.value("clock_ns").toString();
            sceneReady_=result.value("native_scene_ready").toBool();
            dataSynchronized_=!targetNs_.isEmpty()&&clock_==targetNs_&&result.value("native_queues_drained").toBool()
                &&result.value("tf_coverage_complete").toBool()&&result.value("native_data_ready").toBool(sceneReady_);
            synchronized_=dataSynchronized_&&sceneReady_;
            status_=synchronized_
                ? tr("原生历史恢复完成：%1 ns，耗时 %2 ms；尚未认证实时播放或第三方插件异步工作。")
                    .arg(targetNs_,QString::number(result.value("elapsed_ms").toInteger()))
                : tr("已恢复目标时钟与所选消息历史，但必需 TF 或启用的原生 Display 尚未就绪；请查看真实状态及 native_readiness 详情。");
            emit previewChanged();emit changed();refresh();
            requestPreview();
            if(!queuedTarget_.isEmpty()&&queuedTarget_!=targetNs_) {const auto next=queuedTarget_;const bool advance=queuedAdvance_;queuedTarget_.clear();requestTime(next,advance);}
            else {queuedTarget_.clear();requestPreview();}
        }else if(command=="plugins"){plugins_=result.value("displays").toArray().toVariantList();tools_=result.value("tools").toArray().toVariantList();viewControllers_=result.value("views").toArray().toVariantList();emit changed();}
        else if(command=="topics"){topics_=result.value("topics").toArray().toVariantList();emit changed();}
        else if(command=="options"){const auto id=result.value("property_id").toString();options_[id]=result.value("options").toArray().toVariantList();rebuild();}
        else if(command=="save"){
            const auto path=context.value("client_file").toString();if(!path.isEmpty()){QSaveFile file(path);const auto yaml=result.value("yaml").toString().toUtf8();status_=file.open(QIODevice::WriteOnly)&&file.write(yaml)==yaml.size()&&file.commit()?tr("原生配置已保存；未知属性及原始导入配置保存在 .rviz 内。"):tr("保存失败：%1").arg(file.errorString());emit changed();}
        }else if(command=="frame"&&!result.value("throttled").toBool()){
            const auto encoded=result.value("data_base64").toString();
            if(encoded.size()>8*1024*1024){failProtocol(tr("原生预览编码超过上限。"));return;}
            const auto decoded=QByteArray::fromBase64Encoding(encoded.toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
            if(!decoded){failProtocol(tr("原生预览 Base64 无效。"));return;}
            QByteArray data=decoded.decoded;QBuffer buffer(&data);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"PNG");const auto size=reader.size();
            if(!size.isValid()||size.width()>1280||size.height()>1280||size!=QSize(result.value("width").toInt(),result.value("height").toInt())||reader.read().isNull()){failProtocol(tr("原生预览尺寸或 PNG 内容无效。"));return;}
            const auto viewId=request.value("view_id").toString("main");
            if(result.value("view_id").toString("main")!=viewId){failProtocol(tr("原生预览返回了其他视角，已拒绝过期或错误画面。"));return;}
            auto preview=result.toVariantMap();preview.remove("data_base64");preview.insert("source","data:image/png;base64,"+encoded);
            preview.insert("synchronized",result.value("experiment_playhead_synchronized").toBool()&&result.value("native_scene_ready").toBool()
                &&!result.value("synchronized_target_ns").toString().isEmpty()&&result.value("clock_ns")==result.value("synchronized_target_ns"));
            viewPreviews_.insert(viewId,preview);
            if(viewId!="main"){emit previewChanged();emit commandFinished(command,true,{});continue;}
            preview_="data:image/png;base64,"+encoded;previewTarget_=result.value("clock_ns").toString();
            if(replayBusy_){emit previewChanged();emit commandFinished(command,true,{});continue;}
            clock_=result.value("clock_ns").toString();
            targetNs_=result.value("synchronized_target_ns").toString();sceneReady_=result.value("native_scene_ready").toBool();
            dataSynchronized_=result.value("experiment_playhead_synchronized").toBool()&&!targetNs_.isEmpty()&&clock_==targetNs_;
            synchronized_=dataSynchronized_&&sceneReady_;emit previewChanged();
        }else if(command=="set"){
            const auto viewId=context.value("property_view_id").toString();
            if(!viewId.isEmpty()&&result.value("data_history_preserved").toBool()&&result.value("view_id").toString()==viewId) {
                queuePropertyPreview(viewId);
            }else{dataSynchronized_=synchronized_=sceneReady_=false;viewPreviews_.clear();emit previewChanged();}
            refresh();
        }else if(command=="bag.open"){sourceAvailable_=true;topics_=result.value("topics").toArray().toVariantList();status_=tr("原生后端已读取真实录包；从头播放使用记录时间。");emit changed();}
        else if(command=="view"){
            if(!validateTree(result)){failProtocol(tr("原生相机控制器属性树无效。"));return;}
            tree_=result;rebuild();queuePropertyPreview("main");
        }
        else if(command=="pointer"){requestViewPreview(request.value("view_id").toString("main"));}
        else if(command=="view.reset"){refresh();requestViewPreview(request.value("view_id").toString("main"));}
        else if(command=="views.sync"){
            viewArchive_.remember(result.value("removed_views").toArray());
            emit modelChanged();refresh();
        }
        else if(command=="add"&&context.contains("topic")){
            if(sourceClosing_){topicSetupsPending_=std::max(0,topicSetupsPending_-1);emit changed();continue;}
            const auto display=result.value("display").toObject();int count=0;const auto topic=findTopic(display,context.value("topic_type").toString(),count);const auto displayId=display.value("id").toString();
            if(count!=1||displayId.isEmpty()){topicSetupsPending_=std::max(0,topicSetupsPending_-1);if(!displayId.isEmpty())send("remove",{{"property_id",displayId}});status_=tr("插件未提供唯一兼容的原生 Topic 属性，已撤销本次添加。");emit changed();emit commandFinished("add",false,status_);continue;}
            if(!send("set",{{"property_id",topic.value("id")},{"value",context.value("topic")},{"history_group",context.value("history_group")}},{{"rollback_display",displayId},{"topic_setup",true},{"topic_unverified",context.value("topic_unverified")}})) {
                topicSetupsPending_=std::max(0,topicSetupsPending_-1);emit changed();
            }
            refresh();
        }else if(command!="hello"&&command!="frame"&&command!="shutdown")refresh();
        if(command=="set"&&context.value("topic_unverified").toBool()){
            status_=tr("已设置真实原生 Topic 属性；等待数据源，消息类型兼容性尚未验证。 / Native Topic configured; waiting for source data. Message type compatibility is unverified.");
            emit changed();
        }
        if (context.value("runtime_restore").toBool()) { restoringConfiguration_ = false; pendingInitialYaml_.clear(); pendingInitialFile_={}; becameConnected(); }
        const bool configurationChanged = QStringList{"set", "add", "remove", "remove_many", "move", "rename", "duplicate", "reset", "undo", "redo", "view", "view.reset", "views.sync", "tool", "load", "load_yaml", "bag.close"}.contains(command)
            || (command == "pointer" && QStringList{"release", "wheel"}.contains(request.value("type").toString()));
        if (configurationChanged) cacheConfiguration();
        emit commandFinished(command,true,{});
        if(command=="bag.index"&&!queuedTarget_.isEmpty()){const auto next=queuedTarget_;const bool advance=queuedAdvance_;queuedTarget_.clear();requestTime(next,advance);}
        if(!treePending_&&refreshQueued_&&!stopping_){refreshQueued_=false;refresh();}
    }
}
void NativeRvizClient::refresh()
{
    if(!connected_||stopping_)return;
    if(treePending_){refreshQueued_=true;return;}treePending_=send("tree");
}
void NativeRvizClient::rebuild()
{
    rows_.clear();nodes_.clear();
    // Read actual enabled native Display subscriptions, independent of tree search
    // or expansion. This also covers plugins unsupported by the Qt6 bag renderer.
    QSet<QString> topicSet;
    std::function<void(const QJsonObject &,bool,bool)> collectTopics=[&](const QJsonObject &node,bool active,bool root){
        if(!root&&node.contains("display_enabled"))active=active&&node.value("display_enabled").toBool();
        if(!active)return;
        if(node.value("native_type").toString().endsWith("RosTopicProperty")) {
            const auto topic=node.value("topic_name").toString(node.value("value").toString()).trimmed();
            if(!topic.isEmpty())topicSet.insert(topic);
        }
        for(const auto &child:node.value("children").toArray())collectTopics(child.toObject(),active,false);
    };
    collectTopics(tree_.value("displays").toObject(),true,true);
    displayTopics_=topicSet.values();displayTopics_.sort();
    std::function<bool(const QJsonObject &)> matches=[&](const QJsonObject &n){if(n.value("name").toString().contains(search_,Qt::CaseInsensitive)||n.value("plugin_class").toString().contains(search_,Qt::CaseInsensitive))return true;for(const auto &c:n.value("children").toArray())if(matches(c.toObject()))return true;return false;};
    std::function<void(const QJsonObject &,int,bool,QString,QString)> append=[&](const QJsonObject &node,int depth,bool visible,QString displayId,QString viewId){
        auto item=node.toVariantMap();const auto id=node.value("id").toString();
        if(!node.value("plugin_class").toString().isEmpty())displayId=id;
        const bool show=visible&&!node.value("hidden").toBool()&&(search_.isEmpty()||matches(node));
        const bool expanded=depth==0||expanded_.contains(id)||!search_.isEmpty();
        item.insert("depth",depth);item.insert("expanded",expanded);item.insert("display_id",displayId);item.insert("view_id",viewId);if(options_.contains(id))item.insert("options",options_[id]);nodes_.insert(id,item);if(show)rows_.append(item);
        for(const auto &child:node.value("children").toArray())append(child.toObject(),depth+1,show&&expanded,displayId,viewId);
    };
    for(const auto &root:{"displays","views","tools"})if(tree_.contains(root))append(tree_.value(root).toObject(),0,true,{},root==QStringLiteral("views")?QStringLiteral("main"):QString());
    QSet<QString> activeViews{"main"};
    for(const auto &entry:tree_.value("render_views").toArray()) {const auto view=entry.toObject();const auto viewId=view.value("view_id").toString();activeViews.insert(viewId);append(view.value("controller").toObject(),0,true,{},viewId);}
    for(auto it=viewPreviews_.begin();it!=viewPreviews_.end();)if(!activeViews.contains(it.key()))it=viewPreviews_.erase(it);else ++it;
    if(!nodes_.contains(selected_))selected_.clear();
    for(auto it=options_.begin();it!=options_.end();)if(!nodes_.contains(it.key()))it=options_.erase(it);else++it;
    emit modelChanged();emit previewChanged();
}
QVariantList NativeRvizClient::renderViews() const {return tree_.value("render_views").toArray().toVariantList();}
QVariantMap NativeRvizClient::selected() const{return nodes_.value(selected_);}
void NativeRvizClient::select(const QString &id){if(!nodes_.contains(id))return;selected_=id;emit modelChanged();if(!busy()&&nodes_[id].value("options_available").toBool())send("options",{{"property_id",id}});}
void NativeRvizClient::setSearch(const QString &text){if(search_!=text){search_=text;rebuild();}}
void NativeRvizClient::toggleExpanded(const QString &id){if(expanded_.contains(id))expanded_.remove(id);else expanded_.insert(id);rebuild();}
void NativeRvizClient::setPropertyValue(const QString &id,const QVariant &value)
{
    if(connected_&&!busy()&&nodes_.contains(id))
        send("set",{{"property_id",id},{"value",QJsonValue::fromVariant(value)}},{{"property_view_id",nodes_.value(id).value("view_id").toString()}});
}
void NativeRvizClient::queuePropertyPreview(const QString &viewId)
{
    viewPreviews_.remove(viewId);
    if(viewId=="main"){preview_.clear();previewTarget_.clear();synchronized_=sceneReady_=false;}
    propertyPreviews_.insert(viewId);propertyPreviewTimer_.start(0);emit previewChanged();
}
void NativeRvizClient::loadConfig(const QUrl &file)
{
    if(!file.isLocalFile()||!QFileInfo(file.toLocalFile()).isFile())return;
    if(!connected_){pendingInitialFile_=file;pendingInitialYaml_.clear();recoveryYaml_.clear();return;}
    const auto path=nativePath(file);if(!path.isEmpty()&&connected_&&!busy())send("load",{{"file",path}});
}
void NativeRvizClient::loadConfigYaml(const QString &yaml)
{
    if(yaml.isEmpty()){if(!connected_)clearPendingConfig();return;}
    if(yaml.toUtf8().size()>RequestLimit-1024)return;
    if(!connected_){pendingInitialYaml_=yaml;pendingInitialFile_={};recoveryYaml_.clear();return;}
    if(connected_&&!busy())send("load_yaml",{{"yaml",yaml}});
}
void NativeRvizClient::clearPendingConfig()
{
    pendingInitialYaml_.clear();pendingInitialFile_={};initialConfig_={};
    if(!connected_)recoveryYaml_.clear();
    viewArchive_.clear();emit modelChanged();
}
void NativeRvizClient::saveConfig(const QUrl &file){if(file.isLocalFile()&&connected_&&!replayBusy_)send("save",{},{{"client_file",file.toLocalFile()}});}
QString NativeRvizClient::snapshotConfig()
{
    const auto job=QUuid::createUuid().toString(QUuid::WithoutBraces);const bool available=connected_&&!busy()&&!stopping_;
    QTimer::singleShot(0,this,[this,job,available]{
        if(!available||!connected_||busy()||stopping_||!send("save",{},{{"snapshot_job",job}}))
            emit configSnapshotReady(job,false,{},{},tr("请等待当前原生操作完成后再保存项目快照。"));
    });return job;
}
QVariantList NativeRvizClient::compatiblePlugins(const QString &type) const
{
    QVariantList result;for(const auto &entry:plugins_){const auto item=entry.toMap();if(type.isEmpty()||item.value("message_types").toStringList().contains(type))result.append(item);}return result;
}
void NativeRvizClient::addDisplay(const QString &plugin,const QString &name,const QString &topic,const QString &parentId)
{
    if(!connected_||busy())return;
    const auto requestedTopic=topic.trimmed();
    QJsonObject context;
    if(!requestedTopic.isEmpty()){
        QSet<QString> types;for(const auto &t:topics_){const auto row=t.toMap();if(row.value("name").toString()==requestedTopic)types.insert(row.value("type").toString());}
        if(types.size()>1){status_=tr("所选话题存在多个消息类型，不能自动选择原生 Topic 属性。 / The topic has ambiguous message types.");emit changed();return;}
        QString type;
        if(!types.isEmpty()){
            type=*types.begin();bool compatible=false;for(const auto &entry:compatiblePlugins(type))if(entry.toMap().value("class").toString()==plugin)compatible=true;
            if(!compatible){status_=tr("该插件没有声明支持选中话题的消息类型。");emit changed();return;}
        }
        // A manually configured topic need not exist yet. The real plugin must
        // still expose exactly one matching RosTopicProperty; the add response
        // verifies that property before setting it. No compatibility or data
        // readiness is inferred for a topic absent from the current catalog.
        context={{"topic",requestedTopic},{"topic_type",type},{"topic_unverified",types.isEmpty()},{"topic_setup",true},{"history_group",QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    }
    if(!requestedTopic.isEmpty()){++topicSetupsPending_;dataSynchronized_=synchronized_=sceneReady_=false;emit previewChanged();emit changed();}
    QJsonObject arguments{{"class",plugin},{"name",name},{"enabled",true}};if(!parentId.isEmpty())arguments.insert("parent_id",parentId);
    if(context.contains("history_group"))arguments.insert("history_group",context.value("history_group"));
    if(!send("add",arguments,context)&&!requestedTopic.isEmpty()) {
        topicSetupsPending_=std::max(0,topicSetupsPending_-1);emit changed();
    }
}
void NativeRvizClient::moveDisplay(const QString &id,const QString &parentId,int index)
{
    if(!connected_||busy()||!nodes_.contains(id)||!nodes_.contains(parentId))return;
    dataSynchronized_=synchronized_=sceneReady_=false;emit previewChanged();send("move",{{"property_id",id},{"parent_id",parentId},{"index",index}});
}
void NativeRvizClient::cameraMouse(const QString &type,double x,double y,int button,int buttons,int modifiers,int wheelDelta)
{
    cameraMouseForView("main",type,x,y,button,buttons,modifiers,wheelDelta);
}
void NativeRvizClient::cameraMouseForView(const QString &viewId,const QString &type,double x,double y,int button,int buttons,int modifiers,int wheelDelta)
{
    if(!connected_||!outputJob_.isEmpty()||!std::isfinite(x)||!std::isfinite(y))return;
    send("pointer",{{"view_id",viewId},{"type",type},{"x",x},{"y",y},{"button",button},{"buttons",buttons},{"modifiers",modifiers},{"wheel_delta",wheelDelta}});
}
void NativeRvizClient::syncViews(const QVariantList &views)
{
    if(!connected_||busy())return;
    QSet<QString> active;
    for(const auto &view:tree_.value("render_views").toArray())active.insert(view.toObject().value("view_id").toString());
    QJsonArray descriptors;
    for(const auto &entry:views) {
        auto view=QJsonObject::fromVariantMap(entry.toMap());const auto id=view.value("view_id").toString();
        const auto archived=viewArchive_.value(id);
        if(!active.contains(id)&&archived.value("class").toString()==view.value("class").toString()) {
            const auto yaml=archived.value("controller_yaml").toString();
            if(yaml.isEmpty()) {
                status_=tr("此机位的完整撤销快照已超过内存预算，未用默认相机替代。 / The complete camera Undo snapshot exceeded its memory budget; no default camera was substituted.");
                emit changed();emit commandFinished("views.sync",false,status_);return;
            }
            view.insert("controller_yaml",yaml);
        }
        descriptors.append(view);
    }
    send("views.sync",{{"views",descriptors}});
}
void NativeRvizClient::activateTool(const QString &plugin)
{
    if (!connected_ || busy()) return;
    for (const auto &value : tools_) if (value.toMap().value("class").toString() == plugin) { send("tool", {{"class", plugin}}); return; }
    status_=tr("此原生工具未安装。 / Native tool is not installed."); emit changed();
}
void NativeRvizClient::setViewController(const QString &plugin)
{
    if (!connected_ || busy()) return;
    for (const auto &value : viewControllers_) if (value.toMap().value("class").toString() == plugin) { send("view", {{"class", plugin}}); return; }
    status_=tr("此原生视角控制器未安装。 / Native view controller is not installed."); emit changed();
}
void NativeRvizClient::resetCamera()
{
    resetCameraForView("main");
}
void NativeRvizClient::resetCameraForView(const QString &viewId)
{
    if (!connected_ || busy()) return;
    // Reset only the active native camera. Display instances and the exact Bag
    // clock remain intact; a Display reset would discard received scene data.
    send("view.reset",{{"view_id",viewId}});
}
void NativeRvizClient::displayAction(const QString &command,const QString &id,const QString &name){if(!busy()&&QStringList{"remove","duplicate","rename","reset"}.contains(command)){dataSynchronized_=synchronized_=sceneReady_=false;emit previewChanged();send(command,{{"property_id",id},{"name",name}});}}
void NativeRvizClient::removeDisplays(const QStringList &ids)
{
    if(!connected_||busy()||ids.isEmpty())return;
    QJsonArray selected;for(const auto &id:ids){if(!nodes_.contains(id)||nodes_.value(id).value("plugin_class").toString().isEmpty())return;selected.append(id);}
    dataSynchronized_=synchronized_=sceneReady_=false;preview_.clear();previewTarget_.clear();emit previewChanged();send("remove_many",{{"property_ids",selected}});
}
void NativeRvizClient::undoNative(){if(connected_&&!busy()&&canUndo_)send("undo");}
void NativeRvizClient::redoNative(){if(connected_&&!busy()&&canRedo_)send("redo");}
void NativeRvizClient::refreshResources(){if(connected_&&!busy())send("resources");}
void NativeRvizClient::openBag(const QUrl &file){const auto path=nativePath(file);if(!path.isEmpty()&&connected_&&!busy()){indexed_=dataSynchronized_=synchronized_=sceneReady_=false;targetNs_.clear();emit previewChanged();emit changed();send("bag.open",{{"file",path}});}}
void NativeRvizClient::closeBag()
{
    if(!connected_||sourceClosing_)return;
    if(!outputJob_.isEmpty())cancelOutputFrame(outputJob_);
    sourceClosing_=true;queuedTarget_.clear();indexed_=dataSynchronized_=synchronized_=sceneReady_=false;targetNs_.clear();clock_.clear();preview_.clear();previewTarget_.clear();
    emit previewChanged();emit changed();if(!send("bag.close")){sourceClosing_=false;emit changed();}
}
void NativeRvizClient::playBag(double rate){if(connected_&&!busy()&&std::isfinite(rate)){dataSynchronized_=synchronized_=sceneReady_=false;emit previewChanged();send("bag.play_from_start",{{"rate",rate}});}}
void NativeRvizClient::stopBag(){if(connected_&&!replayBusy_)send("bag.stop");}
void NativeRvizClient::requestPreview(){requestViewPreview("main");}
void NativeRvizClient::requestViewPreview(const QString &viewId){if(connected_&&!framePending_&&!stopping_&&!busy())framePending_=send("frame",{{"view_id",viewId},{"max_width",960}});}
void NativeRvizClient::indexBag(const QUrl &file,const QString &basis,const QString &policy,const QVariantMap &options)
{
    const auto path=nativePath(file);
    if(!connected_||busy()||path.isEmpty())return;
    if(!QStringList{"header","record"}.contains(basis)||!QStringList{"reject","skip","map_record"}.contains(policy)) {
        status_=tr("请选择明确的 Header / record 时间及未定时消息策略。");emit changed();return;
    }
    QJsonObject arguments{{"file",path},{"time_basis",basis},{"untimed_policy",policy}};
    for(const auto &key:{"topics","anchor_record_ns","anchor_header_ns"})if(options.contains(key))arguments.insert(key,QJsonValue::fromVariant(options.value(key)));
    indexed_=dataSynchronized_=synchronized_=sceneReady_=false;targetNs_.clear();replayResult_.clear();preview_.clear();
    replayBusy_=send("bag.index",arguments);emit previewChanged();emit changed();
}
void NativeRvizClient::seekAbsolute(const QString &nanoseconds)
{requestTime(nanoseconds,false);}
void NativeRvizClient::advanceAbsolute(const QString &nanoseconds)
{requestTime(nanoseconds,true);}
void NativeRvizClient::requestTime(const QString &nanoseconds,bool incremental)
{
    if(!connected_)return;
    if(!outputJob_.isEmpty())return;
    if(topicSetupsPending_>0){status_=tr("请等待原生 Display 的 Topic 配置完成。");emit changed();return;}
    bool ok=false;const auto target=nanoseconds.toULongLong(&ok);
    if(!ok||nanoseconds.startsWith('-')||target==0){status_=tr("实验目标时间必须是精确的正整数纳秒字符串。");emit changed();return;}
    const auto exact=QString::number(target);
    if(dataSynchronized_&&targetNs_==exact&&clock_==exact&&!replayBusy_){requestPreview();return;}
    if(replayBusy_){queuedTarget_=exact;queuedAdvance_=incremental;return;}
    if(!indexed_){status_=tr("请先建立明确时间基准的原生录包索引。");emit changed();return;}
    const bool forward=incremental&&synchronized_&&sceneReady_&&target>targetNs_.toULongLong();
    dataSynchronized_=synchronized_=sceneReady_=false;viewPreviews_.clear();if(!forward){preview_.clear();previewTarget_.clear();}targetNs_.clear();
    replayBusy_=send(forward?"bag.advance":"bag.seek",{{"target_ns",exact}});emit previewChanged();emit changed();
}
void NativeRvizClient::cancelReplay()
{
    if(!outputJob_.isEmpty())return;
    queuedTarget_.clear();dataSynchronized_=synchronized_=sceneReady_=false;emit previewChanged();
    if(connected_&&replayBusy_)send("bag.cancel");
}
void NativeRvizClient::cancelPendingSeek()
{
    if(!outputJob_.isEmpty())return;
    queuedTarget_.clear();
    // Keep useful indexing running when playback begins; only old paused seeks
    // are obsolete. Cancellation replies clear busy through the original request.
    for(const auto &request:requests_)
        if(request.value("command").toString()=="bag.seek"||request.value("command").toString()=="bag.advance"){cancelReplay();return;}
}
QString NativeRvizClient::prepareOutputFrame(const QString &targetNs,int width,int height)
{
    return prepareViewOutputFrame("main",targetNs,width,height);
}
QString NativeRvizClient::prepareViewOutputFrame(const QString &viewId,const QString &targetNs,int width,int height)
{
    return prepareFrames(targetNs,{QVariantMap{{"view_id",viewId},{"width",width},{"height",height}}},false);
}
QString NativeRvizClient::prepareOutputFrames(const QString &targetNs,const QVariantList &views)
{
    return prepareFrames(targetNs,views,true);
}
QString NativeRvizClient::prepareFrames(const QString &targetNs,const QVariantList &views,bool batch)
{
    const auto job=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!outputJob_.isEmpty()) {
        QTimer::singleShot(0,this,[this,job,targetNs,batch]{
            if(batch)emit outputFramesPrepared(job,false,targetNs,{},tr("另一个原生输出任务正在进行。"));
            else emit outputFramePrepared(job,false,targetNs,{},{},tr("另一个原生输出任务正在进行。"));
        });return job;
    }
    const bool available=connected_&&indexed_&&!busy()&&!stopping_;
    outputJob_=job;outputTarget_=targetNs;outputViews_=views;outputBatch_=batch;
    outputViewIndex_=0;outputMetadata_.clear();outputSceneRevision_.clear();emit changed();
    QTimer::singleShot(0,this,[this,job,available]{
        if(outputJob_!=job)return;
        bool ok=false;const auto stamp=outputTarget_.toULongLong(&ok);
        if(!available||!connected_||!indexed_||stopping_) {finishOutput(false,{},{},tr("请先连接原生 RViz、完成实验索引并等待当前操作结束。"));return;}
        if(!ok||!stamp||outputTarget_!=QString::number(stamp)||outputViews_.isEmpty()||outputViews_.size()>16) {
            finishOutput(false,{},{},tr("准确纳秒目标或原生输出尺寸无效（最多 64 MP、单边 16384）。"));return;
        }
        QSet<QString> identities; qint64 totalPixels=0;
        for(const auto &entry:outputViews_) {
            const auto view=entry.toMap();const auto id=view.value("view_id").toString();
            const double w=view.value("width").toDouble(),h=view.value("height").toDouble();
            if(id.isEmpty()||id.toUtf8().size()>128||identities.contains(id)||!std::isfinite(w)||!std::isfinite(h)
                ||w!=std::floor(w)||h!=std::floor(h)||w<=0||h<=0||w>16384||h>16384||w*h>67108864.) {
                finishOutput(false,{},{},tr("原生批量输出的视角标识或尺寸无效。 / Invalid native view identities or dimensions."));return;
            }
            identities.insert(id);totalPixels+=qint64(w)*qint64(h);
        }
        if(totalPixels>96LL*1024*1024) {
            finishOutput(false,{},{},tr("原生多视角合成超过 96 MP 内存预算。 / Native composition exceeds its 96 MP memory budget."));return;
        }
        const auto first=outputViews_.first().toMap();outputView_=first.value("view_id").toString();
        outputWidth_=first.value("width").toInt();outputHeight_=first.value("height").toInt();
        if(dataSynchronized_&&targetNs_==outputTarget_&&clock_==outputTarget_) {
            // All native views share this exact indexed scene. Rendering a
            // second camera must not replay the Bag or reset Display history.
            if(!send("render_frame",{{"view_id",outputView_},{"target_ns",outputTarget_},{"width",outputWidth_},{"height",outputHeight_}},{{"output_job",job}}))
                finishOutput(false,{},{},tr("无法发送原生视角离屏请求。"));
            return;
        }
        const bool forward=synchronized_&&sceneReady_&&stamp>targetNs_.toULongLong();
        queuedTarget_.clear();dataSynchronized_=synchronized_=sceneReady_=false;preview_.clear();viewPreviews_.clear();emit previewChanged();
        replayBusy_=send(forward?"bag.advance":"bag.seek",{{"target_ns",outputTarget_}},{{"output_job",job}});emit changed();
        if(!replayBusy_)finishOutput(false,{},{},tr("无法发送准确实验时间恢复请求。"));
    });
    return job;
}
void NativeRvizClient::completeOutputFrame(const QImage &frame,const QVariantMap &metadata)
{
    if(!outputBatch_){finishOutput(true,frame,metadata);return;}
    const auto revision=metadata.value("scene_revision").toString();
    if(revision.isEmpty()||(!outputSceneRevision_.isEmpty()&&revision!=outputSceneRevision_)) {
        finishOutput(false,{},{},tr("原生多视角输出期间场景配置改变。 / Native scene changed during multi-view output."));return;
    }
    outputSceneRevision_=revision;outputMetadata_.append(metadata);
    const auto job=outputJob_;
    // Keep the same output job active through every camera. Scene, time and
    // pointer mutations stay locked, and large images are handed off one at a time.
    emit outputViewFramePrepared(job,outputView_,outputTarget_,frame,metadata);
    if(outputJob_!=job)return;
    if(++outputViewIndex_==outputViews_.size()){finishOutput(true);return;}
    const auto next=outputViews_[outputViewIndex_].toMap();outputView_=next.value("view_id").toString();
    outputWidth_=next.value("width").toInt();outputHeight_=next.value("height").toInt();
    if(!send("render_frame",{{"view_id",outputView_},{"target_ns",outputTarget_},{"width",outputWidth_},{"height",outputHeight_}},{{"output_job",outputJob_}}))
        finishOutput(false,{},{},tr("无法发送原生批量视角离屏请求。 / Could not request the next native view."));
}
void NativeRvizClient::finishOutput(bool success,const QImage &frame,const QVariantMap &metadata,const QString &error)
{
    if(outputJob_.isEmpty())return;
    const auto job=outputJob_,target=outputTarget_;const bool batch=outputBatch_;const auto metrics=outputMetadata_;
    outputJob_.clear();outputTarget_.clear();outputView_.clear();outputWidth_=outputHeight_=0;
    outputBatch_=false;outputViews_.clear();outputMetadata_.clear();outputViewIndex_=0;outputSceneRevision_.clear();
    emit changed();
    if(batch)emit outputFramesPrepared(job,success,target,metrics,error);
    else emit outputFramePrepared(job,success,target,frame,metadata,error);
}
void NativeRvizClient::cancelOutputFrame(const QString &jobId)
{
    if(jobId.isEmpty()||jobId!=outputJob_)return;
    if(replayBusy_)send("bag.cancel");
    dataSynchronized_=synchronized_=sceneReady_=false;emit previewChanged();finishOutput(false,{},{},tr("原生图片输出已取消。"));
}
