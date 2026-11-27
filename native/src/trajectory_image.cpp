#include "trajectory_image.h"
#include "video_reader.h"

#include <QFile>
#include <QPainter>
#include <QPen>
#include <QTemporaryDir>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <opencv2/tracking.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace replay {
namespace {
constexpr qsizetype InMemoryLimit = 96 * 1024 * 1024;

cv::Rect scaledRect(const cv::Rect &box, cv::Size from, cv::Size to)
{
    const auto left = std::lround(double(box.x) * to.width / from.width);
    const auto top = std::lround(double(box.y) * to.height / from.height);
    const auto right = std::lround(double(box.br().x) * to.width / from.width);
    const auto bottom = std::lround(double(box.br().y) * to.height / from.height);
    return cv::Rect(int(left), int(top), int(right - left), int(bottom - top))
           & cv::Rect(0, 0, to.width, to.height);
}

QImage cachedImage(const TrajectoryFrame &frame)
{
    return frame.cachedSprite.isEmpty() ? frame.sprite : QImage(frame.cachedSprite);
}

void cacheImage(FixedCameraTrajectory &result, TrajectoryFrame &frame, QImage image, qsizetype &bytes, int index)
{
    if (image.isNull()) {
        if (!frame.cachedSprite.isEmpty())
            QFile::remove(frame.cachedSprite);
        frame.cachedSprite.clear();
        frame.sprite = {};
        return;
    }
    if (frame.cachedSprite.isEmpty() && bytes + image.sizeInBytes() <= InMemoryLimit) {
        bytes += image.sizeInBytes();
        frame.sprite = std::move(image);
        return;
    }
    if (!result.cache)
        result.cache = std::make_shared<QTemporaryDir>();
    if (!result.cache->isValid())
        throw std::runtime_error("Unable to allocate the trajectory cache.");
    if (frame.cachedSprite.isEmpty())
        frame.cachedSprite = result.cache->filePath(QString::number(index) + ".png");
    if (!image.save(frame.cachedSprite, "PNG", 85))
        throw std::runtime_error("Unable to write a cached trajectory frame.");
    frame.sprite = {};
}

struct Reference {
    cv::Mat frame;
    cv::Rect box;
    size_t index;
};

cv::Mat referencePatch(const std::vector<Reference> &references, const cv::Rect &bounds, size_t current,
                       cv::Size outputSize, bool &clear)
{
    std::vector<const Reference *> choices;
    for (const auto &reference : references)
        if (reference.index != current)
            choices.push_back(&reference);
    if (choices.empty())
        return {};
    const auto overlap = [&](const Reference *r) { return (r->box & bounds).area(); };
    std::stable_sort(choices.begin(), choices.end(), [&](auto *a, auto *b) { return overlap(a) < overlap(b); });
    clear = overlap(choices.front()) == 0;
    const auto resized = [&](const Reference *r) {
        cv::Mat patch;
        cv::resize(r->frame(bounds), patch, outputSize);
        return patch;
    };
    auto result = resized(choices[0]);
    if (choices.size() >= 3 && overlap(choices[2]) == 0) {
        const auto second = resized(choices[1]);
        const auto third = resized(choices[2]);
        cv::Mat minPair, maxPair;
        cv::min(result, second, minPair);
        cv::max(result, second, maxPair);
        cv::min(maxPair, third, result);
        cv::max(result, minPair, result);
    }
    return result;
}

QImage makeSprite(const QImage &image, const cv::Mat &reference, bool first)
{
    QImage result = image.convertToFormat(QImage::Format_RGBA8888);
    if (reference.empty())
        return first ? result : QImage();

    const auto pixels = image.convertToFormat(QImage::Format_BGR888);
    cv::Mat bgr(pixels.height(), pixels.width(), CV_8UC3, const_cast<uchar *>(pixels.constBits()), pixels.bytesPerLine());
    cv::Mat difference;
    cv::absdiff(bgr, reference, difference);
    std::vector<cv::Mat> channels;
    cv::split(difference, channels);
    cv::max(channels[0], channels[1], channels[0]);
    cv::max(channels[0], channels[2], channels[0]);
    auto &value = channels[0];
    std::vector<int> border;
    for (int x = 0; x < value.cols; ++x) {
        border.push_back(value.at<uchar>(0, x));
        border.push_back(value.at<uchar>(value.rows - 1, x));
    }
    for (int y = 0; y < value.rows; ++y) {
        border.push_back(value.at<uchar>(y, 0));
        border.push_back(value.at<uchar>(y, value.cols - 1));
    }
    const auto median = border.begin() + qsizetype(border.size() / 2);
    std::nth_element(border.begin(), median, border.end());
    cv::threshold(value, value, std::clamp(*median + 13, 14, 40), 255, cv::THRESH_BINARY);
    cv::morphologyEx(value, value, cv::MORPH_CLOSE, cv::Mat::ones(3, 3, CV_8U));
    if (cv::countNonZero(value) < std::max(6., .035 * value.total()))
        return first ? result : QImage();
    cv::GaussianBlur(value, value, {3, 3}, 0.);
    for (int y = 0; y < result.height(); ++y) {
        auto *row = result.scanLine(y);
        const auto *alpha = value.ptr<uchar>(y);
        for (int x = 0; x < result.width(); ++x)
            row[4*x + 3] = alpha[x];
    }
    return result;
}

void checkCancelled(const TrackingCancelled &cancel)
{
    if (cancel && cancel())
        throw std::runtime_error("Tracking cancelled.");
}
}

