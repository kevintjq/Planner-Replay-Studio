#include "movie_exporter.h"

#include "replay_controller.h"
#include "video_reader.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPainter>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
constexpr int Width = 1920;
constexpr int Height = 1080;

QString number(double seconds)
{
    return QString::number(seconds, 'f', 9);
}

QString executablePath(const QString &base)
{
    const QString name =
#ifdef Q_OS_WIN
        base + QStringLiteral(".exe");
#else
        base;
#endif
    const QString bundled = QCoreApplication::applicationDirPath() + '/' + name;
    return QFileInfo(bundled).isExecutable() ? bundled : QStandardPaths::findExecutable(name);
}

double probeOrigin(const QString &file)
{
    const QString probe = executablePath(QStringLiteral("ffprobe"));
    if (probe.isEmpty())
        throw std::runtime_error("FFprobe is required for exporting an audio track.");
    QProcess process;
    process.start(probe, {"-v", "error", "-show_entries", "stream=codec_type,start_time", "-of", "json", file});
    if (!process.waitForFinished(3500) || process.exitCode() != 0) {
        process.kill();
        process.waitForFinished(1000);
        throw std::runtime_error("Unable to inspect the video and audio timestamps.");
    }
    double origin = 0.;
    bool foundAudio = false, foundVideo = false;
    const auto streams = QJsonDocument::fromJson(process.readAllStandardOutput()).object().value("streams").toArray();
    for (const auto &stream : streams) {
        const auto entry = stream.toObject();
        const auto kind = entry.value("codec_type").toString();
        if (kind == "audio")
            foundAudio = true;
        if (kind == "video" && !foundVideo) {
            origin = entry.value("start_time").toString().toDouble();
            foundVideo = true;
        }
    }
    if (!foundAudio || !foundVideo)
        throw std::runtime_error("The selected video has no usable audio track.");
    return origin;
}

bool publish(const QString &source, const QString &target)
{
    QFile input(source);
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly) || input.size() <= 0)
        return false;
    while (!input.atEnd()) {
        const auto bytes = input.read(2 * 1024 * 1024);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size())
            return false;
    }
    return output.commit();
}
}

struct MovieExporter::Decoder {
    explicit Decoder(std::vector<replay::VideoClip> list, std::atomic_bool &stop)
        : clips(std::move(list)), readers(clips.size()), cached(clips.size()), indices(clips.size(), -1), cancelled(stop) {}

    std::vector<QImage> decode(double seconds)
    {
        std::vector<QImage> result(clips.size());
        for (size_t index = 0; index < clips.size() && !cancelled; ++index) {
            const auto &clip = clips[index];
            const auto timestamp = clip.timing.sourceSeconds(seconds);
            if (timestamp < 0. || !clip.timing.includes(timestamp))
                continue;
            auto &reader = readers[index];
            if (!reader)
                reader = std::make_unique<replay::VideoReader>(clip.path);
            if (reader->select(timestamp, [this] { return cancelled.load(); })) {
                if (reader->index() != indices[index]) {
                    auto frame = replay::imageOf(reader->frame);
                    if (frame.width() > Width || frame.height() > Height)
                        frame = frame.scaled(Width, Height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    indices[index] = reader->index();
                    cached[index] = std::move(frame);
                }
                result[index] = cached[index];
            }
        }
        return result;
    }

    std::vector<replay::VideoClip> clips;
    std::vector<std::unique_ptr<replay::VideoReader>> readers;
    std::vector<QImage> cached;
    std::vector<int> indices;
    std::atomic_bool &cancelled;
};

void MovieImages::prepare(const std::vector<QImage> &images)
{
    const QMutexLocker locker(&lock_);
    images_ = images;
}

QImage MovieImages::requestImage(const QString &id, QSize *size, const QSize &)
{
    bool ok = false;
    const auto index = id.section('/', 0, 0).toULongLong(&ok);
    const QMutexLocker locker(&lock_);
    const QImage image = ok && index < images_.size() ? images_[size_t(index)] : QImage();
    if (size)
        *size = image.size();
    return image;
}

MovieExporter::MovieExporter(ReplayController &replay, MovieImages &images) : replay_(replay), images_(images)
{
    connect(&process_, &QProcess::started, this, &MovieExporter::requestNext);
    connect(&process_, &QProcess::bytesWritten, this, [this] {
        if (awaitingWrite_ && process_.bytesToWrite() == 0) {
            awaitingWrite_ = false;
            requestNext();
        }
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        diagnostic_ = (diagnostic_ + process_.readAllStandardError()).right(2048);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(tr("无法启动 FFmpeg。"));
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!exporting_)
            return;
        if (cancelled_) {
            finish(tr("已取消视频导出。"));
        } else if (!pendingError_.isEmpty()) {
            finish(pendingError_);
        } else if (code != 0 || status != QProcess::NormalExit || index_ != count_) {
            finish(tr("视频编码失败（%1/%2 帧，退出码 %3）。%4")
                .arg(index_).arg(count_).arg(code).arg(QString::fromUtf8(diagnostic_).trimmed()));
        } else if (!publish(staging_->filePath("movie.mp4"), destination_)) {
            finish(tr("无法写入目标 MP4。"));
        } else {
            finish({});
        }
    });
    connect(&watcher_, &QFutureWatcher<DecodedFrame>::finished, this, &MovieExporter::collectFrame);
    connect(&replay_, &ReplayController::frameExported, this, [this](bool success) {
        if (exporting_ && still_)
            finish(success ? QString() : replay_.status());
    });
}

