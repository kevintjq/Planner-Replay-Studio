#include "movie_exporter.h"

#include "replay_controller.h"
#include "video_reader.h"
#include "cloud_view.h"
#include "native_rviz_client.h"
#include "still_image_writer.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
QList<QQuickItem *> visualItems(QQuickItem *parent)
{
    QList<QQuickItem *> result{parent};
    for (auto *child : parent->childItems()) result.append(visualItems(child));
    return result;
}

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
        : clips(std::move(list)), readers(clips.size()), cached(clips.size()), timestamps(clips.size()), cancelled(stop) {}

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
                reader = std::make_unique<replay::VideoReader>(clip.path, [this] { return cancelled.load(); });
            if (reader->select(timestamp, [this] { return cancelled.load(); })) {
                if (cached[index].isNull() || reader->pts() != timestamps[index]) {
                    auto frame = replay::imageOf(reader->frame);
                    timestamps[index] = reader->pts();
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
    std::vector<qint64> timestamps;
    std::atomic_bool &cancelled;
};

void MovieImages::prepare(const std::vector<QImage> &images)
{
    const QMutexLocker locker(&lock_);
    images_ = images;
}

void MovieImages::prepareNative(const QHash<QString, QImage> &images)
{
    const QMutexLocker locker(&lock_);
    native_ = images;
}

QImage MovieImages::requestImage(const QString &id, QSize *size, const QSize &)
{
    if (id.section('/', 0, 0) == "native") {
        const QMutexLocker locker(&lock_);
        const auto image = native_.value(QUrl::fromPercentEncoding(id.section('/', 1, 1).toUtf8()));
        if (size) *size = image.size();
        return image;
    }
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
    graphicsDeadline_.setSingleShot(true);
    graphicsDeadline_.setInterval(30000);
    connect(&graphicsDeadline_, &QTimer::timeout, this, [this] {
        if (exporting_ && graphicsWaiting_)
            finish(tr("三维渲染在 30 秒内未就绪，已停止导出。 / Graphics did not become ready within 30 seconds."));
    });
    nativeDeadline_.setSingleShot(true);
    nativeDeadline_.setInterval(360000);
    connect(&nativeDeadline_, &QTimer::timeout, this, [this] {
        if (!nativeJob_.isEmpty()) finish(tr("原生场景输出超时，已停止导出。 / Native scene output timed out."));
    });
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
    connect(&audioProbe_, &QFutureWatcher<AudioProbe>::finished, this, [this] {
        if (!exporting_) return;
        if (cancelled_) { finish(tr("已取消导出。 / Export cancelled.")); return; }
        const auto probe = audioProbe_.result();
        if (!probe.error.isEmpty()) { finish(probe.error); return; }
        audioOrigin_ = probe.origin;
        message_ = tr("正在导出 %1 × %2 / %3 fps MP4…").arg(width_).arg(height_).arg(fps_);
        emit changed();
        if (exporting_ && !cancelled_) process_.start(executable_, encoderArguments());
    });
    connect(&stillWriter_, &QFutureWatcher<QString>::finished, this, [this] {
        if (exporting_ && still_) finish(stillWriter_.result());
    });
    connect(&replay_, &ReplayController::frameExported, this, [this](bool success) {
        if (exporting_ && still_)
            finish(success ? QString() : replay_.status());
    });
}

void MovieExporter::setNativeClient(NativeRvizClient *client)
{
    if (nativeClient_ == client) return;
    if (exporting_ && nativeMode_) {
        const auto job = std::exchange(nativeJob_, {}); nativeDeadline_.stop();
        if (!job.isEmpty() && nativeClient_) nativeClient_->cancelOutputFrame(job);
        finish(tr("导出期间原生数据源改变。 / Native source changed during export."));
    }
    if (nativeClient_) disconnect(nativeClient_, nullptr, this, nullptr);
    nativeClient_ = client;
    if (!client) return;
    connect(client, &NativeRvizClient::outputViewFramePrepared, this, &MovieExporter::acceptNativeViewFrame);
    connect(client, &NativeRvizClient::outputFramesPrepared, this, &MovieExporter::acceptNativeFrames);
    connect(client, &NativeRvizClient::changed, this, [this] {
        if (exporting_ && nativeCapture_ && (!nativeClient_ || !nativeClient_->connected()))
            finish(tr("原生 RViz 已断开，导出停止；不会替换为旧场景。 / Native RViz disconnected; export stopped."));
    });
    connect(client, &QObject::destroyed, this, [this] {
        nativeClient_ = nullptr;
        if (exporting_ && nativeCapture_)
            finish(tr("原生场景已关闭，导出停止。 / Native scene closed; export stopped."));
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
    audioProbe_.waitForFinished();
    stillWriter_.waitForFinished();
    restoreCapture();
}

double MovieExporter::progress() const
{
    return count_ > 0 ? double(index_) / double(count_) : 0.;
}

void MovieExporter::start(const QUrl &destination, QQuickItem *canvas, double begin, double end,
                          double speed, int audioIndex, int width, int height, int fps, int quality, int supersample, bool nvenc)
{
    if (exporting_ || process_.state() != QProcess::NotRunning || watcher_.isRunning() || audioProbe_.isRunning() || stillWriter_.isRunning() || replay_.loading() || replay_.capturing() || !canvas)
        return;
    const auto reject = [this](const QString &error) {
        message_ = error;
        emit changed();
        emit finished(false, message_);
    };
    if (!destination.isLocalFile() || QFileInfo(destination.toLocalFile()).suffix().compare("mp4", Qt::CaseInsensitive) != 0 ||
        !std::isfinite(begin) || !std::isfinite(end) || begin < 0. || end <= begin || end > replay_.duration() + 1e-6 ||
        (speed != 1. && speed != .5 && speed != .25 && speed != 2. && speed != 4.)) {
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
    if (audio_) {
        const auto audioRate = audio_->timing.rate() * speed;
        if (!std::isfinite(audioRate) || audioRate <= 0.) {
            reject(tr("无效音轨播放速率。 / Invalid audio playback rate.")); return;
        }
    }

    destination_ = destination.toLocalFile();
    staging_ = std::make_unique<QTemporaryDir>(QFileInfo(destination_).absolutePath() + "/.planner-movie-XXXXXX");
    if (!staging_->isValid()) {
        reject(tr("目标目录不可写。"));
        staging_.reset();
        return;
    }
    const auto captureError = prepareCapture(canvas, canvas, width_, height_, supersample_, {});
    if (!captureError.isEmpty()) {
        staging_.reset(); reject(captureError); return;
    }
    previousPlaying_ = replay_.playing();
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
    if (audio_) {
        message_ = tr("正在检查音轨时间戳… / Inspecting audio timestamps…");
        audioProbe_.setFuture(QtConcurrent::run([this, file = audio_->path]() -> AudioProbe {
            try {
                replay::VideoReader reader(file, [this] { return cancelled_.load(); });
                if (!reader.hasAudio()) return {0., tr("所选视频没有音轨。 / Selected video has no audio track.")};
                // Exactly the decoded origin used by previews/source frames; no FPS or stream-start estimate.
                return {double(reader.originPts()) * reader.timeBaseNumerator() / reader.timeBaseDenominator(), {}};
            } catch (const std::exception &error) { return {0., QString::fromUtf8(error.what())}; }
        }));
    }
    emit changed();
    if (!audio_ && exporting_ && !cancelled_) process_.start(executable_, encoderArguments());
}

void MovieExporter::startStill(const QUrl &destination, QQuickItem *canvas, double time, const QVariantMap &options)
{
    if (exporting_ || process_.state() != QProcess::NotRunning || watcher_.isRunning() || audioProbe_.isRunning() || stillWriter_.isRunning() || replay_.loading() || replay_.capturing() || !canvas)
        return;
    const auto format = QFileInfo(destination.toLocalFile()).suffix().toLower();
    const int width = options.value("width", 1920).toInt(), height = options.value("height", 1080).toInt();
    const int dpi = options.value("dpi", 300).toInt();
    if (options.contains("background") && !QColor(options.value("background").toString()).isValid()) {
        message_ = tr("请输入有效背景颜色，例如 #FFFFFF。 / Invalid background color.");
        emit changed(); emit finished(false, message_); return;
    }
    if (!destination.isLocalFile() || !QStringList{"png", "jpg", "jpeg", "tif", "tiff", "pdf", "svg"}.contains(format) ||
        replay_.referencesFile(destination.toLocalFile()) || !std::isfinite(time) || time < 0. || time > replay_.duration()) {
        message_ = tr("请选择有效图像路径及实验时间；不能覆盖源素材。 / Invalid destination or time.");
        emit changed();
        emit finished(false, message_);
        return;
    }
    if (width < 16 || height < 16 || width > 8192 || height > 8192 || dpi < 36 || dpi > 2400 ||
        qint64(width) * height > 40'000'000 || canvas->width() <= 0 || canvas->height() <= 0) {
        message_ = tr("尺寸范围 16–8192，最多 4000 万像素；DPI 范围 36–2400。");
        emit changed(); emit finished(false, message_); return;
    }
    // Resolve an individual view only after decoding and seeking the requested
    // experiment time. Its current visibility/geometry may belong to another
    // keyframe or visibility interval, including between batch jobs.
    const auto captureError = prepareCapture(canvas, canvas, width, height, 1, options);
    if (!captureError.isEmpty()) { message_ = captureError; emit changed(); emit finished(false, message_); return; }
    previousPlaying_ = replay_.playing();
    replay_.pause();
    previous_ = replay_.position();
    width_ = width; height_ = height; dpi_ = dpi;
    destination_ = destination.toLocalFile();
    begin_ = time;
    speed_ = 1.;
    count_ = 1;
    index_ = 0;
    cancelled_ = false;
    still_ = true;
    decoder_ = std::make_shared<Decoder>(replay_.videoClips(), cancelled_);
    exporting_ = true;
    message_ = tr("正在导出 %1 × %2 / %3 DPI…").arg(width_).arg(height_).arg(dpi_);
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
    if (!exporting_)
        return;
    if (cancelled_) { finish(tr("已取消导出。 / Export cancelled.")); return; }
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
    // First let bindings apply this exact frame's visibility/animated geometry.
    // Native scene pixels are requested at that geometry's real output size.
    QTimer::singleShot(0, this, &MovieExporter::prepareNativeFrame);
}

void MovieExporter::prepareNativeFrame()
{
    if (!exporting_ || cancelled_) return;
    if (!canvas_ || !captureItem_) { finish(tr("导出画布已删除。 / Export canvas was deleted.")); return; }
    if (canvas_->property("nativeSceneActive").toBool() != nativeMode_) {
        finish(tr("导出期间场景后端改变。 / Scene backend changed during export.")); return;
    }
    auto captureOptions = canvas_->property("captureOptions").toMap();
    if (still_ && captureOptions.value("scope").toString() == "view") {
        QQuickItem *target = nullptr;
        const auto viewId = captureOptions.value("viewId").toString();
        if (!viewId.isEmpty()) for (auto *item : visualItems(canvas_)) {
            if (!item->isVisible() || item->property("exportViewId").toString() != viewId) continue;
            if (target) { finish(tr("目标时间存在重复的视角标识。 / Duplicate view identity at the requested time.")); return; }
            target = item;
        }
        if (!target || target->width() <= 0. || target->height() <= 0.) {
            finish(tr("选中视角在目标实验时间不可见。 / Selected view is not visible at the requested experiment time.")); return;
        }
        captureItem_ = target;
        const auto fit = target->size().toSize().scaled(width_, height_, Qt::KeepAspectRatio);
        const double scale = double(fit.width()) / target->width();
        if (std::abs(captureOptions.value("renderScale").toDouble() - scale) > 1e-9) {
            captureOptions.insert("renderScale", scale);
            canvas_->setProperty("captureOptions", captureOptions);
            // Apply output-scale bindings before requesting native pixels or
            // grabbing the selected item's high-resolution composition.
            QTimer::singleShot(0, this, &MovieExporter::prepareNativeFrame);
            return;
        }
    }
    nativeViews_.clear(); nativeFrames_.clear(); nativeMetrics_.clear();
    nativeFrameUrl_ = {}; nativeFrameSources_.clear();
    images_.prepareNative({});
    const auto fit = captureItem_->size().toSize().scaled(width_, height_, Qt::KeepAspectRatio)
        * (still_ ? 1 : supersample_);
    const double scale = captureItem_->width() > 0. ? double(fit.width()) / captureItem_->width() : 0.;
    QVariantList requests;
    qint64 totalPixels = 0;
    for (auto *item : visualItems(captureItem_)) {
        if (!item->isVisible()) continue;
        if (nativeMode_ && qobject_cast<CloudView *>(item)) {
            finish(tr("原生场景的独立附加机位尚未接入输出；请隐藏附加机位或单独导出主视角。 / Native multi-view export is not ready; legacy views cannot substitute."));
            return;
        }
        if (!nativeMode_ || !item->property("nativeSceneImage").toBool()) continue;
        const auto id = item->property("exportViewId").toString();
        if (id.isEmpty() || nativeViews_.contains(id)) {
            finish(tr("原生输出的视角标识缺失或重复。 / Missing or duplicate native output view identity.")); return;
        }
        const QSize pixels(qCeil(item->width() * scale), qCeil(item->height() * scale));
        if (scale <= 0. || pixels.width() < 1 || pixels.height() < 1 || pixels.width() > 16384
            || pixels.height() > 16384 || qint64(pixels.width()) * pixels.height() > 64LL * 1024 * 1024) {
            finish(tr("原生离屏尺寸无效或超过输出资源限制。 / Invalid native offscreen size or output resource limit.")); return;
        }
        totalPixels += qint64(pixels.width()) * pixels.height();
        bool mapped = false;
        const auto transform = item->itemTransform(captureItem_, &mapped);
        if (!mapped) { finish(tr("无法映射原生机位的画布变换。 / Could not map native camera geometry.")); return; }
        nativeViews_.insert(id, {item, pixels, transform});
        requests.append(QVariantMap{{"view_id", id}, {"width", pixels.width()}, {"height", pixels.height()}});
    }
    if (nativeViews_.size() > 16 || totalPixels > 96LL * 1024 * 1024) {
        finish(tr("原生多视角合成超过 16 个机位或 96 MP 内存预算。 / Native composition exceeds its view or memory budget.")); return;
    }
    nativeCapture_ = nativeMode_ && !nativeViews_.isEmpty();
    emit framesChanged();
    if (!nativeCapture_) { grabFrame(); return; }
    const QColor requestedBackground(captureOptions.value("background").toString());
    if (replay_.transparentCanvas() || (requestedBackground.isValid() && requestedBackground.alpha() < 255)) {
        finish(tr("原生场景透明离屏输出尚未接入；不会以不透明场景冒充透明图。 / Transparent native scene output is not ready.")); return;
    }
    if (!nativeClient_ || !nativeClient_->connected() || !nativeClient_->indexed()) {
        finish(tr("原生场景没有可用的精确时间索引；导出已停止。 / Native scene requires a connected, indexed source for exact output.")); return;
    }
    nativeTargetNs_ = replay_.experimentStampNs();
    if (nativeTargetNs_.isEmpty()) {
        finish(tr("无法确定真实实验纳秒时间；不会使用预览或旧场景输出。 / Exact experiment timestamp is unavailable for native export.")); return;
    }
    message_ = tr("正在准备 %1 个原生机位，实验时间 %2 s… / Preparing exact native cameras…")
        .arg(nativeViews_.size()).arg(begin_ + double(index_) * speed_ / fps_, 0, 'f', 9);
    nativeJob_ = nativeClient_->prepareOutputFrames(nativeTargetNs_, requests);
    if (nativeJob_.isEmpty()) { finish(tr("无法创建原生输出任务。 / Could not create native output job.")); return; }
    nativeDeadline_.start(); emit changed();
}

void MovieExporter::acceptNativeViewFrame(const QString &job, const QString &viewId, const QString &targetNs,
                                         const QImage &frame, const QVariantMap &metadata)
{
    if (!exporting_ || cancelled_ || nativeJob_.isEmpty() || job != nativeJob_) return;
    const auto expected = nativeViews_.value(viewId);
    const bool exact = nativeClient_ && nativeClient_->connected() && canvas_ && nativeMode_
        && canvas_->property("nativeSceneActive").toBool() && targetNs == nativeTargetNs_
        && replay_.experimentStampNs() == nativeTargetNs_
        && expected.item && !nativeFrames_.contains(viewId)
        && metadata.value("view_id").toString() == viewId
        && metadata.value("target_ns").toString() == nativeTargetNs_
        && metadata.value("clock_ns").toString() == nativeTargetNs_
        && metadata.value("native_render_texture").toBool()
        && metadata.value("native_scene_ready").toBool() && metadata.value("required_tf_complete").toBool()
        && metadata.value("experiment_playhead_synchronized").toBool()
        && !metadata.value("scene_revision").toString().isEmpty()
        && frame.size() == expected.size && !frame.isNull()
        && metadata.value("width").toInt() == expected.size.width()
        && metadata.value("height").toInt() == expected.size.height()
        && metadata.value("gpu_max_texture_size").toInt() >= std::max(expected.size.width(), expected.size.height());
    if (!exact) {
        finish(tr("原生输出的时间、就绪状态或真实渲染尺寸不匹配，已停止导出。 / Native output time, readiness or render size mismatch.")); return;
    }
    const auto sceneRevision = metadata.value("scene_revision").toString();
    if (!nativeSceneRevision_.isEmpty() && sceneRevision != nativeSceneRevision_) {
        finish(tr("导出期间原生场景配置改变，已停止导出。 / Native scene configuration changed during export.")); return;
    }
    nativeSceneRevision_ = sceneRevision;
    const QColor requestedBackground(canvas_->property("captureOptions").toMap().value("background").toString());
    const QColor nativeBackground(metadata.value("background_color").toString());
    if (requestedBackground.isValid() && (!nativeBackground.isValid() || requestedBackground != nativeBackground)) {
        finish(tr("当前原生离屏输出保留 RViz 场景背景，暂不能覆盖为指定颜色。 / Native output background override is not ready; RViz scene color was preserved.")); return;
    }
    auto metric = metadata; metric.insert("view", viewId);
    nativeMetrics_.append(metric); nativeFrames_.insert(viewId, frame);
}

void MovieExporter::acceptNativeFrames(const QString &job, bool success, const QString &targetNs,
                                      const QVariantList &metadata, const QString &error)
{
    if (!exporting_ || cancelled_ || nativeJob_.isEmpty() || job != nativeJob_) return;
    nativeJob_.clear(); nativeDeadline_.stop();
    if (!success) { finish(tr("原生场景导出失败：%1 / Native output failed.").arg(error)); return; }
    if (targetNs != nativeTargetNs_ || nativeFrames_.size() != nativeViews_.size()
        || metadata.size() != nativeViews_.size() || nativeMetrics_.size() != nativeViews_.size()) {
        finish(tr("原生多视角输出不完整，未发布合成帧。 / Incomplete native output; composition was not published.")); return;
    }
    // Publish atomically only after every camera passed the same-time batch.
    // Missing cameras keep an empty source throughout, never a preview fallback.
    images_.prepareNative(nativeFrames_); nativeFrames_.clear();
    ++revision_;
    for (auto it = nativeViews_.cbegin(); it != nativeViews_.cend(); ++it)
        nativeFrameSources_.insert(it.key(), QUrl(QStringLiteral("image://movie/native/")
            + QString::fromLatin1(QUrl::toPercentEncoding(it.key())) + '/' + QString::number(revision_)));
    nativeFrameUrl_ = nativeFrameSources_.value("main").toUrl();
    nativeImageWait_.start();
    emit framesChanged();
    QTimer::singleShot(0, this, &MovieExporter::grabFrame);
}

QString MovieExporter::nativeSceneError() const
{
    if (!canvas_ || !captureItem_ || canvas_->property("nativeSceneActive").toBool() != nativeMode_)
        return tr("导出期间场景后端改变。 / Scene backend changed during export.");
    if (!nativeMode_) return {};
    const auto sceneClient = canvas_->property("nativeSceneClient").value<QObject *>();
    if (sceneClient != nativeClient_)
        return tr("导出源与当前原生画布不一致。 / Native output source does not match the current canvas.");
    QSet<QString> visible;
    for (auto *item : visualItems(captureItem_)) {
        if (!item->isVisible()) continue;
        if (qobject_cast<CloudView *>(item))
            return tr("原生输出不能混入旧固定话题机位。 / Legacy views cannot substitute in native output.");
        if (item->property("nativeSceneImage").toBool()) {
            const auto id = item->property("exportViewId").toString();
            if (!nativeCapture_ || visible.contains(id) || nativeViews_.value(id).item != item)
                return tr("原生机位在输出期间改变。 / Native output view changed.");
            visible.insert(id);
        }
    }
    if (visible.size() != nativeViews_.size()) return tr("原生机位在输出期间删除或隐藏。 / Native output view was removed or hidden.");
    if (!nativeCapture_) return {};
    if (!nativeClient_ || !nativeClient_->connected() || replay_.experimentStampNs() != nativeTargetNs_
        || nativeFrameSources_.size() != nativeViews_.size())
        return tr("原生准确帧尚未进入合成画布；不会使用预览替代。 / Exact native frame is not ready in the output canvas.");
    const auto fit = captureItem_->size().toSize().scaled(width_, height_, Qt::KeepAspectRatio)
        * (still_ ? 1 : supersample_);
    const double scale = captureItem_->width() > 0. ? double(fit.width()) / captureItem_->width() : 0.;
    for (auto it = nativeViews_.cbegin(); it != nativeViews_.cend(); ++it) {
        const auto &view = it.value();
        if (!view.item || view.item->property("source").toUrl() != nativeFrameSources_.value(it.key()).toUrl()
            || view.item->property("status").toInt() != 1)
            return tr("原生准确帧尚未进入合成画布；不会使用预览替代。 / Exact native frame is not ready in the output canvas.");
        bool mapped = false;
        const auto transform = view.item->itemTransform(captureItem_, &mapped);
        if (!mapped || scale <= 0. || QSize(qCeil(view.item->width() * scale), qCeil(view.item->height() * scale)) != view.size
            || transform != view.transform)
            return tr("原生视角的输出尺寸或变换在渲染期间改变，已停止导出。 / Native output geometry changed during rendering.");
    }
    return {};
}

void MovieExporter::grabFrame()
{
    if (!exporting_ || cancelled_)
        return;
    if (!canvas_ || canvas_->width() <= 0. || canvas_->height() <= 0.) {
        finish(tr("无法读取导出画布。"));
        return;
    }
    const auto dataError = canvas_->property("outputDataError").toString();
    if (!dataError.isEmpty()) { finish(dataError); return; }
    if (canvas_->property("outputDataPending").toBool()) {
        if (captureDataWait_.elapsed() >= 120000) {
            finish(tr("完整科研数据仍未就绪，已停止导出以避免使用临时预览数据。 / Full scientific data is not ready."));
            return;
        }
        QTimer::singleShot(20, this, &MovieExporter::grabFrame);
        return;
    }
    // Image providers can be asynchronous. Wait without blocking the UI, then
    // enforce Ready (1); never grab a loading image or fall back to the preview.
    const bool loadingNative = std::any_of(nativeViews_.cbegin(), nativeViews_.cend(), [this](const auto &view) {
        const auto id = view.item ? view.item->property("exportViewId").toString() : QString();
        return view.item && nativeFrameSources_.contains(id)
            && view.item->property("source").toUrl() == nativeFrameSources_.value(id).toUrl()
            && view.item->property("status").toInt() == 2;
    });
    if (nativeCapture_ && loadingNative && nativeImageWait_.isValid() && nativeImageWait_.elapsed() < 5000) {
        QTimer::singleShot(10, this, &MovieExporter::grabFrame); return;
    }
    if (!prepareCaptureGraphics()) return;
    const auto captureError = updateCaptureSizes();
    if (!captureError.isEmpty()) { finish(captureError); return; }
    if (still_) {
        if (!captureItem_) { finish(tr("导出视角已删除。")); return; }
        const auto fit = captureItem_->size().toSize().scaled(width_, height_, Qt::KeepAspectRatio);
        grab_ = captureItem_->grabToImage(fit);
        if (!grab_) { finish(tr("无法创建离屏图像。")); return; }
        connect(grab_.data(), &QQuickItemGrabResult::ready, this, [this] {
            const auto grabbed = std::move(grab_);
            if (!exporting_ || cancelled_) return;
            saveStill(grabbed->image());
        }, Qt::SingleShotConnection);
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
        const auto captureError = validateCapture();
        if (!captureError.isEmpty()) { finish(captureError); return; }
        emit frameCaptured(frame, begin_ + double(index_) * speed_ / fps_);
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
    restoreCapture();
    if (grab_) {
        grab_->disconnect(this);
        grab_.reset();
    }
    decoder_.reset();
    frames_.clear();
    images_.prepare({});
    staging_.reset();
    replay_.seek(previous_);
    if (previousPlaying_) replay_.togglePlayback();
    previousPlaying_ = false;
    message_ = error.isEmpty() ? tr("已导出 %1：%2").arg(still_ ? QStringLiteral("Image") : QStringLiteral("MP4"), destination_)
                               : error;
    emit framesChanged();
    emit changed();
    emit finished(error.isEmpty(), message_);
}

void MovieExporter::restoreCapture()
{
    ++captureGeneration_;
    clearGraphicsWait();
    for (const auto &connection : captureConnections_) disconnect(connection);
    captureConnections_.clear(); captureWindow_ = nullptr;
    nativeDeadline_.stop();
    const auto job = std::exchange(nativeJob_, {});
    if (!job.isEmpty() && nativeClient_) nativeClient_->cancelOutputFrame(job);
    nativeMode_ = nativeCapture_ = false; nativeViews_.clear(); nativeFrames_.clear();
    nativeFrameUrl_ = {}; nativeFrameSources_.clear(); nativeTargetNs_.clear(); nativeSceneRevision_.clear(); nativeMetrics_.clear();
    nativeImageWait_.invalidate();
    images_.prepareNative({});
    if (canvas_) canvas_->setProperty("captureOptions", previousCaptureOptions_);
    for (const auto &[item, size] : captureViews_)
        if (auto *view = qobject_cast<CloudView *>(item.data())) view->setCaptureSize(size);
    captureViews_.clear(); captureItem_ = nullptr; canvas_ = nullptr; previousCaptureOptions_ = {};
}

QString MovieExporter::prepareCapture(QQuickItem *canvas, QQuickItem *target, int width, int height,
                                      int supersample, const QVariantMap &options)
{
    if (!canvas || !target || target->width() <= 0. || target->height() <= 0.)
        return tr("无法读取导出画布。 / Invalid export canvas.");
    const auto fit = target->size().toSize().scaled(width, height, Qt::KeepAspectRatio) * supersample;
    const double scale = double(fit.width()) / target->width();
    const auto items = visualItems(target);
    const bool nativeMode = canvas->property("nativeSceneActive").toBool();
    if (nativeMode && (!nativeClient_ || !nativeClient_->connected()))
        return tr("原生输出源未连接；不能使用预览或旧场景替代。 / Native output source is disconnected; preview or legacy substitution is prohibited.");
    if (nativeMode && canvas->property("nativeSceneClient").value<QObject *>() != nativeClient_)
        return tr("导出源与当前原生画布不一致。 / Native output source does not match the current canvas.");
    for (auto *item : items) {
        auto *view = qobject_cast<CloudView *>(item);
        if (!view || !view->isVisible()) continue;
        // Export options may hide these views. Native-mode legacy rejection runs
        // after exact-time bindings and layer selection have actually applied.
        if (nativeMode || options.value("scope").toString() == "view") continue;
        const QSize pixels(qCeil(view->width() * scale), qCeil(view->height() * scale));
        // A new view has no GPU limit until its first render synchronization.
        // Defer that case to the asynchronous exact-frame preflight, while
        // retaining immediate rejection when a known limit is exceeded.
        if (view->textureLimit() > 0 && std::max({pixels.width(), pixels.height(), width * supersample, height * supersample}) > view->textureLimit())
            return tr("导出超过 GPU 最大纹理尺寸；当前版本尚未启用分块渲染。 / Export exceeds GPU texture limits.");
    }
    canvas_ = canvas; captureItem_ = target; nativeMode_ = nativeMode;
    captureDataWait_.start();
    previousCaptureOptions_ = canvas->property("captureOptions");
    auto captureOptions = options; captureOptions.insert("renderScale", scale);
    canvas->setProperty("captureOptions", captureOptions);
    captureViews_.clear(); captureMetrics_.clear();
    ++captureGeneration_;
    if (canvas->window()) watchCaptureWindow(canvas->window());
    return {};
}

void MovieExporter::watchCaptureWindow(QQuickWindow *window)
{
    if (!window || captureWindow_ == window) return;
    captureWindow_ = window;
    const auto generation = captureGeneration_;
    captureConnections_.append(connect(window, &QQuickWindow::sceneGraphError, this,
        [this, generation](QQuickWindow::SceneGraphError error, const QString &message) {
            if (exporting_ && generation == captureGeneration_)
                finish(tr("导出图形上下文失败：%1（%2）。 / Export graphics context failed.").arg(message).arg(int(error)));
        }));
    captureConnections_.append(connect(window, &QWindow::visibleChanged, this, [this, generation](bool visible) {
        if (exporting_ && generation == captureGeneration_ && !visible)
            finish(tr("导出窗口已关闭或隐藏。 / The export window was closed or hidden."));
    }));
    captureConnections_.append(connect(window, &QObject::destroyed, this, [this, generation] {
        if (exporting_ && generation == captureGeneration_)
            finish(tr("导出窗口已销毁。 / The export window was destroyed."));
    }));
    captureConnections_.append(connect(canvas_, &QQuickItem::windowChanged, this, [this, generation](QQuickWindow *current) {
        if (exporting_ && generation == captureGeneration_ && current != captureWindow_)
            finish(tr("导出画布已离开原窗口。 / The export canvas left its window."));
    }));
}

bool MovieExporter::prepareCaptureGraphics()
{
    auto *window = captureItem_ ? captureItem_->window() : nullptr;
    if (!window || !window->isVisible() || (captureWindow_ && captureWindow_ != window)) {
        finish(tr("导出画布没有可用的可见窗口。 / Export canvas has no available visible window."));
        return false;
    }
    watchCaptureWindow(window);
    QList<CloudView *> pending;
    for (auto *item : visualItems(captureItem_)) {
        auto *view = qobject_cast<CloudView *>(item);
        if (view && view->isVisible() && view->textureLimit() <= 0) pending.append(view);
    }
    if (window->isSceneGraphInitialized() && pending.isEmpty()) {
        clearGraphicsWait();
        return true;
    }
    if (!graphicsWaiting_) {
        graphicsWaiting_ = true;
        graphicsDeadline_.start();
        message_ = tr("正在准备三维渲染… / Preparing graphics for export…");
        emit changed();
    }
    // Re-evaluate the exact target's current views after every readiness signal.
    // Keep the original deadline running if more than one view is pending.
    for (const auto &connection : graphicsConnections_) disconnect(connection);
    graphicsConnections_.clear();
    graphicsConnections_.append(connect(window, &QQuickWindow::sceneGraphInitialized,
        this, &MovieExporter::resumeGraphicsCapture, Qt::QueuedConnection));
    for (auto *view : pending) {
        graphicsConnections_.append(connect(view, &CloudView::textureLimitChanged, this, &MovieExporter::resumeGraphicsCapture));
        graphicsConnections_.append(connect(view, &QObject::destroyed, this, &MovieExporter::resumeGraphicsCapture));
        view->update();
    }
    window->update();
    return false;
}

void MovieExporter::resumeGraphicsCapture()
{
    if (!exporting_ || cancelled_ || !graphicsWaiting_ || graphicsResumeScheduled_) return;
    graphicsResumeScheduled_ = true;
    const auto generation = captureGeneration_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != captureGeneration_) return;
        graphicsResumeScheduled_ = false;
        if (exporting_ && !cancelled_ && graphicsWaiting_) grabFrame();
    });
}

void MovieExporter::clearGraphicsWait()
{
    graphicsDeadline_.stop();
    for (const auto &connection : graphicsConnections_) disconnect(connection);
    graphicsConnections_.clear();
    graphicsWaiting_ = graphicsResumeScheduled_ = false;
}

QString MovieExporter::updateCaptureSizes()
{
    if (!captureItem_ || !captureItem_->isVisible() || captureItem_->width() <= 0. || captureItem_->height() <= 0.)
        return tr("导出视角不可用。 / Export view is unavailable.");
    const auto nativeError = nativeSceneError();
    if (!nativeError.isEmpty()) return nativeError;
    const auto fit = captureItem_->size().toSize().scaled(width_, height_, Qt::KeepAspectRatio) * (still_ ? 1 : supersample_);
    const double scale = double(fit.width()) / captureItem_->width();
    // Geometry and visibility can change with the exact frame's animation state.
    for (auto *item : visualItems(captureItem_)) {
        auto *view = qobject_cast<CloudView *>(item);
        if (!view || !view->isVisible()) continue;
        const auto cameraError = view->exportValidationError(begin_ + double(index_) * speed_ / fps_);
        if (!cameraError.isEmpty()) return cameraError;
        const QSize pixels(qCeil(view->width() * scale), qCeil(view->height() * scale));
        if (view->textureLimit() <= 0 || std::max({pixels.width(), pixels.height(), fit.width(), fit.height()}) > view->textureLimit())
            return tr("当前动画帧超过 GPU 纹理上限；尚未启用分块渲染。 / Frame exceeds GPU texture limits.");
        if (std::none_of(captureViews_.begin(), captureViews_.end(), [view](const auto &entry) { return entry.first == view; }))
            captureViews_.append(qMakePair(QPointer<QQuickItem>(view), view->captureSize()));
        view->setCaptureSize(pixels);
    }
    return {};
}

QString MovieExporter::validateCapture()
{
    if (!captureItem_ || !captureItem_->isVisible())
        return tr("导出视角在目标实验时间不可见。 / Export view is not visible at the requested experiment time.");
    const auto nativeError = nativeSceneError();
    if (!nativeError.isEmpty()) return nativeError;
    QVariantList metrics;
    if (nativeCapture_) metrics.append(nativeMetrics_);
    for (const auto &[item, previous] : captureViews_) {
        auto *view = qobject_cast<CloudView *>(item.data());
        if (view && !view->isVisible()) continue;
        if (view) {
            const auto cameraError = view->exportValidationError(begin_ + double(index_) * speed_ / fps_);
            if (!cameraError.isEmpty()) return cameraError;
        }
        if (!view || view->framebufferSize() != view->captureSize())
            return tr("点云离屏缓冲区未达到输出分辨率，已停止导出。 / Offscreen resolution mismatch.");
        metrics.append(QVariantMap{{"view", view->property("exportViewId")},
            {"width", view->framebufferSize().width()}, {"height", view->framebufferSize().height()},
            {"texture_limit", view->textureLimit()}});
    }
    captureMetrics_ = std::move(metrics);
    return {};
}

void MovieExporter::saveStill(const QImage &image)
{
    if (image.isNull()) { finish(tr("离屏图像为空。")); return; }
    const auto captureError = validateCapture();
    if (!captureError.isEmpty()) { finish(captureError); return; }
    emit frameCaptured(image, begin_);
    const auto options = canvas_->property("captureOptions").toMap();
    const QColor background(options.value("background", "transparent").toString());
    const int width = width_, height = height_, dpi = dpi_;
    const auto destination = destination_;
    const auto time = begin_;
    stillWriter_.setFuture(QtConcurrent::run([image, background, width, height, dpi, destination, time] {
        QImage result(width, height, QImage::Format_ARGB32_Premultiplied);
        if (result.isNull()) return QStringLiteral("内存不足。 / Insufficient memory.");
        result.fill(background.isValid() ? background : QColor(Qt::transparent));
        QPainter painter(&result);
        painter.drawImage(QPoint((width - image.width()) / 2, (height - image.height()) / 2), image);
        painter.end();
        return replay::writeStillImage(result, destination, dpi, time);
    }));
}

void MovieExporter::cancel()
{
    if (!exporting_)
        return;
    if (stillWriter_.isRunning()) { message_ = tr("正在完成图像写入…"); emit changed(); return; }
    cancelled_ = true;
    if (!nativeJob_.isEmpty()) {
        const auto job = std::exchange(nativeJob_, {}); nativeDeadline_.stop();
        if (nativeClient_) nativeClient_->cancelOutputFrame(job);
    }
    if (audioProbe_.isRunning()) { message_ = tr("正在取消音轨检查… / Cancelling audio inspection…"); emit changed(); return; }
    if (watcher_.isRunning()) { message_ = tr("正在取消解码… / Cancelling decode…"); emit changed(); return; }
    if (process_.state() == QProcess::NotRunning)
        finish(tr("已取消视频导出。"));
    else
        process_.kill();
}