QImage readTrajectoryFrame(const QString &file, double seconds)
{
    VideoReader reader(file);
    return reader.select(seconds) ? imageOf(reader.frame) : QImage();
}

FixedCameraTrajectory trackTrajectory(const QString &file, double start, std::optional<double> end,
                                      const QRectF &selection, const TrackingCancelled &cancel,
                                      const TrackingProgress &progress)
{
    if (!std::isfinite(start) || start < 0. || (end && (!std::isfinite(*end) || *end <= start)) ||
        !selection.isValid() || !QRectF(0., 0., 1., 1.).contains(selection))
        throw std::runtime_error("Invalid tracking interval or selection.");
    VideoReader reader(file);
    if (!reader.select(start, cancel)) {
        checkCancelled(cancel);
        throw std::runtime_error("No frame at the selected time.");
    }

    const auto original = reader.frame.size();
    const double target = std::min(selection.width() * original.width, selection.height() * original.height);
    const double scale = std::min(1., std::max(960. / std::max(original.width, original.height), 24. / std::max(1., target)));
    const cv::Size trackingSize(int(std::lround(original.width * scale)), int(std::lround(original.height * scale)));
    cv::Rect box(int(std::lround(selection.x() * trackingSize.width)), int(std::lround(selection.y() * trackingSize.height)),
                 int(std::lround(selection.width() * trackingSize.width)), int(std::lround(selection.height() * trackingSize.height)));
    box &= cv::Rect(0, 0, trackingSize.width, trackingSize.height);
    if (std::min(box.width, box.height) < 6)
        throw std::runtime_error("Selection is too small.");

    cv::TrackerCSRT::Params params;
    params.template_size = 160;
    params.number_of_scales = 17;
    params.admm_iterations = 3;
    auto tracker = cv::TrackerCSRT::create(params);
    cv::Mat reduced;
    cv::resize(reader.frame, reduced, trackingSize);
    tracker->init(reduced, box);

    FixedCameraTrajectory result;
    result.background = imageOf(reader.frame).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const double total = std::max(.01, end.value_or(reader.duration()) - reader.seconds);
    const double first = reader.seconds;
    std::vector<int> frameIndices;
    std::vector<cv::Rect> trackedBounds, cropBounds;
    qsizetype bytes = 0;
    for (;;) {
        checkCancelled(cancel);
        const int pad = std::max(1, int(std::lround(std::min(box.width, box.height) * .09)));
        const auto bounds = (box + cv::Size(2*pad, 2*pad) - cv::Point(pad, pad))
                            & cv::Rect(0, 0, trackingSize.width, trackingSize.height);
        const auto full = scaledRect(bounds, trackingSize, original);
        TrajectoryFrame sample{reader.seconds, QRect(full.x, full.y, full.width, full.height), {}, {}};
        cacheImage(result, sample, imageOf(reader.frame(full)), bytes, int(result.frames.size()));
        result.frames.push_back(std::move(sample));
        frameIndices.push_back(reader.index());
        trackedBounds.push_back(box);
        cropBounds.push_back(bounds);
        if (progress && result.frames.size() % 10 == 0)
            progress(.65 * std::clamp((reader.seconds - first) / total, 0., 1.));
        if (!reader.advance() || (end && reader.seconds > *end + 1e-8))
            break;
        cv::resize(reader.frame, reduced, trackingSize);
        if (!tracker->update(reduced, box) || box.width <= 0 || box.height <= 0 ||
            !(box & cv::Rect(0, 0, trackingSize.width, trackingSize.height)).area()) {
            result.lostAt = reader.seconds;
            break;
        }
    }

    if (result.frames.size() < 2)
        throw std::runtime_error("Tracking lost the target before a trajectory was established.");
    VideoReader referenceReader(file);
    std::vector<Reference> references;
    const size_t count = std::min(size_t(12), result.frames.size());
    for (size_t i = 0; i < count; ++i) {
        checkCancelled(cancel);
        const size_t index = size_t(std::lround(double(i) * double(result.frames.size() - 1) / double(count - 1)));
        if (referenceReader.jump(frameIndices[index])) {
            Reference reference{{}, trackedBounds[index], index};
            cv::resize(referenceReader.frame, reference.frame, trackingSize);
            references.push_back(std::move(reference));
        }
    }

    bool clear = false;
    const auto region = result.frames.front().bounds;
    const cv::Size regionSize(region.width(), region.height());
    const auto plate = referencePatch(references, cropBounds.front(), 0, regionSize, clear);
    if (clear) {
        QPainter painter(&result.background);
        painter.drawImage(region.topLeft(), imageOf(plate));
    } else {
        const auto area = region.adjusted(-16, -16, 16, 16).intersected(result.background.rect());
        const auto rgb = result.background.copy(area).convertToFormat(QImage::Format_BGR888);
        cv::Mat pixels(rgb.height(), rgb.width(), CV_8UC3, const_cast<uchar *>(rgb.constBits()), rgb.bytesPerLine());
        cv::Mat mask = cv::Mat::zeros(pixels.size(), CV_8U), filled;
        const auto roi = scaledRect(trackedBounds.front(), trackingSize, original);
        cv::rectangle(mask, cv::Rect(roi.x - area.x(), roi.y - area.y(), roi.width, roi.height), cv::Scalar(255), cv::FILLED);
        cv::inpaint(pixels, mask, filled, 3., cv::INPAINT_TELEA);
        QPainter painter(&result.background);
        painter.drawImage(area.topLeft(), imageOf(filled));
    }

    bytes = 0;
    for (size_t i = 0; i < result.frames.size(); ++i) {
        checkCancelled(cancel);
        auto &sample = result.frames[i];
        const auto crop = cachedImage(sample);
        if (crop.isNull())
            throw std::runtime_error("Unable to read a cached trajectory frame.");
        const auto reference = referencePatch(references, cropBounds[i], i, {crop.width(), crop.height()}, clear);
        cacheImage(result, sample, makeSprite(crop, reference, i == 0), bytes, int(i));
        if (progress && i % 10 == 0)
            progress(.65 + .35 * double(i) / double(result.frames.size()));
    }
    if (progress)
        progress(1.);
    return result;
}

