#include "trajectory_image.h"
#include "video_reader.h"
#include "canvas_layer_state.h"

#include <QFile>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPen>
#include <QTemporaryDir>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <opencv2/tracking.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <array>
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

void cacheImage(FixedCameraTrajectory &result, TrajectoryFrame &frame, QImage image, qsizetype &bytes, int index, bool source = false)
{
    auto &cached = source ? frame.cachedSource : frame.cachedSprite;
    auto &memory = source ? frame.sourceCrop : frame.sprite;
    if (image.isNull()) {
        if (!cached.isEmpty()) QFile::remove(cached);
        cached.clear(); memory = {};
        return;
    }
    if (cached.isEmpty() && bytes + image.sizeInBytes() <= InMemoryLimit) {
        bytes += image.sizeInBytes();
        memory = std::move(image);
        return;
    }
    if (!result.cache)
        result.cache = std::make_shared<QTemporaryDir>();
    if (!result.cache->isValid())
        throw std::runtime_error("Unable to allocate the trajectory cache.");
    if (cached.isEmpty()) cached = result.cache->filePath((source ? "source-" : "sprite-") + QString::number(index) + ".png");
    if (!image.save(cached, "PNG", 85))
        throw std::runtime_error("Unable to write a cached trajectory frame.");
    memory = {};
}

struct Reference {
    cv::Mat frame;
    cv::Rect box;
    size_t index;
};

struct BackgroundPatch {
    cv::Mat pixels;
    cv::Mat coverage;
    bool clear = false;
};

BackgroundPatch referencePatch(const std::vector<Reference> &references, const cv::Rect &bounds, size_t current,
                               const TrackingCancelled &cancel)
{
    BackgroundPatch result{cv::Mat::zeros(bounds.size(), CV_8UC3), cv::Mat::zeros(bounds.size(), CV_8U)};
    for (int y = 0; y < bounds.height; ++y) {
        if (cancel && cancel()) throw std::runtime_error("Tracking cancelled.");
        auto *pixels = result.pixels.ptr<cv::Vec3b>(y);
        auto *coverage = result.coverage.ptr<uchar>(y);
        for (int x = 0; x < bounds.width; ++x) {
            std::array<std::array<uchar, 32>, 3> values{};
            int count = 0;
            const cv::Point point(bounds.x + x, bounds.y + y);
            for (const auto &reference : references) {
                if (reference.index == current || reference.box.contains(point)) continue;
                const auto value = reference.frame.at<cv::Vec3b>(point);
                for (int c = 0; c < 3; ++c) values[c][count] = value[c];
                if (++count == 32) break;
            }
            if (!count) continue;
            for (int c = 0; c < 3; ++c) {
                auto middle = values[c].begin() + count / 2;
                std::nth_element(values[c].begin(), middle, values[c].begin() + count);
                pixels[x][c] = *middle;
            }
            coverage[x] = 255;
        }
    }
    result.clear = cv::countNonZero(result.coverage) == bounds.area();
    return result;
}

int median(std::vector<int> values)
{
    if (values.empty()) return 0;
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

void checkCancelled(const TrackingCancelled &cancel)
{
    if (cancel && cancel())
        throw std::runtime_error("Tracking cancelled.");
}

cv::Mat temporalBackground(const QString &file, double begin, double end, cv::Size size,
                           const TrackingCancelled &cancel, const TrackingProgress &progress)
{
    VideoReader reader(file, cancel);
    std::vector<Reference> references;
    constexpr qsizetype budget = 160 * 1024 * 1024;
    const int count = int(std::clamp<qsizetype>(budget / std::max<qsizetype>(1, qsizetype(size.area()) * 3), 3, 24));
    for (int i = 0; i < count; ++i) {
        checkCancelled(cancel);
        if (reader.select(begin + (end-begin) * i / (count-1), cancel))
            references.push_back({reader.frame.clone(), {}, size_t(-1)});
        if (progress) progress(.08 * (i+1) / count);
    }
    if (references.size() < 3) return {};
    return referencePatch(references, {0, 0, size.width, size.height}, size_t(-2), cancel).pixels;
}

struct ForegroundAppearance {
    std::array<int, 18> hues{};
    int peak = 0, peakBin = 0, pixels = 0;
    cv::Point2d offset;
    bool matches(const cv::Vec3b &hsv) const {
        if (!peak || hsv[1] < 55 || hsv[2] < 30) return false;
        const int bin = hsv[0]/10;
        const int distance = std::min(std::abs(bin-peakBin), 18-std::abs(bin-peakBin));
        return distance <= 1 && hues[bin] >= std::max(4, int(peak*.28));
    }
};

ForegroundAppearance foregroundAppearance(const ForegroundExtraction &extraction, const cv::Mat &source)
{
    ForegroundAppearance result;
    if (extraction.sprite.isNull() || !extraction.error.isEmpty()) return result;
    cv::Mat hsv; cv::cvtColor(source, hsv, cv::COLOR_BGR2HSV);
    for (int y = 0; y < hsv.rows; ++y) for (int x = 0; x < hsv.cols; ++x) {
        const auto color = hsv.at<cv::Vec3b>(y,x);
        if (extraction.sprite.constScanLine(y)[4*x+3] >= 128 && color[1] >= 75 && color[2] >= 40)
            ++result.hues[color[0]/10];
    }
    result.peak = *std::max_element(result.hues.cbegin(), result.hues.cend());
    result.peakBin = int(std::max_element(result.hues.cbegin(), result.hues.cend()) - result.hues.cbegin());
    if (result.peak < 8) { result.peak = 0; return result; }
    cv::Point2d total;
    for (int y = 0; y < hsv.rows; ++y) for (int x = 0; x < hsv.cols; ++x)
        if (extraction.sprite.constScanLine(y)[4*x+3] >= 128 && result.matches(hsv.at<cv::Vec3b>(y,x))) {
            total += cv::Point2d(x,y); ++result.pixels;
        }
    if (result.pixels < 12) { result.peak = 0; return result; }
    result.offset = total / result.pixels - cv::Point2d(source.cols*.5, source.rows*.5);
    return result;
}

std::optional<cv::Point2d> foregroundLocation(const cv::Mat &frame, const cv::Mat &background,
    cv::Rect box, double expectedPixels, const ForegroundAppearance &appearance)
{
    const auto search = (box + cv::Size(box.width*2, box.height*3) - cv::Point(box.width, box.height*3/2))
        & cv::Rect(0, 0, frame.cols, frame.rows);
    if (search.area() < 16) return {};
    const auto extracted = extractTrajectoryForeground(imageOf(frame(search)), imageOf(background(search)));
    if (!extracted.error.isEmpty()) return {};
    cv::Mat mask(search.height, search.width, CV_8U), hsv;
    if (appearance.peak) cv::cvtColor(frame(search), hsv, cv::COLOR_BGR2HSV);
    for (int y = 0; y < mask.rows; ++y) {
        const auto *row = extracted.sprite.constScanLine(y);
        auto *out = mask.ptr<uchar>(y);
        for (int x = 0; x < mask.cols; ++x) out[x] = row[4*x+3] >= 64 &&
            (!appearance.peak || appearance.matches(hsv.at<cv::Vec3b>(y,x))) ? 255 : 0;
    }
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_ELLIPSE, {5, 5}));
    cv::Mat labels, statistics, centroids;
    const int count = cv::connectedComponentsWithStats(mask, labels, statistics, centroids);
    double best = 0.; std::optional<cv::Point2d> location;
    const cv::Point2d previous(box.x + box.width*.5, box.y + box.height*.5);
    for (int label = 1; label < count; ++label) {
        const double area = statistics.at<int>(label, cv::CC_STAT_AREA);
        const cv::Point2d center(search.x + centroids.at<double>(label, 0), search.y + centroids.at<double>(label, 1));
        const double distance = cv::norm(center-previous);
        const double expected = appearance.peak ? appearance.pixels : expectedPixels;
        if (area < std::max(4., expected*.08) || area > expected*6. ||
            distance > std::max(box.width, box.height)*(appearance.peak ? .4 : .25)) continue;
        const double score = std::min(area, expectedPixels) / (1. + distance / std::max(1., box.width*.2));
        if (score > best) { best = score; location = center - (appearance.peak ? appearance.offset : cv::Point2d()); }
    }
    return location;
}
}

