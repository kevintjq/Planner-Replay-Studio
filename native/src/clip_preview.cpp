#include "clip_preview.h"
#include "video_reader.h"
#include <QPainter>
#include <QtConcurrentRun>
#include <atomic>
#include <cmath>

struct ClipPreview::Decoder {
    QUrl source;
    std::unique_ptr<replay::VideoReader> reader;
    // A reader owns its predicate. Capture only this flag, never its owning Decoder.
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
};
ClipPreview::ClipPreview(QQuickItem *parent) : QQuickPaintedItem(parent), decoder_(std::make_shared<Decoder>())
{
    setAntialiasing(true);
    timer_.setSingleShot(true); timer_.setInterval(0);
    connect(&timer_, &QTimer::timeout, this, &ClipPreview::decode);
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        const auto result = watcher_.result();
        const bool current = result.revision == revision_;
        const bool continuous = !precise_ && result.source == source_ && requested_ >= result.requested && requested_ - result.requested < .25;
        if (enabled_ && (current || continuous)) {
            const bool durationChanged = duration_ != result.duration;
            image_ = result.image; duration_ = result.duration; actual_ = result.actual;
            pts_ = result.pts; error_ = result.error;
            if (durationChanged) emit this->durationChanged();
            update();
        }
        if (!current && enabled_) timer_.start();
        emit frameChanged();
    });
}
ClipPreview::~ClipPreview()
{
    *decoder_->cancelled = true;
    // Worker owns only shared Decoder state; QObject signal connections are
    // disconnected on destruction. AVIO interrupt releases it asynchronously.
}
void ClipPreview::setSource(const QUrl &source)
{
    if (source == source_) return;
    *decoder_->cancelled = true;
    const bool hadDuration = duration_ != 0.;
    source_ = source; image_ = {}; duration_ = actual_ = 0.; pts_.clear(); error_.clear();
    if (hadDuration) emit durationChanged();
    emit sourceChanged(); emit frameChanged(); update(); request();
}
void ClipPreview::setSourceSeconds(double time)
{
    if (!std::isfinite(time) || requested_ == time) return;
    requested_ = time; request();
}
void ClipPreview::setDecodingEnabled(bool value)
{
    if (enabled_ == value) return;
    enabled_ = value; request();
    if (!value) { *decoder_->cancelled = true; timer_.stop(); }
}
void ClipPreview::setPrecise(bool value)
{
    if (precise_ == value) return;
    precise_ = value; request();
}
void ClipPreview::request()
{
    ++revision_; emit requestChanged();
    if (enabled_ && source_.isLocalFile() && requested_ >= 0. && !watcher_.isRunning()) timer_.start();
}
void ClipPreview::decode()
{
    if (!enabled_ || !source_.isLocalFile() || requested_ < 0. || watcher_.isRunning()) return;
    *decoder_->cancelled = false;
    watcher_.setFuture(QtConcurrent::run([state = decoder_, source = source_, time = requested_, revision = revision_] {
        Result result; result.source = source; result.requested = time; result.revision = revision;
        const auto cancelled = [flag = state->cancelled] { return flag->load(); };
        try {
            if (state->source != source || !state->reader) {
                state->reader.reset();
                state->reader = std::make_unique<replay::VideoReader>(source.toLocalFile(), cancelled);
                state->source = source;
            }
            result.duration = state->reader->duration();
            if (state->reader->select(time, cancelled)) {
                result.image = replay::imageOf(state->reader->frame);
                result.actual = state->reader->seconds;
                result.pts = QString::number(state->reader->pts());
                if (!state->reader->hasSourcePts()) result.error = QStringLiteral("视频缺少原始 PTS，当前使用解码器恢复时间戳。 / Recovered timestamp; original PTS unavailable.");
            }
        } catch (const std::exception &error) { result.error = QString::fromUtf8(error.what()); }
        // An interrupted AVIO context can retain AVERROR_EXIT. Reopen on resume.
        if (cancelled()) { state->reader.reset(); result.image = {}; result.error.clear(); }
        return result;
    }));
    emit frameChanged();
}
void ClipPreview::paint(QPainter *painter)
{
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    if (!image_.isNull()) painter->drawImage(boundingRect(), image_);
}
