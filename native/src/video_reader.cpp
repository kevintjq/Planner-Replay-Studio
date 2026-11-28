#include "video_reader.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/parseutils.h>
#include <libswscale/swscale.h>
}
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace replay {
namespace {
constexpr AVRational Nanoseconds{1, 1'000'000'000};
void checked(int code, const char *action)
{
    if (code >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, error, sizeof(error));
    throw std::runtime_error(std::string(action) + ": " + error);
}
}

struct VideoReader::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVFrame *decoded = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *scaler = nullptr;
    int stream = -1;
    AVRational base{};
    qint64 origin = AV_NOPTS_VALUE;
    qint64 previous = AV_NOPTS_VALUE;
    qint64 end = AV_NOPTS_VALUE;
    int ordinal = 0;
    int rotation = 0;
    bool draining = false;
    bool eof = false;
    std::map<qint64, int> ordinals;
    std::function<bool()> lifetimeCancelled, requestCancelled;
    bool isCancelled() const
    {
        return (lifetimeCancelled && lifetimeCancelled()) || (requestCancelled && requestCancelled());
    }
    void checkCancellation() const
    {
        if (isCancelled()) throw std::runtime_error("Video decoding cancelled.");
    }
    ~Impl()
    {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&decoded);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    bool seek(qint64 target)
    {
        if (isCancelled()) return false;
        if (avformat_seek_file(format, stream, std::numeric_limits<qint64>::min(), target, target, 0) < 0) return false;
        avcodec_flush_buffers(codec);
        av_packet_unref(packet);
        av_frame_unref(decoded);
        draining = eof = false;
        previous = AV_NOPTS_VALUE;
        ordinal = target <= origin ? 0 : -1;
        return true;
    }
};

VideoReader::VideoReader(const QString &file, const std::function<bool()> &cancel) : impl_(std::make_unique<Impl>())
{
    auto &s = *impl_;
    s.format = avformat_alloc_context();
    if (!s.format) throw std::bad_alloc();
    s.lifetimeCancelled = cancel;
    s.format->interrupt_callback = {[](void *opaque) {
        const auto *state = static_cast<Impl *>(opaque);
        return state->isCancelled() ? 1 : 0;
    }, &s};
    s.checkCancellation();
    checked(avformat_open_input(&s.format, file.toUtf8().constData(), nullptr, nullptr), "Open source video");
    s.checkCancellation();
    checked(avformat_find_stream_info(s.format, nullptr), "Read source video metadata");
    s.checkCancellation();
    const AVCodec *decoder = nullptr;
    s.stream = av_find_best_stream(s.format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    checked(s.stream, "Find source video stream");
    auto *stream = s.format->streams[s.stream];
    s.base = stream->time_base;
    if (s.base.num <= 0 || s.base.den <= 0) throw std::runtime_error("Video has no valid timestamp time_base.");
    s.codec = avcodec_alloc_context3(decoder);
    if (!s.codec) throw std::bad_alloc();
    checked(avcodec_parameters_to_context(s.codec, stream->codecpar), "Read video codec parameters");
    s.codec->pkt_timebase = s.base;
    s.codec->thread_count = 4;
    checked(avcodec_open2(s.codec, decoder, nullptr), "Open video decoder");
    s.decoded = av_frame_alloc(); s.packet = av_packet_alloc();
    if (!s.decoded || !s.packet) throw std::bad_alloc();
    const auto *matrix = av_packet_side_data_get(stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    size_t matrixSize = matrix ? matrix->size : 0;
    const auto *matrixData = matrix ? matrix->data : av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &matrixSize);
    if (matrixData && matrixSize >= 9 * sizeof(int32_t)) {
        const double rotation = -av_display_rotation_get(reinterpret_cast<const int32_t *>(matrixData));
        if (std::isfinite(rotation)) s.rotation = (int(std::lround(rotation)) % 360 + 360) % 360;
    }
    next_ = decode();
    s.checkCancellation();
    if (!next_) throw std::runtime_error("Video contains no decodable frames.");
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        const auto start = stream->start_time == AV_NOPTS_VALUE ? s.origin : stream->start_time;
        s.end = start + stream->duration;
    } else if (const auto *tag = av_dict_get(stream->metadata, "DURATION", nullptr, 0)) {
        int64_t endpoint = 0;
        // Matroska's track DURATION tag is an absolute presentation endpoint.
        if (av_parse_time(&endpoint, tag->value, 1) >= 0)
            s.end = av_rescale_q(endpoint, AV_TIME_BASE_Q, s.base);
    } else if (s.format->duration != AV_NOPTS_VALUE && s.format->duration > 0) {
        const auto start = s.format->start_time == AV_NOPTS_VALUE ? s.origin : av_rescale_q(s.format->start_time, AV_TIME_BASE_Q, s.base);
        s.end = start + av_rescale_q(s.format->duration, AV_TIME_BASE_Q, s.base);
    }
}
VideoReader::~VideoReader() = default;

std::optional<VideoReader::Frame> VideoReader::decode()
{
    auto &s = *impl_;
    while (!s.eof) {
        if (s.isCancelled()) return {};
        const auto code = avcodec_receive_frame(s.codec, s.decoded);
        if (code == AVERROR_EOF) { s.eof = true; return {}; }
        if (code == AVERROR(EAGAIN)) {
            if (s.draining) throw std::runtime_error("Video decoder stalled while draining.");
            int read = 0;
            do {
                av_packet_unref(s.packet);
                read = av_read_frame(s.format, s.packet);
            } while (read >= 0 && s.packet->stream_index != s.stream && !s.isCancelled());
            if (s.isCancelled()) return {};
            if (read >= 0 && s.packet->stream_index != s.stream) return {};
            if (read == AVERROR_EOF) {
                checked(avcodec_send_packet(s.codec, nullptr), "Drain video decoder");
                s.draining = true;
            } else {
                checked(read, "Read video packet");
                checked(avcodec_send_packet(s.codec, s.packet), "Decode video packet");
                av_packet_unref(s.packet);
            }
            continue;
        }
        checked(code, "Receive decoded video frame");
        const auto *decoded = s.decoded;
        const bool sourcePts = decoded->pts != AV_NOPTS_VALUE;
        const qint64 pts = sourcePts ? decoded->pts : decoded->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) throw std::runtime_error("Video frame has no PTS or recoverable timestamp; exact synchronization is unavailable.");
        if (s.previous != AV_NOPTS_VALUE && pts <= s.previous)
            throw std::runtime_error("Video presentation timestamps are duplicate or nonmonotonic; repair the source timestamps before synchronizing.");
        if (s.origin == AV_NOPTS_VALUE) s.origin = pts;
        s.previous = pts;
        if (const auto known = s.ordinals.find(pts); known != s.ordinals.end()) s.ordinal = known->second;
        const int ordinal = s.ordinal;
        if (s.ordinal >= 0) { s.ordinals[pts] = s.ordinal; ++s.ordinal; }
        auto aspect = av_guess_sample_aspect_ratio(s.format, s.format->streams[s.stream], s.decoded);
        if (aspect.num <= 0 || aspect.den <= 0) aspect = {1, 1};
        const auto displayWidth = av_rescale_rnd(decoded->width, aspect.num, aspect.den, AV_ROUND_NEAR_INF);
        if (decoded->width <= 0 || decoded->height <= 0 || displayWidth <= 0 || displayWidth > 32768 ||
            decoded->height > 32768 || displayWidth * decoded->height > 268'435'456)
            throw std::runtime_error("Source display dimensions exceed supported memory limits after sample-aspect conversion.");
        const QSize codedSize(decoded->width, decoded->height);
        cv::Mat image(decoded->height, int(displayWidth), CV_8UC3);
        s.scaler = sws_getCachedContext(s.scaler, decoded->width, decoded->height, AVPixelFormat(decoded->format),
            image.cols, image.rows, AV_PIX_FMT_BGR24, SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        if (!s.scaler) throw std::runtime_error("Cannot convert the decoded source video pixel format.");
        const int colorSpace = decoded->colorspace == AVCOL_SPC_UNSPECIFIED ? SWS_CS_DEFAULT : int(decoded->colorspace);
        const auto *coefficients = sws_getCoefficients(colorSpace);
        checked(sws_setColorspaceDetails(s.scaler, coefficients, decoded->color_range == AVCOL_RANGE_JPEG,
            coefficients, 1, 0, 1 << 16, 1 << 16), "Configure video color conversion");
        uint8_t *pixels[]{image.data, nullptr, nullptr, nullptr};
        int strides[]{int(image.step), 0, 0, 0};
        checked(sws_scale(s.scaler, decoded->data, decoded->linesize, 0, decoded->height, pixels, strides), "Convert video pixels");
        if (s.rotation == 90) cv::rotate(image, image, cv::ROTATE_90_CLOCKWISE);
        else if (s.rotation == 180) cv::rotate(image, image, cv::ROTATE_180);
        else if (s.rotation == 270) cv::rotate(image, image, cv::ROTATE_90_COUNTERCLOCKWISE);
        const qint64 duration = decoded->duration;
        av_frame_unref(s.decoded);
        return Frame{std::move(image), double(pts - s.origin) * av_q2d(s.base), ordinal, pts, duration, sourcePts,
            codedSize, aspect.num, aspect.den};
    }
    return {};
}
void VideoReader::use(Frame image)
{
    frame = std::move(image.image); seconds = image.seconds; index_ = image.index;
    pts_ = image.pts; frameDuration_ = image.duration; sourcePts_ = image.sourcePts;
    codedSize_ = image.codedSize;
    sampleAspectNumerator_ = image.sampleAspectNumerator; sampleAspectDenominator_ = image.sampleAspectDenominator;
}
bool VideoReader::advance()
{
    if (impl_->isCancelled()) return false;
    auto selected = next_ ? std::exchange(next_, std::nullopt) : decode();
    if (!selected) return false;
    use(std::move(*selected)); return true;
}
bool VideoReader::jump(int index)
{
    if (index < 0) return false;
    if (index_ < 0 || index < index_) {
        if (!impl_->seek(impl_->origin)) return false;
        next_.reset(); frame.release(); index_ = -1;
    }
    while (index_ < index) if (!advance()) return false;
    return true;
}
bool VideoReader::select(double time, const std::function<bool()> &cancel)
{
    if (!std::isfinite(time) || time < 0. || time >= double(std::numeric_limits<qint64>::max()) / 1.e9) return false;
    return selectNanoseconds(qint64(std::llround(time * 1.e9)), cancel);
}
bool VideoReader::selectNanoseconds(qint64 time, const std::function<bool()> &cancel)
{
    if (time < 0 || impl_->isCancelled() || (cancel && cancel())) return false;
    auto &s = *impl_;
    s.requestCancelled = cancel;
    struct Clear { Impl &s; ~Clear() { s.requestCancelled = {}; } } clear{s};
    // Seek in stream ticks, then decode in presentation order; never estimate using FPS.
    if ((!frame.empty() && time < timestampNs()) || (time > 3'000'000'000LL && (frame.empty() || time - timestampNs() > 3'000'000'000LL))) {
        const auto relative = av_rescale_q_rnd(time, Nanoseconds, s.base, AV_ROUND_DOWN);
        if (!s.seek(s.origin + relative) && !s.seek(s.origin)) return false;
        next_.reset(); frame.release(); index_ = -1;
    }
    if (frame.empty() && !advance()) return false;
    if (timestampNs() > time) {
        if (!s.seek(s.origin)) return false;
        next_.reset(); frame.release(); index_ = -1;
        if (!advance() || timestampNs() > time) return false;
    }
    while (!s.isCancelled()) {
        if (!next_) next_ = decode();
        if (!next_) {
            if (s.isCancelled()) return false;
            const auto end = frameDuration_ > 0 ? pts_ + frameDuration_ : s.end;
            return time == timestampNs() || (end != AV_NOPTS_VALUE && time < av_rescale_q(end - s.origin, s.base, Nanoseconds));
        }
        if (av_rescale_q(next_->pts - s.origin, s.base, Nanoseconds) > time) break;
        advance();
    }
    return !s.isCancelled();
}
bool VideoReader::hasAudio() const
{
    for (unsigned i = 0; i < impl_->format->nb_streams; ++i)
        if (impl_->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) return true;
    return false;
}
int VideoReader::displayRotation() const { return impl_->rotation; }
qint64 VideoReader::originPts() const { return impl_->origin; }
int VideoReader::timeBaseNumerator() const { return impl_->base.num; }
int VideoReader::timeBaseDenominator() const { return impl_->base.den; }
qint64 VideoReader::timestampNs() const { return av_rescale_q(pts_ - impl_->origin, impl_->base, Nanoseconds); }
double VideoReader::duration() const
{
    const auto &s = *impl_;
    return s.end == AV_NOPTS_VALUE ? 0. : double(s.end - s.origin) * av_q2d(s.base);
}
QImage imageOf(const cv::Mat &mat)
{
    return QImage(mat.data, mat.cols, mat.rows, qsizetype(mat.step), QImage::Format_BGR888).copy();
}
}