qint64 trajectoryAlphaPixels(const QImage &image)
{
    if (image.isNull()) return 0;
    const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    qint64 count = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const auto *row = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) count += row[4*x+3] >= 8;
    }
    return count;
}

QImage trajectorySourceCrop(const TrajectoryFrame &frame)
{
    return frame.cachedSource.isEmpty() ? frame.sourceCrop : QImage(frame.cachedSource);
}

QImage trajectorySprite(const TrajectoryFrame &frame, const TrajectoryFrameEdit &edit)
{
    auto image = cachedImage(frame).convertToFormat(QImage::Format_RGBA8888);
    if (edit.maskStrokes.empty()) return image;
    const auto source = trajectorySourceCrop(frame).convertToFormat(QImage::Format_RGBA8888);
    if (source.isNull() || source.size() != image.size())
        throw std::runtime_error("Original crop is missing; regenerate tracking before editing the mask.");
    QImage mask(image.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = image.constScanLine(y);
        auto *alpha = reinterpret_cast<QRgb *>(mask.scanLine(y));
        for (int x = 0; x < image.width(); ++x) alpha[x] = qRgb(row[4*x+3], row[4*x+3], row[4*x+3]);
    }
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing);
    for (const auto &stroke : edit.maskStrokes) {
        if (stroke.points.isEmpty()) continue;
        const double radius = std::max(.5, stroke.radius * std::min(mask.width(), mask.height()));
        painter.setPen(QPen(stroke.erase ? Qt::black : Qt::white, radius*2., Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path;
        const auto position = [&](const QPointF &point) { return QPointF(point.x() * (mask.width()-1), point.y() * (mask.height()-1)); };
        path.moveTo(position(stroke.points.front()));
        for (qsizetype i = 1; i < stroke.points.size(); ++i) path.lineTo(position(stroke.points[i]));
        // Press/release repeats the same point for a stationary click. Qt drops
        // zero-length path segments, so explicitly keep that precise brush dot.
        if (path.isEmpty()) painter.drawPoint(position(stroke.points.front()));
        else painter.drawPath(path);
    }
    painter.end();
    image = source;
    for (int y = 0; y < image.height(); ++y) {
        auto *row = image.scanLine(y);
        const auto *alpha = reinterpret_cast<const QRgb *>(mask.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) row[4*x+3] = qRed(alpha[x]);
    }
    return image;
}

QString TrajectoryCompositionStats::exportError(const TrajectoryEdits &edits) const
{
    if (edits.lineOnly) return lineSegments ? QString() : QStringLiteral("仅轨迹线模式没有可见线段，不能导出空结果。 / No visible line segments.");
    if (!composed) return QStringLiteral("没有可见无人机前景，禁止导出仅背景结果。请修正蒙版或背景；仅线图需显式选择“仅轨迹线”。 / No visible UAV foreground.");
    if (failed && !edits.skipInvalid) return QStringLiteral("%1 个采样帧前景无效；请修正蒙版、排除失败帧或显式启用“跳过无效帧”。 / Invalid foreground samples.").arg(failed);
    return {};
}

ForegroundExtraction extractTrajectoryForeground(const QImage &source, const QImage &background,
                                                 const QImage &backgroundCoverage, const QImage &targetSeeds)
{
    ForegroundExtraction result;
    if (!source.isNull()) {
        result.sprite = QImage(source.size(), QImage::Format_RGBA8888);
        result.sprite.fill(Qt::transparent);
    }
    const auto failed = [&](const char *reason) {
        result.error = QString::fromUtf8(reason);
        return result;
    };
    if (source.isNull() || background.isNull() || source.size() != background.size() ||
        source.width() < 4 || source.height() < 4)
        return failed("前景提取失败：缺少同尺寸背景参考。 / No matching background reference.");
    const auto image = source.convertToFormat(QImage::Format_RGB888);
    const auto reference = background.convertToFormat(QImage::Format_RGB888);
    const auto coverage = backgroundCoverage.isNull() ? QImage() : backgroundCoverage.convertToFormat(QImage::Format_Grayscale8);
    const auto seeds = targetSeeds.isNull() ? QImage() : targetSeeds.convertToFormat(QImage::Format_Grayscale8);
    if (!seeds.isNull() && seeds.size() != image.size())
        return failed("前景提取失败：目标种子蒙版尺寸不符。 / Target seed size mismatch.");
    if (!coverage.isNull() && coverage.size() != image.size())
        return failed("前景提取失败：背景覆盖蒙版尺寸不符。 / Background coverage size mismatch.");
    if (!coverage.isNull()) {
        int covered = 0;
        for (int y = 0; y < coverage.height(); ++y)
            for (int x = 0; x < coverage.width(); ++x) covered += coverage.constScanLine(y)[x] >= 128;
        if (covered < .98 * image.width() * image.height())
            return failed("前景提取失败：目标遮挡区域缺少真实背景，请扩大生成区间或选择背景参考帧。 / Insufficient unoccluded background.");
    }
    std::array<std::vector<int>, 3> borderDifferences;
    const int borderWidth = std::max(1, std::min(image.width(), image.height()) / 12);
    for (int y = 0; y < image.height(); ++y) {
        const auto *a = image.constScanLine(y), *b = reference.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            if (x >= borderWidth && y >= borderWidth && x < image.width() - borderWidth && y < image.height() - borderWidth) continue;
            if (!coverage.isNull() && coverage.constScanLine(y)[x] < 128) continue;
            for (int c = 0; c < 3; ++c) borderDifferences[c].push_back(int(a[3*x+c]) - b[3*x+c]);
        }
    }
    std::array<int, 3> exposure{};
    for (int c = 0; c < 3; ++c) {
        if (borderDifferences[c].size() < 8) return failed("前景提取失败：有效背景边界不足。 / Insufficient reference boundary.");
        exposure[c] = median(borderDifferences[c]);
        if (std::abs(exposure[c]) > 64)
            return failed("前景提取失败：光照变化过大，请更换背景参考。 / Excessive background exposure change.");
    }
    cv::Mat difference(image.height(), image.width(), CV_8U);
    std::vector<int> borderNoise;
    for (int y = 0; y < image.height(); ++y) {
        const auto *a = image.constScanLine(y), *b = reference.constScanLine(y);
        auto *row = difference.ptr<uchar>(y);
        for (int x = 0; x < image.width(); ++x) {
            int value = 0;
            for (int c = 0; c < 3; ++c) value = std::max(value, std::abs(int(a[3*x+c]) - b[3*x+c] - exposure[c]));
            row[x] = uchar(std::min(255, value));
            if (x < borderWidth || y < borderWidth || x >= image.width() - borderWidth || y >= image.height() - borderWidth)
                if (coverage.isNull() || coverage.constScanLine(y)[x] >= 128) borderNoise.push_back(value);
        }
    }
    const int noise = median(borderNoise);
    if (noise > 32)
        return failed("前景提取失败：背景不匹配或相机移动，请先配准背景。 / Background mismatch; register camera motion first.");
    for (auto &value : borderNoise) value = std::abs(value - noise);
    const int threshold = std::clamp(noise + 3 * median(borderNoise) + 8, 10, 56);
    cv::Mat mask;
    cv::threshold(difference, mask, threshold, 255, cv::THRESH_BINARY);
    if (!coverage.isNull()) {
        cv::Mat observed(coverage.height(), coverage.width(), CV_8U, const_cast<uchar *>(coverage.constBits()), coverage.bytesPerLine()), binary;
        cv::compare(observed, 128, binary, cv::CMP_GE);
        cv::bitwise_and(mask, binary, mask);
    }
    // Closing groups the observed fine frame parts; in seeded extraction it must
    // not manufacture alpha inside real openings or across background gaps.
    const cv::Mat observedMask = seeds.isNull() ? cv::Mat() : mask.clone();
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_ELLIPSE, {3, 3}));
    cv::Mat labels, statistics, centroids;
    const int count = cv::connectedComponentsWithStats(mask, labels, statistics, centroids);
    cv::Mat filtered = cv::Mat::zeros(mask.size(), CV_8U);
    const int minimumArea = std::max(3, int(mask.total() * .001));
    const cv::Rect interior(mask.cols / 5, mask.rows / 5, std::max(1, mask.cols * 3 / 5), std::max(1, mask.rows * 3 / 5));
    std::vector<int> seedPixels(size_t(count), 0);
    if (!seeds.isNull()) {
        int supported = 0;
        for (int y = 0; y < mask.rows; ++y) {
            const auto *seed = seeds.constScanLine(y), *observed = observedMask.ptr<uchar>(y);
            const auto *label = labels.ptr<int>(y);
            for (int x = 0; x < mask.cols; ++x)
                if (seed[x] >= 128 && observed[x]) { ++seedPixels[size_t(label[x])]; ++supported; }
        }
        if (supported < 3)
            return failed("前景提取失败：当前帧缺少可靠目标外观种子。 / No reliable observed target seeds.");
    }
    for (int label = 1; label < count; ++label) {
        const cv::Rect bounds(statistics.at<int>(label, cv::CC_STAT_LEFT), statistics.at<int>(label, cv::CC_STAT_TOP),
                              statistics.at<int>(label, cv::CC_STAT_WIDTH), statistics.at<int>(label, cv::CC_STAT_HEIGHT));
        const bool belongsToTarget = seeds.isNull() ? (bounds & interior).area() > 0 : seedPixels[size_t(label)] >= 2;
        if (statistics.at<int>(label, cv::CC_STAT_AREA) >= minimumArea && belongsToTarget)
            filtered.setTo(255, labels == label);
    }
    if (!seeds.isNull()) cv::bitwise_and(filtered, observedMask, filtered);
    const int foregroundPixels = cv::countNonZero(filtered);
    const double fraction = double(foregroundPixels) / mask.total();
    if (foregroundPixels < std::max(6, minimumArea))
        return failed("前景提取失败：没有可靠目标差异，请检查目标或背景参考。 / No reliable foreground difference.");
    if (fraction > .92)
        return failed("前景提取失败：几乎整个区域被判为前景，请检查背景及目标框。 / Foreground occupies almost the complete ROI.");
    cv::GaussianBlur(filtered, filtered, {3, 3}, .6);
    if (!seeds.isNull()) filtered.setTo(0, observedMask == 0);
    result.sprite = source.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < result.sprite.height(); ++y) {
        auto *row = result.sprite.scanLine(y);
        const auto *alpha = filtered.ptr<uchar>(y);
        for (int x = 0; x < result.sprite.width(); ++x)
            row[4*x + 3] = coverage.isNull() || coverage.constScanLine(y)[x] >= 128 ? alpha[x] : 0;
    }
    result.foregroundFraction = fraction;
    return result;
}