std::vector<size_t> sampleTrajectory(const FixedCameraTrajectory &trajectory, double interval)
{
    if (trajectory.frames.empty() || !std::isfinite(interval) || interval < 0.)
        return {};
    std::vector<size_t> indices{0};
    if (interval > 1e-8) {
        const double first = trajectory.frames.front().seconds;
        double next = 1.;
        for (size_t i = 1; i < trajectory.frames.size(); ++i) {
            const double position = (trajectory.frames[i].seconds - first + 1e-8) / interval;
            if (position >= next) {
                indices.push_back(i);
                next = std::floor(position) + 1.;
            }
        }
        return indices;
    }
    std::vector<int> widths;
    for (const auto &frame : trajectory.frames)
        widths.push_back(std::max(frame.bounds.width(), frame.bounds.height()));
    const auto median = widths.begin() + qsizetype(widths.size() / 2);
    std::nth_element(widths.begin(), median, widths.end());
    const double spacing = std::max(6., .55 * *median);
    for (size_t i = 1; i < trajectory.frames.size(); ++i) {
        const auto center = trajectory.frames[i].bounds.center();
        if (std::all_of(indices.begin(), indices.end(), [&](size_t j) {
                const auto delta = center - trajectory.frames[j].bounds.center();
                return std::hypot(delta.x(), delta.y()) >= spacing;
            }))
            indices.push_back(i);
    }
    if (indices.size() <= 7)
        return indices;
    std::vector<size_t> limited;
    for (size_t i = 0; i < 7; ++i)
        limited.push_back(indices[size_t(std::lround(double(i) * double(indices.size() - 1) / 6.))]);
    return limited;
}