MovieExporter::~MovieExporter()
{
    cancelled_ = true;
    process_.blockSignals(true);
    process_.kill();
    if (process_.state() != QProcess::NotRunning)
        process_.waitForFinished(2000);
    watcher_.waitForFinished();
}

double MovieExporter::progress() const
{
    return count_ > 0 ? double(index_) / double(count_) : 0.;
}

void MovieExporter::start(const QUrl &destination, QQuickItem *canvas, double begin, double end,
                          double speed, int audioIndex, int width, int height, int fps, int quality, int supersample, bool nvenc)
{
    if (exporting_ || process_.state() != QProcess::NotRunning || watcher_.isRunning() || replay_.loading() || replay_.capturing() || !canvas)
        return;
    const auto reject = [this](const QString &error) {
        message_ = error;
        emit changed();
        emit finished(false, message_);
    };
    if (!destination.isLocalFile() || QFileInfo(destination.toLocalFile()).suffix().compare("mp4", Qt::CaseInsensitive) != 0 ||
        !std::isfinite(begin) || !std::isfinite(end) || begin < 0. || end <= begin || end > replay_.duration() + 1e-6 ||
        (speed != 1. && speed != .5 && speed != .25)) {
        reject(tr("请选择 MP4、有效的时间范围和导出速度。"));
        return;
    }
    if (replay_.referencesFile(destination.toLocalFile())) {
        reject(tr("导出文件不能覆盖项目素材。"));
        return;
    }
    if (width < 320 || width > 3840 || width % 4 || height < 240 || height > 2160 || height % 2 ||
        fps < 1 || fps > 120 || quality < 0 || quality > 51 || (supersample != 1 && supersample != 2)) {
        reject(tr("请检查导出尺寸、帧率和 CRF。"));
        return;
    }
    width_ = width;
    height_ = height;
    fps_ = fps;
    quality_ = quality;
    supersample_ = supersample;
    nvenc_ = nvenc;
    const auto &clips = replay_.videoClips();
    if (audioIndex < -1 || (audioIndex >= 0 && size_t(audioIndex) >= clips.size())) {
        reject(tr("所选音轨已失效。"));
        return;
    }
    executable_ = executablePath(QStringLiteral("ffmpeg"));
    if (executable_.isEmpty()) {
        reject(tr("找不到 FFmpeg。请把 FFmpeg 和 FFprobe 放在程序目录或 PATH 中。"));
        return;
    }
    audio_ = audioIndex < 0 ? std::nullopt : std::optional(clips[size_t(audioIndex)]);
    try {
        if (audio_) {
            const auto audioRate = audio_->timing.rate() * speed;
            if (!std::isfinite(audioRate) || audioRate <= 0.)
                throw std::runtime_error("Invalid audio playback rate.");
            audioOrigin_ = probeOrigin(audio_->path);
        }
    } catch (const std::exception &error) {
        reject(QString::fromUtf8(error.what()));
        return;
    }

    destination_ = destination.toLocalFile();
    staging_ = std::make_unique<QTemporaryDir>(QFileInfo(destination_).absolutePath() + "/.planner-movie-XXXXXX");
    if (!staging_->isValid()) {
        reject(tr("目标目录不可写。"));
        staging_.reset();
        return;
    }
    replay_.pause();
    previous_ = replay_.position();
    canvas_ = canvas;
    begin_ = begin;
    speed_ = speed;
    still_ = false;
    index_ = 0;
    count_ = std::max(qint64(1), qint64(std::ceil((end - begin) * fps_ / speed - 1e-8)));
    cancelled_ = false;
    awaitingWrite_ = false;
    diagnostic_.clear();
    pendingError_.clear();
    decoder_ = std::make_shared<Decoder>(clips, cancelled_);
    message_ = tr("正在导出 %1 × %2 / %3 fps MP4…").arg(width_).arg(height_).arg(fps_);
    exporting_ = true;
    emit changed();
    process_.start(executable_, encoderArguments());
}

