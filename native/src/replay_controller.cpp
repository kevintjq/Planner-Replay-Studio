#include "replay_controller.h"

#include <QtConcurrentRun>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

ReplayController::ReplayController(QObject *parent) : QObject(parent), record_(replay::makeDemo())
{
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] {
        setPosition(start_ + clock_.nsecsElapsed() / 1.e9);
        if (position_ >= duration())
            pause();
    });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        auto result = watcher_.result();
        loading_ = false;
        if (!cancelRequested_ && result.record) {
            path_ = result.path;
            mapMode_ = result.mode;
            project_ = std::move(result.project);
            status_ = result.record->odometryTopic.isEmpty() ? tr("点云已载入。此记录没有里程计。")
                : result.record->poses.empty() ? tr("点云已载入。所选里程计与点云坐标系不匹配。") : tr("载入完成");
            if (project_)
                status_ += tr(" · 项目设置已保留，当前预览点云。");
            install(std::move(result.record));
        } else {
            status_ = cancelRequested_ ? tr("已取消读取") : result.issue;
            emit recordingChanged();
        }
        emit stateChanged();
    });
    connect(&folderWatcher_, &QFutureWatcher<QString>::finished, this, [this] {
        loading_ = false;
        status_ = folderWatcher_.result();
        emit stateChanged();
    });
}

ReplayController::~ReplayController()
{
    cancelRequested_ = true;
    watcher_.waitForFinished();
    folderWatcher_.waitForFinished();
}

void ReplayController::install(std::shared_ptr<const replay::Recording> record)
{
    pause();
    record_ = std::move(record);
    position_ = 0.;
    emit recordingChanged();
    emit positionChanged();
}

void ReplayController::openBag(const QUrl &url)
{
    if (url.isLocalFile())
        load(url.toLocalFile(), {});
}

void ReplayController::openDemo()
{
    if (loading_)
        return;
    path_.clear();
    project_.reset();
    mapMode_.reset();
    status_.clear();
    install(replay::makeDemo());
    emit stateChanged();
}