QImage readTrajectoryFrame(const QString &file, double seconds)
{
    VideoReader reader(file);
    return reader.select(seconds) ? imageOf(reader.frame) : QImage();
}

FixedCameraTrajectory trackTrajectory(const QString &file, double start, std::optional<double> end,
                                      const QRectF &selection, const TrackingCancelled &cancel,
                                      const TrackingProgress &progress, const TrajectoryTrackingOptions &options)
{
    if (!std::isfinite(start) || start < 0. || (end && (!std::isfinite(*end) || *end <= start)) ||
        !selection.isValid() || !QRectF(0., 0., 1., 1.).contains(selection))
        throw std::runtime_error("Invalid tracking interval or selection.");
    VideoReader reader(file, cancel);
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
    cv::Mat temporal;
    if (options.backgroundFile.isEmpty() && !options.backgroundSeconds)
        temporal = temporalBackground(file, reader.seconds, end.value_or(reader.duration()), original, cancel, progress);
    else {
        const auto supplied = options.backgroundFile.isEmpty() ? readTrajectoryFrame(file, *options.backgroundSeconds) : QImage(options.backgroundFile);
        if (supplied.size() != QSize(original.width, original.height)) throw std::runtime_error("Background reference must have the same display dimensions as the video.");
        const auto bgr = supplied.convertToFormat(QImage::Format_BGR888);
        temporal = cv::Mat(bgr.height(), bgr.width(), CV_8UC3, const_cast<uchar *>(bgr.constBits()), bgr.bytesPerLine()).clone();
    }
    const auto initialFull = scaledRect(box, trackingSize, original);
    ForegroundExtraction initialMask;
    if (!temporal.empty()) initialMask = extractTrajectoryForeground(imageOf(reader.frame(initialFull)), imageOf(temporal(initialFull)));
    const auto appearance = foregroundAppearance(initialMask, reader.frame(initialFull));
    if (initialMask.error.isEmpty() && !initialMask.sprite.isNull()) {
        cv::Mat alpha(initialMask.sprite.height(), initialMask.sprite.width(), CV_8U);
        for (int y = 0; y < alpha.rows; ++y) for (int x = 0; x < alpha.cols; ++x)
            alpha.at<uchar>(y,x) = initialMask.sprite.constScanLine(y)[4*x+3];
        cv::resize(alpha, alpha, box.size(), 0., 0., cv::INTER_NEAREST);
        tracker->setInitialMask(alpha);
    }
    tracker->init(reduced, box);

    FixedCameraTrajectory result;
    result.background = imageOf(reader.frame).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const double total = std::max(.01, end.value_or(reader.duration()) - reader.seconds);
    const double first = reader.seconds;
    std::vector<cv::Rect> trackedBounds, cropBounds;
    std::vector<bool> targetObserved;
    bool observed = true;
    std::vector<cv::Point2d> translations;
    cv::Mat registrationReference, registrationWindow;
    const cv::Size registrationSize(std::min(640, original.width), std::max(1, original.height * std::min(640, original.width) / original.width));
    if (options.registerCamera) {
        cv::resize(reader.frame, registrationReference, registrationSize);
        cv::cvtColor(registrationReference, registrationReference, cv::COLOR_BGR2GRAY);
        registrationReference.convertTo(registrationReference, CV_32F);
        cv::createHanningWindow(registrationWindow, registrationSize, CV_32F);
    }
    cv::Point2d translation;
    const auto warp = [](cv::Mat &frame, const cv::Point2d &translation) {
        if (std::hypot(translation.x, translation.y) < .05) return;
        cv::Mat stabilized;
        cv::warpAffine(frame, stabilized, cv::Matx23d(1., 0., -translation.x, 0., 1., -translation.y), frame.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT101);
        frame = std::move(stabilized);
    };
    qsizetype bytes = 0;
    for (;;) {
        checkCancelled(cancel);
        const int padX = std::max(2, int(std::lround(box.width * .2)));
        const int padY = std::max(2, int(std::lround(box.height * .25)));
        const auto bounds = (box + cv::Size(2*padX, 2*padY) - cv::Point(padX, padY))
                            & cv::Rect(0, 0, trackingSize.width, trackingSize.height);
        const auto full = scaledRect(bounds, trackingSize, original);
        TrajectoryFrame sample{reader.seconds, QRect(full.x, full.y, full.width, full.height), {}, {}};
        cacheImage(result, sample, imageOf(reader.frame(full)), bytes, int(result.frames.size()), true);
        result.frames.push_back(std::move(sample));
        trackedBounds.push_back(scaledRect(box, trackingSize, original));
        cropBounds.push_back(full);
        targetObserved.push_back(observed);
        translations.push_back(translation);
        if (progress && result.frames.size() % 10 == 0)
            progress(.1 + .55 * std::clamp((reader.seconds - first) / total, 0., 1.));
        if (!reader.advance() || (end && reader.seconds > *end + 1e-8))
            break;
        if (options.registerCamera) {
            cv::Mat current;
            cv::resize(reader.frame, current, registrationSize);
            cv::cvtColor(current, current, cv::COLOR_BGR2GRAY); current.convertTo(current, CV_32F);
            double response = 0.;
            const auto shift = cv::phaseCorrelate(registrationReference, current, registrationWindow, &response);
            if (response >= .25 && std::isfinite(shift.x) && std::isfinite(shift.y) &&
                std::abs(shift.x) < registrationSize.width * .025 && std::abs(shift.y) < registrationSize.height * .025) {
                translation = {shift.x * original.width / registrationSize.width, shift.y * original.height / registrationSize.height};
                warp(reader.frame, translation); ++result.registeredFrames;
            } else { translation = {}; ++result.registrationRejected; }
        }
        cv::resize(reader.frame, reduced, trackingSize);
        bool foregroundTracked = false;
        if (!temporal.empty() && initialMask.error.isEmpty() && initialMask.foregroundFraction > 0.) {
            const auto previous = scaledRect(box, trackingSize, original);
            if (const auto center = foregroundLocation(reader.frame, temporal, previous, initialMask.foregroundFraction * initialFull.area(), appearance)) {
                box.x = int(std::lround(center->x * scale - box.width*.5));
                box.y = int(std::lround(center->y * scale - box.height*.5));
                foregroundTracked = true;
            }
        }
        observed = !appearance.peak || foregroundTracked;
        if ((!foregroundTracked && !appearance.peak && !tracker->update(reduced, box)) || box.width <= 0 || box.height <= 0 ||
            !(box & cv::Rect(0, 0, trackingSize.width, trackingSize.height)).area()) {
            result.lostAt = reader.seconds;
            break;
        }
    }

    if (result.frames.size() < 2)
        throw std::runtime_error("Tracking lost the target before a trajectory was established.");
    VideoReader referenceReader(file, cancel);
    std::vector<Reference> references;
    // Keep native-resolution references; downscaling a textured background before
    // subtraction creates false foreground around every stationary edge.
    constexpr qsizetype ReferenceBudget = 160 * 1024 * 1024;
    const auto frameBytes = qsizetype(original.width) * original.height * 3;
    const size_t count = std::min(result.frames.size(), size_t(std::clamp<qsizetype>(ReferenceBudget / std::max<qsizetype>(1, frameBytes), 2, 24)));
    if (!options.backgroundFile.isEmpty() || options.backgroundSeconds) {
        QImage supplied;
        if (!options.backgroundFile.isEmpty()) supplied = QImage(options.backgroundFile);
        else supplied = readTrajectoryFrame(file, *options.backgroundSeconds);
        if (supplied.size() != QSize(original.width, original.height))
            throw std::runtime_error("Background reference must have the same display dimensions as the video.");
        const auto bgr = supplied.convertToFormat(QImage::Format_BGR888);
        references.push_back({cv::Mat(bgr.height(), bgr.width(), CV_8UC3, const_cast<uchar *>(bgr.constBits()), bgr.bytesPerLine()).clone(), {}, size_t(-1)});
        result.background = supplied.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    for (size_t i = 0; temporal.empty() && i < count; ++i) {
        checkCancelled(cancel);
        const size_t index = size_t(std::lround(double(i) * double(result.frames.size() - 1) / double(count - 1)));
        if (referenceReader.select(result.frames[index].seconds, cancel)) {
            auto pixels = referenceReader.frame.clone();
            warp(pixels, translations[index]);
            Reference reference{std::move(pixels), trackedBounds[index], index};
            references.push_back(std::move(reference));
        }
    }

    const auto region = result.frames.front().bounds;
    auto plate = temporal.empty() ? referencePatch(references, cropBounds.front(), 0, cancel) :
        BackgroundPatch{temporal(cropBounds.front()).clone(), cv::Mat(cropBounds.front().size(), CV_8U, cv::Scalar(255)), false};
    if (!temporal.empty() && initialMask.error.isEmpty() && initialMask.foregroundFraction > 0.) {
        plate.pixels = temporal(cropBounds.front()).clone(); plate.coverage.setTo(255); plate.clear = true;
    }
    if (plate.clear) {
        QPainter painter(&result.background);
        painter.drawImage(region.topLeft(), imageOf(plate.pixels));
    } else {
        result.backgroundWarning = QStringLiteral("首帧目标区域缺少完整真实背景，底图使用修补近似；请检查并更换背景参考。 / Initial target background is inpainted, not observed; inspect or replace the reference.");
        const auto area = region.adjusted(-16, -16, 16, 16).intersected(result.background.rect());
        const auto rgb = result.background.copy(area).convertToFormat(QImage::Format_BGR888);
        cv::Mat pixels(rgb.height(), rgb.width(), CV_8UC3, const_cast<uchar *>(rgb.constBits()), rgb.bytesPerLine());
        cv::Mat mask = cv::Mat::zeros(pixels.size(), CV_8U), filled;
        const auto roi = trackedBounds.front();
        cv::rectangle(mask, cv::Rect(roi.x - area.x(), roi.y - area.y(), roi.width, roi.height), cv::Scalar(255), cv::FILLED);
        cv::inpaint(pixels, mask, filled, 3., cv::INPAINT_TELEA);
        QPainter painter(&result.background);
        painter.drawImage(area.topLeft(), imageOf(filled));
    }

    for (size_t i = 0; i < result.frames.size(); ++i) {
        checkCancelled(cancel);
        auto &sample = result.frames[i];
        const auto crop = trajectorySourceCrop(sample);
        if (crop.isNull())
            throw std::runtime_error("Unable to read a cached trajectory frame.");
        auto reference = temporal.empty() ? referencePatch(references, cropBounds[i], i, cancel) :
            BackgroundPatch{temporal(cropBounds[i]), cv::Mat(cropBounds[i].size(), CV_8U, cv::Scalar(255)), true};
        const QImage coverage(reference.coverage.data, reference.coverage.cols, reference.coverage.rows,
                              qsizetype(reference.coverage.step), QImage::Format_Grayscale8);
        QImage targetSeeds;
        if (appearance.peak) {
            targetSeeds = QImage(crop.size(), QImage::Format_Grayscale8);
            targetSeeds.fill(0);
            const auto bgr = crop.convertToFormat(QImage::Format_BGR888);
            cv::Mat pixels(bgr.height(), bgr.width(), CV_8UC3, const_cast<uchar *>(bgr.constBits()), bgr.bytesPerLine()), hsv;
            cv::cvtColor(pixels, hsv, cv::COLOR_BGR2HSV);
            const auto target = (trackedBounds[i] & cropBounds[i]) - cropBounds[i].tl();
            for (int y = target.y; y < target.br().y; ++y)
                for (int x = target.x; x < target.br().x; ++x)
                    if (appearance.matches(hsv.at<cv::Vec3b>(y, x))) targetSeeds.scanLine(y)[x] = 255;
        }
        auto extraction = extractTrajectoryForeground(crop, imageOf(reference.pixels), coverage, targetSeeds);
        if (!targetObserved[i]) {
            extraction.sprite.fill(Qt::transparent); extraction.foregroundFraction = 0.;
            extraction.error = QStringLiteral("目标被遮挡或外观定位失败；该帧未使用背景纹理冒充无人机。 / Target appearance not observed or occluded.");
        }
        sample.foregroundError = extraction.error;
        sample.foregroundFraction = extraction.foregroundFraction;
        sample.alphaPixels = trajectoryAlphaPixels(extraction.sprite);
        cacheImage(result, sample, std::move(extraction.sprite), bytes, int(i));
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
    std::erase_if(result, [&](size_t i) { return edits.frames.value(trajectoryTimestamp(frames[i].seconds)).removed; });
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

TrajectoryLayerData trajectoryLayers(const FixedCameraTrajectory &trajectory, const std::vector<size_t> &sampled,
    double opacity, const QSet<int> &excluded, const TrajectoryEdits &edits, const TrackingCancelled &cancel)
{
    TrajectoryLayerData layers; layers.canvasSize = trajectory.background.size(); layers.opacity = std::clamp(opacity, 0., 1.);
    auto &counts = layers.counts;
    counts.tracked = int(trajectory.frames.size()); counts.sampled = int(sampled.size());
    const TrajectoryExclusions exclusions(edits);
    {
        for (const auto &frame : trajectory.frames) {
            const auto stamp = trajectoryTimestamp(frame.seconds);
            if ((edits.trim && (stamp < edits.trim->first || stamp >= edits.trim->second)) || exclusions.contains(stamp,TrajectoryLine)) continue;
            if (!layers.start) layers.start = TrajectoryEndpoint{frame.bounds.center(),stamp};
            layers.goal = TrajectoryEndpoint{frame.bounds.center(),stamp};
        }
        // 轨迹使用全部跟踪位置，排除拖影不会改变轨迹。跳过跨越排除区间的边，避免错误直线桥接。
        bool connected = false;
        for (size_t i = 1; i < trajectory.frames.size(); ++i) {
            if (cancel && cancel())
                return {};
            const auto &a = trajectory.frames[i - 1], &b = trajectory.frames[i];
            const auto begin = trajectoryTimestamp(a.seconds), end = trajectoryTimestamp(b.seconds);
            if ((edits.trim && (begin < edits.trim->first || end >= edits.trim->second)) ||
                exclusions.intersects(begin, end, TrajectoryLine) || exclusions.contains(end, TrajectoryLine)) {
                connected = false;
                continue;
            }
            const auto from = a.bounds.center(), to = b.bounds.center();
            if (!connected) layers.linePath.moveTo(from);
            layers.linePath.lineTo(to); connected = true;
            if (from != to && (edits.showLine || edits.lineOnly) && edits.line.opacity > 1./255. && QColor(edits.line.color).alpha() > 0)
                ++counts.lineSegments;
        }
    }
    for (const size_t index : sampled) {
        if (cancel && cancel())
            return {};
        if (index >= trajectory.frames.size() || excluded.contains(int(index))) {
            ++counts.filtered;
            continue;
        }
        const auto &frame = trajectory.frames[index];
        const auto stamp = trajectoryTimestamp(frame.seconds);
        const auto edit = edits.frames.value(stamp);
        if (edit.removed || edit.excluded || exclusions.contains(stamp, DroneFrames) ||
            (edits.trim && (stamp < edits.trim->first || stamp >= edits.trim->second)) ||
            edits.lineOnly || std::clamp(opacity, 0., 1.) * edit.opacity < 1./255.) {
            ++counts.filtered;
            continue;
        }
        const auto sprite = trajectorySprite(frame, edit);
        if (sprite.isNull())
            throw std::runtime_error("Unable to read a cached trajectory frame. Regenerate the trajectory before exporting.");
        if (!trajectoryAlphaPixels(sprite)) { ++counts.failed; continue; }
        ++counts.successful;
        if (!frame.bounds.intersects(trajectory.background.rect())) { ++counts.filtered; continue; }
        layers.instances.push_back({index, stamp, frame.bounds, std::clamp(opacity, 0., 1.) * edit.opacity});
        ++counts.composed;
    }
    return layers;
}

QString trajectoryEndpointCaption(const TrajectoryEndpoint &point, const TrajectoryEndpointStyle &style)
{
    QString caption = style.showText ? style.text : QString();
    if (style.showTime) caption += (caption.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("%1 s").arg(double(point.timeNs)/1e9, 0, 'f', 3);
    return caption;
}
QRectF trajectoryEndpointBounds(const TrajectoryEndpoint &point, const TrajectoryEndpointStyle &style)
{
    const double radius = style.size*.5 + style.strokeWidth*.5 + 2.;
    QRectF bounds(point.center-QPointF(radius,radius), QSizeF(radius*2.,radius*2.));
    const auto caption = trajectoryEndpointCaption(point, style);
    if (!caption.isEmpty()) {
        QFont font("Arial"); font.setPixelSize(std::max(1, qRound(style.fontSize)));
        auto textBounds = QFontMetricsF(font).boundingRect(caption);
        textBounds.translate(point.center + QPointF(style.size*.5+5., style.fontSize*.35));
        bounds = bounds.united(textBounds.adjusted(-2.,-2.,2.,2.));
    }
    return bounds;
}
QVariantList trajectoryCanvasLayerRows(const FixedCameraTrajectory &trajectory, const TrajectoryLayerData &layers,
    const TrajectoryEdits &edits, double opacity, const QVariantMap &canvasState)
{
    QVariantList rows;
    if (layers.canvasSize.isEmpty()) return rows;
    const auto copies = canvasState.value("copies").toMap();
    const auto append = [&](const QString &requestedId) {
        const auto id = copies.value(requestedId, requestedId).toString();
        QRectF bounds(QPointF(), layers.canvasSize);
        QVariantMap row{{"id", requestedId}, {"sourceId", id}, {"opacity", 1.}, {"order", 0}};
        if (id == "background") { row["name"] = QStringLiteral("Background · 背景"); row["kind"] = "trajectory-background"; }
        else if (id.startsWith("sprite:")) {
            bool ok = false; const auto stamp = id.mid(7).toLongLong(&ok); if (!ok) return;
            const auto frame = std::lower_bound(trajectory.frames.cbegin(), trajectory.frames.cend(), stamp,
                [](const auto &frame, qint64 stamp) { return trajectoryTimestamp(frame.seconds) < stamp; });
            if (frame == trajectory.frames.cend() || trajectoryTimestamp(frame->seconds) != stamp) return;
            bounds = frame->bounds; row["frameIndex"] = int(frame-trajectory.frames.cbegin());
            row["name"] = QStringLiteral("UAV · %1 s").arg(frame->seconds, 0, 'f', 3); row["kind"] = "trajectory-sprite";
            row["timeNs"] = QString::number(stamp); row["seconds"] = frame->seconds;
            row["opacity"] = opacity*edits.frames.value(stamp).opacity; row["order"] = 200+int(frame-trajectory.frames.cbegin());
        } else if (id == "line") {
            row["name"] = QStringLiteral("Trajectory · 轨迹线"); row["kind"] = "trajectory-line"; row["order"] = 100;
        } else if (id == "start" || id == "goal") {
            const auto &point = id == "start" ? layers.start : layers.goal; if (!point) return;
            const auto &style = id == "start" ? edits.start : edits.goal;
            bounds = trajectoryEndpointBounds(*point, style).toAlignedRect();
            row["name"] = id == "start" ? QStringLiteral("Start · 起点") : QStringLiteral("Goal · 终点");
            row["kind"] = "trajectory-"+id; row["order"] = 100000;
            row["timeNs"] = QString::number(point->timeNs); row["seconds"] = double(point->timeNs)/1e9;
        } else return;
        row["x"] = bounds.x()/layers.canvasSize.width(); row["y"] = bounds.y()/layers.canvasSize.height();
        row["width"] = bounds.width()/layers.canvasSize.width(); row["height"] = bounds.height()/layers.canvasSize.height();
        row["sourceWidth"] = layers.canvasSize.width(); row["sourceHeight"] = layers.canvasSize.height();
        if (requestedId != id) row["name"] = row["name"].toString()+QStringLiteral(" Copy");
        rows.append(row);
    };
    if (!canvasState.value("background_removed").toBool()) append("background");
    if (edits.showLine || edits.lineOnly) append("line");
    if (!edits.lineOnly) for (const auto &instance : layers.instances)
        if (!edits.frames.value(instance.timeNs).removed) append("sprite:"+QString::number(instance.timeNs));
    if (edits.start.enabled) append("start"); if (edits.goal.enabled) append("goal");
    for (auto i = copies.cbegin(); i != copies.cend(); ++i) append(i.key());
    return rows;
}
void paintTrajectoryLayer(QPainter &painter, const FixedCameraTrajectory &trajectory, const TrajectoryLayerData &layers,
    const QString &layerId, const QRectF &target, const TrajectoryEdits &edits, const QVariantMap &canvasState,
    const TrackingCancelled &cancel)
{
    const auto id = canvasState.value("copies").toMap().value(layerId, layerId).toString();
    if (id == "background") { painter.drawImage(target, trajectory.background); return; }
    if (id.startsWith("sprite:")) {
        bool ok = false; const auto stamp = id.mid(7).toLongLong(&ok); if (!ok) return;
        const auto frame = std::lower_bound(trajectory.frames.cbegin(), trajectory.frames.cend(), stamp,
            [](const auto &frame, qint64 value) { return trajectoryTimestamp(frame.seconds) < value; });
        if (frame == trajectory.frames.cend() || trajectoryTimestamp(frame->seconds) != stamp) return;
        const auto image = trajectorySprite(*frame, edits.frames.value(stamp));
        if (image.isNull()) throw std::runtime_error("Unable to read a trajectory foreground cache.");
        painter.drawImage(target, image); return;
    }
    auto style = edits; int part = 0; QRectF bounds(QPointF(), layers.canvasSize);
    if (id == "line") { style.showLine = true; part = TrajectoryPath; }
    else if (id == "start" && layers.start) { style.start.enabled = true; part = TrajectoryStart; bounds = trajectoryEndpointBounds(*layers.start, style.start).toAlignedRect(); }
    else if (id == "goal" && layers.goal) { style.goal.enabled = true; part = TrajectoryGoal; bounds = trajectoryEndpointBounds(*layers.goal, style.goal).toAlignedRect(); }
    if (!part || bounds.isEmpty() || target.isEmpty()) return;
    painter.save(); painter.translate(target.topLeft()); painter.scale(target.width()/bounds.width(), target.height()/bounds.height());
    painter.translate(-bounds.topLeft());
    paintTrajectoryLayers(painter, trajectory, layers, QRectF(QPointF(), layers.canvasSize), style, cancel, part);
    painter.restore();
}
void paintTrajectoryLayers(QPainter &painter, const FixedCameraTrajectory &trajectory, const TrajectoryLayerData &layers,
    const QRectF &target, const TrajectoryEdits &edits, const TrackingCancelled &cancel, int parts,
    const QVariantMap &canvasState, double time, TrajectoryCompositionStats *paintedStats, const QStringList &selectedLayers)
{
    if (layers.canvasSize.isEmpty() || target.isEmpty()) return;
    if (!canvasState.isEmpty() || !selectedLayers.isEmpty()) {
        const auto canvasSize = QSize(canvasState.value("outputWidth", layers.canvasSize.width()).toInt(), canvasState.value("outputHeight", layers.canvasSize.height()).toInt());
        if (canvasSize.isEmpty()) return;
        time = time >= 0. ? time : trajectory.frames.empty() ? 0. : trajectory.frames.back().seconds;
        auto rows = trajectoryCanvasLayerRows(trajectory, layers, edits, layers.opacity, canvasState);
        rows.append(canvasState.value("elements").toList());
        std::stable_sort(rows.begin(), rows.end(), [&](const QVariant &a, const QVariant &b) {
            const auto x = a.toMap(), y = b.toMap();
            return canvasItemSettings(canvasState,x.value("id").toString(),time,x).value("z",x.value("order",600.)).toDouble() <
                canvasItemSettings(canvasState,y.value("id").toString(),time,y).value("z",y.value("order",600.)).toDouble();
        });
        auto counts = layers.counts; counts.composed = 0; counts.lineSegments = 0; counts.paintedLayers = 0;
        painter.save(); painter.setClipRect(target, Qt::IntersectClip); painter.translate(target.topLeft());
        painter.scale(target.width()/canvasSize.width(), target.height()/canvasSize.height());
        painter.setRenderHints(QPainter::Antialiasing|QPainter::SmoothPixmapTransform|QPainter::TextAntialiasing);
        for (const auto &value : rows) {
            if (cancel && cancel()) break;
            const auto row = value.toMap(); const auto id = row.value("id").toString(), sourceId = row.value("sourceId").toString();
            if (!selectedLayers.isEmpty() && !selectedLayers.contains(id)) continue;
            const bool scientific = canvasElementKinds().contains(row.value("kind").toString());
            const int part = sourceId == "background" ? TrajectoryBackground : sourceId == "line" ? TrajectoryPath : sourceId == "start" ? TrajectoryStart : sourceId == "goal" ? TrajectoryGoal : TrajectoryInstances;
            if ((!scientific && !(parts & part)) || !canvasItemActive(canvasState,id,time,true,row)) continue;
            const QRectF base(row.value("x").toDouble()*canvasSize.width(),row.value("y").toDouble()*canvasSize.height(),
                row.value("width").toDouble()*canvasSize.width(),row.value("height").toDouble()*canvasSize.height());
            const auto settings = canvasItemSettings(canvasState,id,time,row);
            const auto geometry = canvasItemGeometry(canvasState,id,base,canvasSize,time,row);
            if (!std::isfinite(geometry.x()) || !std::isfinite(geometry.y()) || !std::isfinite(geometry.width()) || !std::isfinite(geometry.height()) || geometry.isEmpty() ||
                settings.value("opacity",1.).toDouble() < 1./255.) continue;
            painter.save(); applyCanvasItemTransform(painter,settings,geometry);
            const QRectF local(QPointF(),geometry.size());
            if (painter.hasClipping() && !painter.clipPath().intersects(local)) { painter.restore(); continue; }
            paintCanvasItemAppearance(painter,settings,geometry.size());
            const auto backgroundMode = settings.value("backgroundMode").toString();
            bool painted = settings.value("backgroundOpacity",1.).toDouble()>0. &&
                (((backgroundMode=="solid" || backgroundMode=="translucent") && QColor(settings.value("backgroundColor","transparent").toString()).alpha()>0) ||
                 (settings.value("borderWidth").toDouble()>0. && QColor(settings.value("borderColor","transparent").toString()).alpha()>0));
            if (scientific) {
                QImage image;
                if (settings.value("kind") == "image") {
                    image = loadCanvasElementImage(settings.value("source").toString());
                    if (image.isNull()) { painter.restore(); throw std::runtime_error("Unable to read a scientific image layer."); }
                }
                painted = paintCanvasElement(painter,settings,geometry.size(),image) || painted;
            } else if (part == TrajectoryInstances) {
                const auto frameIndex = row.value("frameIndex",-1).toInt();
                if (frameIndex >= 0 && size_t(frameIndex) < trajectory.frames.size()) {
                    const auto &frame = trajectory.frames[size_t(frameIndex)];
                    const auto sprite = trajectorySprite(frame,edits.frames.value(trajectoryTimestamp(frame.seconds))).convertToFormat(QImage::Format_RGBA8888);
                    if (sprite.isNull()) { painter.restore(); throw std::runtime_error("Unable to read a trajectory foreground cache."); }
                    const auto clip = painter.clipPath(); bool visible = false;
                    for (int y = 0; y < sprite.height() && !visible; ++y) {
                        const auto *alpha = sprite.constScanLine(y);
                        for (int x = 0; x < sprite.width(); ++x) if (alpha[4*x+3] >= 8 && (!painter.hasClipping() || clip.contains(QPointF((x+.5)*local.width()/sprite.width(),(y+.5)*local.height()/sprite.height())))) { visible = true; break; }
                    }
                    if (visible) { painter.drawImage(local,sprite); ++counts.composed; painted = true; }
                    else ++counts.filtered;
                }
            } else {
                paintTrajectoryLayer(painter,trajectory,layers,id,local,edits,canvasState,cancel);
                if (part == TrajectoryBackground) painted = painted || !trajectory.background.isNull();
                if (part == TrajectoryStart || part == TrajectoryGoal) {
                    const auto &style = part == TrajectoryStart ? edits.start : edits.goal;
                    painted = painted || (style.opacity>1./255. && (QColor(style.fill).alpha()>0 ||
                        (style.strokeWidth>0. && QColor(style.stroke).alpha()>0) ||
                        ((style.showText || style.showTime) && QColor(style.textColor).alpha()>0)));
                }
            }
            if (!scientific && part == TrajectoryPath && edits.line.opacity*settings.value("opacity",1.).toDouble() > 1./255.) {
                QTransform transform; transform.scale(local.width()/layers.canvasSize.width(),local.height()/layers.canvasSize.height());
                QPainterPathStroker stroker; stroker.setWidth(edits.line.width*std::max(local.width()/layers.canvasSize.width(),local.height()/layers.canvasSize.height()));
                const auto stroke = stroker.createStroke(transform.map(layers.linePath));
                if (!painter.hasClipping() || stroke.intersects(painter.clipPath())) {
                    counts.lineSegments = layers.counts.lineSegments;
                    painted = painted || counts.lineSegments>0;
                }
            }
            if (painted) ++counts.paintedLayers;
            painter.restore();
        }
        painter.restore(); if (paintedStats) *paintedStats = counts;
        return;
    }
    if (paintedStats) *paintedStats = layers.counts;
    painter.save(); painter.setClipRect(target, Qt::IntersectClip);
    painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.translate(target.topLeft()); painter.scale(target.width()/layers.canvasSize.width(), target.height()/layers.canvasSize.height());
    if (parts & TrajectoryBackground) painter.drawImage(QPointF(), trajectory.background);
    if ((parts & TrajectoryPath) && (edits.showLine || edits.lineOnly)) {
        painter.save(); painter.setOpacity(painter.opacity()*edits.line.opacity);
        constexpr std::array<Qt::PenStyle, 4> patterns{Qt::SolidLine, Qt::DashLine, Qt::DotLine, Qt::DashDotLine};
        painter.setPen(QPen(QColor(edits.line.color), edits.line.width, patterns[size_t(std::clamp(edits.line.pattern, 0, 3))], Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush); painter.drawPath(layers.linePath); painter.restore();
    }
    if (parts & TrajectoryInstances) for (const auto &instance : layers.instances) {
        if (cancel && cancel()) { painter.restore(); return; }
        const auto &frame = trajectory.frames[instance.frameIndex];
        const auto sprite = trajectorySprite(frame, edits.frames.value(instance.timeNs));
        if (sprite.isNull()) { painter.restore(); throw std::runtime_error("Unable to read a trajectory foreground cache."); }
        painter.save(); painter.setOpacity(painter.opacity()*instance.opacity);
        painter.drawImage(instance.placement, sprite); painter.restore();
    }
    const auto endpoint = [&](const std::optional<TrajectoryEndpoint> &point, const TrajectoryEndpointStyle &style) {
        if (!point || !style.enabled || style.opacity <= 0.) return;
        painter.save(); painter.setOpacity(painter.opacity()*style.opacity);
        painter.setPen(style.strokeWidth > 0. ? QPen(QColor(style.stroke), style.strokeWidth) : QPen(Qt::NoPen));
        painter.setBrush(QColor(style.fill));
        painter.drawEllipse(point->center, style.size*.5, style.size*.5);
        const auto caption = trajectoryEndpointCaption(*point, style);
        if (!caption.isEmpty()) {
            QFont font("Arial"); font.setPixelSize(std::max(1, qRound(style.fontSize)));
            const auto position = point->center + QPointF(style.size*.5 + 5., style.fontSize*.35);
            const QColor textColor(style.textColor);
            QColor halo(qGray(textColor.rgb()) >= 128 ? Qt::black : Qt::white);
            halo.setAlpha(textColor.alpha());
            QPainterPath glyphs; glyphs.addText(position, font, caption);
            // Output styling follows the document text colour, never the app
            // theme. Keep the halo as vector outlines and the caption as text.
            painter.strokePath(glyphs, QPen(halo, std::clamp(style.fontSize*.14, 1.25, 3.), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setFont(font); painter.setPen(textColor);
            painter.drawText(position, caption);
        }
        painter.restore();
    };
    if (parts & TrajectoryStart) endpoint(layers.start, edits.start);
    if (parts & TrajectoryGoal) endpoint(layers.goal, edits.goal);
    painter.restore();
}

QImage renderTrajectory(const FixedCameraTrajectory &trajectory, const std::vector<size_t> &sampled,
                        double opacity, const QSet<int> &excluded, QSize limit,
                        const TrajectoryEdits &edits, const TrackingCancelled &cancel, bool exactOutputSize,
                        TrajectoryCompositionStats *stats, bool requireForeground, const QVariantMap &canvasState, double time)
{
    const auto layers = trajectoryLayers(trajectory, sampled, opacity, excluded, edits, cancel);
    if (trajectory.background.isNull() || (cancel && cancel())) return {};
    auto size = QSize(canvasState.value("outputWidth",layers.canvasSize.width()).toInt(),canvasState.value("outputHeight",layers.canvasSize.height()).toInt());
    if (limit.isValid() && (exactOutputSize || size.width() > limit.width() || size.height() > limit.height()))
        size.scale(limit, Qt::KeepAspectRatio);
    QImage result(size, QImage::Format_ARGB32_Premultiplied); result.fill(Qt::transparent);
    QPainter painter(&result);
    TrajectoryCompositionStats counts;
    paintTrajectoryLayers(painter, trajectory, layers, result.rect(), edits, cancel, TrajectoryAll, canvasState, time, &counts);
    painter.end();
    if (cancel && cancel()) return {};
    if (stats) *stats = counts;
    if (requireForeground) {
        const auto error = counts.exportError(edits);
        if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
    }
    return result;
}
}