void MovieExporter::startStill(const QUrl &destination, QQuickItem *canvas, double time)
{
    if (exporting_ || process_.state() != QProcess::NotRunning || watcher_.isRunning() || replay_.loading() || replay_.capturing() || !canvas)
        return;
    if (!destination.isLocalFile() || QFileInfo(destination.toLocalFile()).suffix().compare("png", Qt::CaseInsensitive) != 0 ||
        replay_.referencesFile(destination.toLocalFile()) || !std::isfinite(time) || time < 0. || time > replay_.duration()) {
        message_ = tr("请选择 PNG 输出路径和有效的截图时间。");
        emit changed();
        emit finished(false, message_);
        return;
    }
    replay_.pause();
    previous_ = replay_.position();
    canvas_ = canvas;
    destination_ = destination.toLocalFile();
    begin_ = time;
    speed_ = 1.;
    count_ = 1;
    index_ = 0;
    cancelled_ = false;
    still_ = true;
    decoder_ = std::make_shared<Decoder>(replay_.videoClips(), cancelled_);
    exporting_ = true;
    message_ = tr("正在导出 PNG…");
    emit changed();
    requestNext();
}

QStringList MovieExporter::encoderArguments() const
{
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-copyts",
                          "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", QString::number(width_) + "x" + QString::number(height_),
                          "-framerate", QString::number(fps_), "-i", "pipe:0"};
    if (audio_) {
        const auto &timing = audio_->timing;
        const double outputDuration = double(count_) / fps_;
        const double sourceStart = timing.sourceSeconds(begin_);
        const double sourceEnd = timing.sourceSeconds(begin_ + outputDuration * speed_);
        std::vector<std::pair<double,double>> spans;
        for (const auto &[first, last] : timing.spans()) {
            const double low = std::max(first, sourceStart);
            const double high = std::min(last.value_or(sourceEnd), sourceEnd);
            if (high > low)
                spans.emplace_back(low, high);
        }
        if (spans.empty()) {
            arguments << "-f" << "lavfi" << "-i" << "anullsrc=r=48000:cl=stereo:d=" + number(outputDuration)
                      << "-map" << "0:v:0" << "-map" << "1:a:0";
        } else {
            const double low = spans.front().first;
            double rate = timing.rate() * speed_;
            const auto actualRate = rate;
            const QString from = number(audioOrigin_ + low);
            QStringList filters{"atrim=start=" + from + ":end=" + number(audioOrigin_ + spans.back().second),
                                "asetpts=PTS-(" + from + ")/TB", "aresample=48000:async=1:first_pts=0"};
            while (rate > 2.) {
                filters << "atempo=2";
                rate /= 2.;
            }
            while (rate < .5) {
                filters << "atempo=0.5";
                rate *= 2.;
            }
            if (std::abs(rate - 1.) > 1e-9)
                filters << "atempo=" + number(rate);
            QStringList muted;
            for (size_t i = 1; i < spans.size(); ++i) {
                if (spans[i-1].second < spans[i].first) {
                    muted << "gte(t," + number((spans[i-1].second - low) / actualRate)
                          + ")*lt(t," + number((spans[i].first - low) / actualRate) + ")";
                }
            }
            if (!muted.isEmpty())
                filters << "asetnsamples=n=128:p=0" << "volume=0:enable='" + muted.join('+') + "'";
            const auto delay = std::llround(std::max(0., (low - sourceStart) / actualRate) * 1000.);
            filters << "adelay=" + QString::number(delay) + ":all=1" << "apad" << "atrim=duration=" + number(outputDuration);
            arguments << "-i" << audio_->path
                      << "-filter_complex" << "[1:a:0]" + filters.join(',') + "[audio]"
                      << "-map" << "0:v:0" << "-map" << "[audio]";
        }
        arguments << "-c:a" << "aac";
    } else {
        arguments << "-an";
    }
    if (nvenc_)
        arguments << "-c:v" << "h264_nvenc" << "-preset" << "p5" << "-rc" << "vbr"
                  << "-cq" << QString::number(quality_) << "-b:v" << "0";
    else
        arguments << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << QString::number(quality_);
    arguments << "-pix_fmt" << "yuv420p" << "-movflags" << "+faststart" << staging_->filePath("movie.mp4");
    return arguments;
}

