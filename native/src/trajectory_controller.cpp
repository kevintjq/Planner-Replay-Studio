#include "trajectory_controller.h"

#include <QFileInfo>
#include <QSaveFile>
#include <QtConcurrentRun>
#include <cmath>
#include <stdexcept>

void TrajectoryImages::setImage(const QImage &image)
{
    const QMutexLocker locker(&lock_);
    image_ = image;
}

QImage TrajectoryImages::requestImage(const QString &, QSize *size, const QSize &requestedSize)
{
    QImage image;
    {
        const QMutexLocker locker(&lock_);
        image = image_;
    }
    if (size)
        *size = image.size();
    if (!image.isNull() && requestedSize.isValid() &&
        (image.width() > requestedSize.width() || image.height() > requestedSize.height()))
        return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

TrajectoryController::TrajectoryController(TrajectoryImages &images) : images_(images)
{
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        auto result = watcher_.result();
        working_ = false;
        if (cancelled_) {
            status_ = tr("已取消。");
        } else {
            status_ = result.message;
            if (result.trajectory) {
                trajectory_ = std::move(result.trajectory);
                excluded_.clear();
                updatePreview();
            } else if (!result.image.isNull()) {
                initial_ = std::move(result.image);
                setImage(initial_);
            }
        }
        emit changed();
    });
}

TrajectoryController::~TrajectoryController()
{
    cancelled_ = true;
    watcher_.waitForFinished();
}

QString TrajectoryController::fileName() const
{
    return QFileInfo(file_).fileName();
}

QUrl TrajectoryController::image() const
{
    if (!revision_ || (!trajectory_ && initial_.isNull()))
        return {};
    return QUrl(QStringLiteral("image://trajectory/") + QString::number(revision_));
}

void TrajectoryController::setImage(const QImage &image)
{
    images_.setImage(image);
    ++revision_;
}

void TrajectoryController::begin(const QString &status, std::function<Result()> job)
{
    cancelled_ = false;
    working_ = true;
    progress_ = 0.;
    status_ = status;
    emit changed();
    watcher_.setFuture(QtConcurrent::run([job = std::move(job)] {
        try {
            return job();
        } catch (const std::exception &error) {
            return Result{{}, {}, QString::fromUtf8(error.what())};
        }
    }));
}

void TrajectoryController::open(const QUrl &file)
{
    if (working_ || !file.isLocalFile())
        return;
    file_ = file.toLocalFile();
    preview(0.);
}

void TrajectoryController::preview(double seconds)
{
    if (working_ || file_.isEmpty() || !std::isfinite(seconds) || seconds < 0.)
        return;
    start_ = seconds;
    trajectory_.reset();
    initial_ = {};
    frames_.clear();
    sampled_.clear();
    excluded_.clear();
    begin(tr("正在读取起始帧…"), [file = file_, seconds] {
        const auto image = replay::readTrajectoryFrame(file, seconds);
        return Result{image, {}, image.isNull() ? tr("该时刻没有视频帧。") : tr("在起始画面框选无人机。")};
    });
}

void TrajectoryController::track(const QRectF &selection, const QString &endSeconds)
{
    if (working_ || initial_.isNull())
        return;
    const bool fixedEnd = !endSeconds.trimmed().isEmpty();
    bool validEnd = false;
    const double end = endSeconds.toDouble(&validEnd);
    if (fixedEnd && (!validEnd || !std::isfinite(end) || end <= start_)) {
        status_ = tr("结束时间必须晚于开始时间。");
        emit changed();
        return;
    }
    begin(tr("正在跟踪无人机并缓存拖影帧…"), [this, file = file_, start = start_, end, fixedEnd, selection] {
        auto result = std::make_shared<replay::FixedCameraTrajectory>(
            replay::trackTrajectory(file, start, fixedEnd ? std::optional(end) : std::nullopt, selection,
                [this] { return cancelled_.load(); },
                [this](double value) {
                    QMetaObject::invokeMethod(this, [this, value] {
                        if (working_) {
                            progress_ = value;
                            emit changed();
                        }
                    }, Qt::QueuedConnection);
                }));
        const QString message = result->lostAt ? tr("跟踪在 %1 秒丢失，已保留此前的帧。").arg(*result->lostAt, 0, 'f', 3)
                                              : tr("已生成拖影，可调整采样和不透明度。");
        return Result{{}, std::move(result), message};
    });
}

void TrajectoryController::configure(double interval, double opacity)
{
    if (!std::isfinite(interval) || interval < 0. || !std::isfinite(opacity) || opacity < 0. || opacity > 1.)
        return;
    interval_ = interval;
    opacity_ = opacity;
    if (!working_ && trajectory_) {
        updatePreview();
        emit changed();
    }
}

void TrajectoryController::updatePreview()
{
    sampled_ = replay::sampleTrajectory(*trajectory_, interval_);
    frames_.clear();
    for (const auto index : sampled_) {
        const auto &frame = trajectory_->frames[index];
        frames_.append(QVariantMap{{"seconds", frame.seconds},
                                   {"excluded", excluded_.contains(int(index))},
                                   {"valid", !frame.sprite.isNull() || !frame.cachedSprite.isEmpty()}});
    }
    setImage(replay::renderTrajectory(*trajectory_, sampled_, opacity_, excluded_, {1120, 760}));
}

void TrajectoryController::toggleFrame(int row)
{
    if (working_ || row < 0 || size_t(row) >= sampled_.size())
        return;
    const int index = int(sampled_[size_t(row)]);
    if (!excluded_.remove(index))
        excluded_.insert(index);
    updatePreview();
    emit changed();
}

void TrajectoryController::restoreFrames()
{
    if (working_ || !trajectory_)
        return;
    excluded_.clear();
    updatePreview();
    emit changed();
}

void TrajectoryController::resetPreview()
{
    if (working_ || initial_.isNull())
        return;
    trajectory_.reset();
    frames_.clear();
    sampled_.clear();
    excluded_.clear();
    setImage(initial_);
    status_ = tr("在起始画面重新框选无人机。");
    emit changed();
}

void TrajectoryController::save(const QUrl &target)
{
    if (working_ || !trajectory_ || !target.isLocalFile())
        return;
    const QString path = target.toLocalFile();
    if (QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) != 0)
        return;
    begin(tr("正在保存原分辨率 PNG…"), [this, path, track = trajectory_, frames = sampled_, opacity = opacity_, excluded = excluded_] {
        QSaveFile file(path);
        const auto exported = replay::renderTrajectory(*track, frames, opacity, excluded);
        if (cancelled_)
            return Result{};
        if (!file.open(QIODevice::WriteOnly) || !exported.save(&file, "PNG"))
            throw std::runtime_error("Unable to write trajectory PNG.");
        if (cancelled_)
            return Result{};
        if (!file.commit())
            throw std::runtime_error("Unable to save trajectory PNG.");
        return Result{{}, {}, tr("已保存拖影 PNG。")};
    });
}

void TrajectoryController::cancel()
{
    cancelled_ = true;
}