std::vector<size_t> editedTrajectorySamples(const FixedCameraTrajectory &trajectory, double interval,
                                          int mode, int count, const TrajectoryEdits &edits)
{
    std::vector<size_t> result;
    const auto &frames = trajectory.frames;
    if (frames.empty())
        return result;
    if (mode == 0 || mode == 3)
        result = sampleTrajectory(trajectory, mode == 3 ? 0. : interval);
    else if (mode == 1 && count > 0) {
        const double begin = edits.trim ? double(edits.trim->first) / 1e9 : frames.front().seconds;
        // trim 为左闭右开；最后一个采样仍选择范围内真实存在的源帧。
        const double end = edits.trim ? double(edits.trim->second - 1) / 1e9 : frames.back().seconds;
        for (int i = 0; i < count; ++i) {
            const double time = count == 1 ? begin : begin + (end - begin) * i / (count - 1);
            auto frame = std::lower_bound(frames.begin(), frames.end(), time,
                [](const auto &frame, double time) { return frame.seconds < time - 1e-8; });
            if (frame == frames.end() || (edits.trim && trajectoryTimestamp(frame->seconds) >= edits.trim->second)) {
                if (frame == frames.begin())
                    continue;
                --frame;
            }
            result.push_back(size_t(frame - frames.begin()));
        }
    }
    for (auto edit = edits.frames.cbegin(); edit != edits.frames.cend(); ++edit) {
        if (!edit->manual)
            continue;
        const auto frame = std::lower_bound(frames.begin(), frames.end(), edit.key(),
            [](const auto &frame, qint64 stamp) { return trajectoryTimestamp(frame.seconds) < stamp; });
        if (frame != frames.end() && trajectoryTimestamp(frame->seconds) == edit.key())
            result.push_back(size_t(frame - frames.begin()));
    }
    if (edits.trim)
        std::erase_if(result, [&](size_t i) {
            const auto stamp = trajectoryTimestamp(frames[i].seconds);
            return stamp < edits.trim->first || stamp >= edits.trim->second;
        });
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

QImage renderTrajectory(const FixedCameraTrajectory &trajectory, const std::vector<size_t> &sampled,
                        double opacity, const QSet<int> &excluded, QSize limit,
                        const TrajectoryEdits &edits, const TrackingCancelled &cancel, bool exactOutputSize)
{
    auto result = trajectory.background;
    if (result.isNull())
        return result;
    if (limit.isValid() && (exactOutputSize || result.width() > limit.width() || result.height() > limit.height()))
        result = result.scaled(limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const double xScale = double(result.width()) / trajectory.background.width();
    const double yScale = double(result.height()) / trajectory.background.height();
    const TrajectoryExclusions exclusions(edits);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (edits.showLine) {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#38a5eb"), std::max(1., 2. * xScale)));
        // 轨迹使用全部跟踪位置，排除拖影不会改变轨迹。跳过跨越排除区间的边，避免错误直线桥接。
        for (size_t i = 1; i < trajectory.frames.size(); ++i) {
            if (cancel && cancel())
                return {};
            const auto &a = trajectory.frames[i - 1], &b = trajectory.frames[i];
            const auto begin = trajectoryTimestamp(a.seconds), end = trajectoryTimestamp(b.seconds);
            if ((edits.trim && (begin < edits.trim->first || end >= edits.trim->second)) ||
                exclusions.intersects(begin, end, TrajectoryLine) || exclusions.contains(end, TrajectoryLine))
                continue;
            const auto from = a.bounds.center(), to = b.bounds.center();
            painter.drawLine(QPointF(from.x() * xScale, from.y() * yScale), QPointF(to.x() * xScale, to.y() * yScale));
        }
    }
    for (const size_t index : sampled) {
        if (cancel && cancel())
            return {};
        if (index >= trajectory.frames.size() || excluded.contains(int(index)))
            continue;
        const auto &frame = trajectory.frames[index];
        const auto stamp = trajectoryTimestamp(frame.seconds);
        const auto edit = edits.frames.value(stamp);
        if (edit.excluded || exclusions.contains(stamp, DroneFrames) ||
            (edits.trim && (stamp < edits.trim->first || stamp >= edits.trim->second)))
            continue;
        painter.setOpacity(std::clamp(opacity, 0., 1.) * edit.opacity);
        const QRectF area(frame.bounds.x() * xScale, frame.bounds.y() * yScale,
                          frame.bounds.width() * xScale, frame.bounds.height() * yScale);
        painter.drawImage(area, cachedImage(frame));
    }
    painter.end();
    return result;
}
}
