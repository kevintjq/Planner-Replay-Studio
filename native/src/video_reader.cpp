#include "video_reader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace replay {
namespace {
constexpr double Tolerance = 1e-8;
}

VideoReader::VideoReader(const QString &file)
    : video_(file.toUtf8().toStdString(), cv::CAP_ANY)
{
    if (!video_.isOpened())
        throw std::runtime_error("Unable to open video.");
    video_.set(cv::CAP_PROP_ORIENTATION_AUTO, 1.);
    const double rate = video_.get(cv::CAP_PROP_FPS);
    if (std::isfinite(rate) && rate > 0.)
        fps_ = rate;
}

std::optional<VideoReader::Frame> VideoReader::decode()
{
    cv::Mat image;
    if (exhausted_ || !video_.read(image) || image.empty()) {
        exhausted_ = true;
        return {};
    }
    const double decoded = video_.get(cv::CAP_PROP_POS_MSEC) / 1000.;
    const double index = video_.get(cv::CAP_PROP_POS_FRAMES) - 1.;
    const int frameIndex = std::isfinite(index) && index >= 0. && index < std::numeric_limits<int>::max()
        ? int(index) : index_ + 1;
    const double time = std::isfinite(decoded) && (decoded > 0. || frameIndex == 0)
        ? decoded : frameIndex / fps_;
    return Frame{std::move(image), time, frameIndex};
}

void VideoReader::use(Frame image)
{
    frame = std::move(image.image);
    seconds = image.seconds;
    index_ = image.index;
}

bool VideoReader::advance()
{
    auto selected = next_ ? std::exchange(next_, std::nullopt) : decode();
    if (!selected)
        return false;
    use(std::move(*selected));
    return true;
}

bool VideoReader::jump(int index)
{
    next_.reset();
    exhausted_ = false;
    frame.release();
    return video_.set(cv::CAP_PROP_POS_FRAMES, index) && advance();
}

bool VideoReader::select(double time, const std::function<bool()> &cancel)
{
    if (!std::isfinite(time) || time < 0.)
        return false;
    if (frame.empty() || time < seconds - Tolerance || time > seconds + 3.) {
        const double suggested = std::clamp((time - 2.) * fps_, 0., double(std::numeric_limits<int>::max() - 1));
        if ((!jump(int(suggested)) || seconds > time + Tolerance) && !jump(0))
            return false;
    }
    while (seconds < time - Tolerance && (!cancel || !cancel())) {
        if (!next_)
            next_ = decode();
        if (!next_)
            return time < seconds + 1. / fps_ - Tolerance;
        if (next_->seconds > time + Tolerance)
            break;
        advance();
    }
    return !cancel || !cancel();
}

double VideoReader::duration() const
{
    return video_.get(cv::CAP_PROP_FRAME_COUNT) / fps_;
}

QImage imageOf(const cv::Mat &mat)
{
    return QImage(mat.data, mat.cols, mat.rows, qsizetype(mat.step), QImage::Format_BGR888).copy();
}

}
