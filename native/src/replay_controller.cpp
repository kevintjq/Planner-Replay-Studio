#include "replay_controller.h"

#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <exception>

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
            status_ = result.record->odometryTopic.isEmpty() ? tr("点云已载入。此记录没有里程计。")
                : result.record->poses.empty() ? tr("点云已载入。所选里程计与点云坐标系不匹配。") : tr("载入完成");
            install(std::move(result.record));
        } else {
            status_ = cancelRequested_ ? tr("已取消读取") : result.issue;
            emit recordingChanged();
        }
        emit stateChanged();
    });
}

ReplayController::~ReplayController()
{
    cancelRequested_ = true;
    watcher_.waitForFinished();
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
    mapMode_.reset();
    status_.clear();
    install(replay::makeDemo());
    emit stateChanged();
}

void ReplayController::setCloudTopic(const QString &topic)
{
    if (!path_.isEmpty() && !topic.isEmpty() && topic != cloudTopic())
        load(path_, {topic, odometryTopic(), mapMode_});
}

void ReplayController::setOdometryTopic(const QString &topic)
{
    if (!path_.isEmpty() && !topic.isEmpty() && topic != odometryTopic())
        load(path_, {cloudTopic(), topic, mapMode_});
}

void ReplayController::setReplacing(bool replace)
{
    if (!path_.isEmpty() && replace != replacing())
        load(path_, {cloudTopic(), odometryTopic(), replace ? replay::MapMode::Replace : replay::MapMode::Accumulate});
}

void ReplayController::load(const QString &path, const replay::LoadOptions &options)
{
    if (loading_)
        return;
    pause();
    status_ = tr("正在读取 ROS1 bag");
    loading_ = true;
    progress_ = 0.;
    cancelRequested_ = false;
    emit stateChanged();
    watcher_.setFuture(QtConcurrent::run([this, path, options] {
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
            return Result{std::move(record), path, {}, options.mapMode};
        } catch (const std::exception &error) {
            return Result{nullptr, path, QString::fromUtf8(error.what()), {}};
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