void ReplayController::openProject(const QUrl &url)
{
    if (loading_ || !url.isLocalFile())
        return;
    try {
        auto project = replay::ProjectDocument::load(url.toLocalFile());
        if (!project.bag().isEmpty()) {
            if (!QFileInfo(project.bag()).isFile())
                throw std::runtime_error(tr("找不到项目录包：%1").arg(project.bag()).toUtf8().constData());
            const auto settings = project.values();
            const auto mode = settings.value("cloud_mode").toString();
            std::optional<replay::MapMode> mapMode;
            if (mode == QStringLiteral("替换整图"))
                mapMode = replay::MapMode::Replace;
            else if (mode == QStringLiteral("累积扫描"))
                mapMode = replay::MapMode::Accumulate;
            const auto bag = project.bag();
            load(bag, {settings.value("cloud_topic").toString(),
                       settings.value("odom_topic").toString(), mapMode}, std::move(project));
            return;
        }

        path_.clear();
        mapMode_.reset();
        project_ = std::move(project);
        status_ = tr("项目设置已保留。");
        if (project_->source() == "demo") {
            install(replay::makeDemo());
        } else {
            auto empty = std::make_shared<replay::Recording>();
            empty->name = QFileInfo(project_->path()).fileName();
            empty->frames.push_back({0, 0, 0});
            install(std::move(empty));
        }
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

replay::ProjectDocument ReplayController::projectSnapshot() const
{
    if (project_)
        return *project_;
    auto project = replay::ProjectDocument::create(demo() ? QStringLiteral("demo") : QStringLiteral("bag"), path_);
    project.setValue("cloud_topic", cloudTopic());
    project.setValue("odom_topic", odometryTopic());
    project.setValue("cloud_mode", replacing() ? QStringLiteral("替换整图") : QStringLiteral("累积扫描"));
    return project;
}

void ReplayController::saveProjectAs(const QUrl &url)
{
    if (loading_ || !url.isLocalFile())
        return;
    try {
        project_ = projectSnapshot().saveAs(url.toLocalFile());
        status_ = tr("项目已保存：%1").arg(QFileInfo(project_->path()).fileName());
        emit recordingChanged();
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
    emit stateChanged();
}

void ReplayController::exportProjectFolder(const QUrl &parent)
{
    if (loading_ || !parent.isLocalFile())
        return;
    auto document = projectSnapshot();
    QString name = QFileInfo(document.path()).completeBaseName();
    if (name.isEmpty())
        name = QFileInfo(path_).completeBaseName().isEmpty() ? QStringLiteral("Planner Replay")
                                                            : QFileInfo(path_).completeBaseName();
    const QDir directory(parent.toLocalFile());
    QString destination = directory.filePath(name + QStringLiteral(" - project"));
    for (int i = 2; QFileInfo::exists(destination); ++i)
        destination = directory.filePath(QStringLiteral("%1 - project (%2)").arg(name).arg(i));

    pause();
    cancelRequested_ = false;
    progress_ = 0.;
    loading_ = true;
    status_ = tr("正在打包项目和素材…");
    emit stateChanged();
    folderWatcher_.setFuture(QtConcurrent::run([this, document, destination] {
        try {
            const auto manifest = document.exportFolder(destination, [this] { return cancelRequested_.load(); });
            return tr("项目已打包：%1").arg(QDir::toNativeSeparators(manifest));
        } catch (const std::exception &error) {
            return QString::fromUtf8(error.what());
        }
    }));
}

void ReplayController::setCloudTopic(const QString &topic)
{
    if (!path_.isEmpty() && !topic.isEmpty() && topic != cloudTopic()) {
        auto document = project_;
        if (document)
            document->setValue("cloud_topic", topic);
        load(path_, {topic, odometryTopic(), mapMode_}, std::move(document));
    }
}

void ReplayController::setOdometryTopic(const QString &topic)
{
    if (!path_.isEmpty() && !topic.isEmpty() && topic != odometryTopic()) {
        auto document = project_;
        if (document)
            document->setValue("odom_topic", topic);
        load(path_, {cloudTopic(), topic, mapMode_}, std::move(document));
    }
}

void ReplayController::setReplacing(bool replace)
{
    if (!path_.isEmpty() && replace != replacing()) {
        auto document = project_;
        if (document)
            document->setValue("cloud_mode", replace ? QStringLiteral("替换整图") : QStringLiteral("累积扫描"));
        load(path_, {cloudTopic(), odometryTopic(), replace ? replay::MapMode::Replace : replay::MapMode::Accumulate},
             std::move(document));
    }
}

void ReplayController::load(const QString &path, const replay::LoadOptions &options,
                            std::optional<replay::ProjectDocument> project)
{
    if (loading_)
        return;
    pause();
    status_ = tr("正在读取 ROS1 bag");
    loading_ = true;
    progress_ = 0.;
    cancelRequested_ = false;
    emit stateChanged();
    watcher_.setFuture(QtConcurrent::run([this, path, options, project = std::move(project)]() mutable {
        try {
            int lastPercent = -1;
            auto updateProgress = [this, &lastPercent](double value) {
                const int percent = int(value * 100);
                if (percent != lastPercent) {
                    lastPercent = percent;
                    QMetaObject::invokeMethod(this, [this, value] {
                        progress_ = value;
                        emit stateChanged();
                    }, Qt::QueuedConnection);
                }
            };
            auto record = replay::loadBag(path, options, [this] { return cancelRequested_.load(); }, updateProgress);
            return Result{std::move(record), path, {}, options.mapMode, std::move(project)};
        } catch (const std::exception &error) {
            return Result{nullptr, path, QString::fromUtf8(error.what()), {}, {}};
        }
    }));
}

void ReplayController::cancel()
{
    if (loading_) {
        cancelRequested_ = true;
        status_ = tr("正在取消…");
        emit stateChanged();
    }
}

void ReplayController::seek(double seconds)
{
    if (!std::isfinite(seconds))
        return;
    setPosition(seconds);
    if (playing()) {
        start_ = position_;
        clock_.restart();
    }
}

void ReplayController::setPosition(double seconds)
{
    seconds = std::clamp(seconds, 0., duration());
    if (seconds == position_)
        return;
    position_ = seconds;
    emit positionChanged();
}

void ReplayController::togglePlayback()
{
    if (playing()) {
        pause();
    } else if (!loading_ && duration() > 0.) {
        if (position_ >= duration())
            seek(0.);
        start_ = position_;
        clock_.start();
        timer_.start();
        emit playingChanged();
    }
}

void ReplayController::pause()
{
    if (playing()) {
        timer_.stop();
        emit playingChanged();
    }
}

QString ReplayController::speed() const
{
    const auto pose = record_->poseAt(record_->stampAt(position_));
    return pose && pose->speed ? QString::number(*pose->speed, 'f', 1) : QStringLiteral("—");
}

int ReplayController::pointCount() const
{
    const auto &frame = record_->frameAt(record_->stampAt(position_));
    return frame.end - (record_->replace ? frame.first : 0);
}