void MovieExporter::requestNext()
{
    if (!exporting_ || cancelled_)
        return;
    if (index_ == count_) {
        process_.closeWriteChannel();
        return;
    }
    const auto time = begin_ + double(index_) * speed_ / fps_;
    watcher_.setFuture(QtConcurrent::run([decoder = decoder_, time]() -> DecodedFrame {
        try {
            return {decoder->decode(time), {}};
        } catch (const std::exception &error) {
            return {{}, QString::fromUtf8(error.what())};
        }
    }));
}

void MovieExporter::collectFrame()
{
    if (!exporting_ || cancelled_)
        return;
    const auto decoded = watcher_.result();
    if (!decoded.error.isEmpty()) {
        finish(tr("无法解码实拍图层。%1").arg(decoded.error));
        return;
    }
    images_.prepare(decoded.images);
    frames_.clear();
    ++revision_;
    for (size_t i = 0; i < decoded.images.size(); ++i) {
        const auto &image = decoded.images[i];
        const QUrl url = image.isNull() ? QUrl() : QUrl(QStringLiteral("image://movie/")
                          + QString::number(i) + '/' + QString::number(revision_));
        frames_.append(QVariantMap{{"url", url}, {"width", image.width()}, {"height", image.height()}});
    }
    replay_.seek(begin_ + double(index_) * speed_ / fps_);
    emit framesChanged();
    QTimer::singleShot(0, this, &MovieExporter::grabFrame);
}

void MovieExporter::grabFrame()
{
    if (!exporting_ || cancelled_)
        return;
    if (!canvas_ || canvas_->width() <= 0. || canvas_->height() <= 0.) {
        finish(tr("无法读取导出画布。"));
        return;
    }
    if (still_) {
        replay_.exportFrame(QUrl::fromLocalFile(destination_), canvas_);
        return;
    }
    const QSize fit = canvas_->size().toSize().scaled(width_, height_, Qt::KeepAspectRatio);
    grab_ = canvas_->grabToImage(fit * supersample_);
    if (!grab_) {
        finish(tr("无法获取合成帧。"));
        return;
    }
    connect(grab_.data(), &QQuickItemGrabResult::ready, this, [this] {
        const auto grabbed = std::move(grab_);
        if (!exporting_ || cancelled_)
            return;
        auto frame = grabbed->image();
        if (supersample_ > 1 && !frame.isNull())
            frame = frame.scaled(frame.size() / supersample_, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (frame.isNull()) {
            finish(tr("合成帧为空。"));
            return;
        }
        QImage canvas(width_, height_, QImage::Format_RGB888);
        canvas.fill(Qt::black);
        QPainter draw(&canvas);
        draw.drawImage(QPoint((width_ - frame.width()) / 2, (height_ - frame.height()) / 2), frame);
        draw.end();
        const QByteArray data(reinterpret_cast<const char *>(canvas.constBits()), canvas.sizeInBytes());
        awaitingWrite_ = true;
        if (process_.write(data) != data.size()) {
            finish(tr("无法向编码器写入画面。"));
            return;
        }
        ++index_;
        emit changed();
    }, Qt::SingleShotConnection);
}

void MovieExporter::finish(const QString &error)
{
    if (!exporting_)
        return;
    if (process_.state() != QProcess::NotRunning) {
        pendingError_ = error;
        process_.kill();
        return;
    }
    exporting_ = false;
    if (grab_) {
        grab_->disconnect(this);
        grab_.reset();
    }
    decoder_.reset();
    frames_.clear();
    images_.prepare({});
    staging_.reset();
    replay_.seek(previous_);
    message_ = error.isEmpty() ? tr("已导出 %1：%2").arg(still_ ? QStringLiteral("PNG") : QStringLiteral("MP4"), destination_)
                               : error;
    emit framesChanged();
    emit changed();
    emit finished(error.isEmpty(), message_);
}

void MovieExporter::cancel()
{
    if (!exporting_)
        return;
    cancelled_ = true;
    if (process_.state() == QProcess::NotRunning)
        finish(tr("已取消视频导出。"));
    else
        process_.kill();
}
