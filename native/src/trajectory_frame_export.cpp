#include "trajectory_frame_export.h"
#include "frame_export_plan.h"
#include "still_image_writer.h"
#include "video_reader.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace replay {
TrajectoryBatchResult exportTrajectoryBatch(const QString &source, const QString &parentDirectory,
    std::shared_ptr<const FixedCameraTrajectory> trajectory, const std::vector<size_t> &samples,
    const TrajectoryEdits &edits, double opacity, double currentTime, const QVariantMap &options,
    const TrackingCancelled &cancel, const TrackingProgress &progress)
{
    TrajectoryBatchResult result;
    QJsonArray completed;
    QJsonArray sampleTimes;
    if (trajectory) for (auto index : samples) if (index < trajectory->frames.size())
        sampleTimes.append(QString::number(trajectoryTimestamp(trajectory->frames[index].seconds)));
    QVector<FrameExportJob> jobs;
    const auto content = options.value("content", "source").toString();
    const auto cancelled = [&] { return cancel && cancel(); };
    const auto saveManifest = [&](const QString &status) {
        QSaveFile file(QDir(result.directory).filePath("manifest.json"));
        const QJsonObject document{{"status", status}, {"error", result.error}, {"content", content},
            {"time_basis", "source_video_seconds"}, {"source_file", QFileInfo(source).fileName()},
            {"original_preview_time", currentTime}, {"options", QJsonObject::fromVariantMap(options)},
            {"edits", edits.toJson()}, {"opacity", opacity}, {"completed", completed}, {"planned", int(jobs.size())},
            {"sample_timestamps_ns", sampleTimes},
            {"limitations", "Video frame times use the decoder timestamp, with frame-index/FPS fallback; source PTS certification pending. Sampling indices refer to the configured time grid. Trajectory snapshots preserve the current background, samples and edits, clipped at the requested time. Panels are raster; PDF/SVG figure captions are vector."}};
        const auto bytes = QJsonDocument(document).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save trajectory export manifest.");
    };
    try {
        if (!QDir(parentDirectory).exists()) throw std::runtime_error("Select an existing output directory.");
        if (content != "source" && content != "trajectory") throw std::runtime_error("Unknown trajectory export content.");
        if (options.value("allViews").toBool() || (!options.value("scope").toString().isEmpty() && options.value("scope").toString() != "canvas"))
            throw std::runtime_error("Trajectory batch export currently supports its complete canvas only.");
        const QSize size(options.value("width", 1920).toInt(), options.value("height", 1080).toInt());
        const int dpi = options.value("dpi", 300).toInt();
        const QColor background(options.value("background", "transparent").toString());
        if (size.width() < 16 || size.height() < 16 || size.width() > 8192 || size.height() > 8192 ||
            qint64(size.width()) * size.height() > 40'000'000 || dpi < 36 || dpi > 2400 || !background.isValid())
            throw std::runtime_error("Dimensions must be 16–8192, at most 40 megapixels; DPI 36–2400 and a valid background are required.");
        if (content == "trajectory" && (!trajectory || trajectory->frames.empty()))
            throw std::runtime_error("Generate a tracked trajectory before exporting accumulated snapshots.");
        std::unique_ptr<VideoReader> reader;
        if (content == "source") reader = std::make_unique<VideoReader>(source);
        auto planning = options;
        // 轨迹采样使用真实跟踪时间；独立采样网格使用序号 / 采样率。
        if (options.value("mode").toString() == "tracked") {
            if (!trajectory) throw std::runtime_error("No tracked samples are available.");
            QStringList times;
            for (auto index : samples) {
                if (index >= trajectory->frames.size()) throw std::runtime_error("Invalid trajectory sample.");
                times.append(QString::number(trajectory->frames[index].seconds, 'f', 9));
            }
            planning.insert("mode", "manual"); planning.insert("times", times.join(','));
        }
        const double duration = reader ? reader->duration() : trajectory->frames.back().seconds;
        jobs = planFrameExport(duration, currentTime, {}, {}, planning);
        if (content == "trajectory") for (const auto &job : jobs)
            if (trajectoryTimestamp(job.time) < trajectoryTimestamp(trajectory->frames.front().seconds))
                throw std::runtime_error("Export time is before the tracked trajectory begins.");
        const auto figureFormat = options.value("figureFormat", "pdf").toString().toLower();
        const int columns = options.value("columns", 2).toInt();
        if (options.value("figure").toBool()) {
            if (!QStringList{"png", "jpg", "tiff", "pdf", "svg"}.contains(figureFormat)) throw std::runtime_error("Invalid figure format.");
            for (const auto &job : jobs) if (job.fileName.compare("figure." + figureFormat, Qt::CaseInsensitive) == 0)
                throw std::runtime_error("Frame filename conflicts with figure output.");
            const auto error = validatePanelFigure(jobs.size(), size, columns);
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
        }
        if (cancelled()) { result.cancelled = true; result.error = "已取消。 / Cancelled."; return result; }
        result.directory = QDir(parentDirectory).filePath("trajectory-frames-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + '-' + QUuid::createUuid().toString(QUuid::Id128).left(8));
        if (!QDir().mkdir(result.directory)) { result.directory.clear(); throw std::runtime_error("Cannot create output directory."); }
        saveManifest("running");
        QStringList paths; QList<double> figureTimes;
        for (const auto &job : jobs) {
            if (cancelled()) break;
            QImage frame; double sourceTime = job.time; int sourceIndex = -1;
            if (reader) {
                if (!reader->select(job.time, cancel) || reader->frame.empty()) {
                    if (cancelled()) break;
                    throw std::runtime_error("No source video frame at the requested time.");
                }
                sourceTime = reader->seconds; sourceIndex = reader->index();
                frame = imageOf(reader->frame).scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            } else {
                const auto stamp = trajectoryTimestamp(job.time);
                auto clipped = edits;
                const auto end = stamp + 1; // 编辑使用 [入点,出点)，加 1 纳秒以包含请求端点。
                if (clipped.trim) clipped.trim->second = std::min(clipped.trim->second, end);
                else clipped.trim = {trajectoryTimestamp(trajectory->frames.front().seconds), end};
                // 空交集用空的半开区间表达，不改变原始编辑范围。
                if (clipped.trim->first > clipped.trim->second) clipped.trim->first = clipped.trim->second;
                frame = renderTrajectory(*trajectory, samples, opacity, {}, size, clipped, cancel, true);
                const auto latest = std::upper_bound(trajectory->frames.begin(), trajectory->frames.end(), stamp,
                    [](qint64 time, const auto &frame) { return time < trajectoryTimestamp(frame.seconds); });
                sourceTime = latest == trajectory->frames.begin() ? trajectory->frames.front().seconds : std::prev(latest)->seconds;
            }
            if (cancelled()) break;
            if (frame.isNull()) throw std::runtime_error("Empty source-quality frame.");
            QImage output(size, QImage::Format_ARGB32_Premultiplied);
            if (output.isNull()) throw std::runtime_error("Insufficient memory for output image.");
            output.fill(background);
            QPainter painter(&output);
            painter.drawImage(QPoint((size.width() - frame.width()) / 2, (size.height() - frame.height()) / 2), frame);
            painter.end();
            const auto path = QDir(result.directory).filePath(job.fileName);
            // 源帧标签使用实际解码时间；累积轨迹图使用裁切时间。
            const double outputTime = reader ? sourceTime : job.time;
            const auto error = writeStillImage(output, path, dpi, outputTime);
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
            paths.append(path); figureTimes.append(outputTime); ++result.completed;
            completed.append(QJsonObject{{"file", job.fileName}, {"requested_time", job.time}, {"output_time", outputTime},
                {"source_frame_time", sourceTime}, {"source_frame_index", sourceIndex}, {"width", size.width()}, {"height", size.height()}, {"dpi", dpi}});
            saveManifest("running");
            if (progress) progress(double(result.completed) / (jobs.size() + (options.value("figure").toBool() ? 1 : 0)));
        }
        if (!cancelled() && options.value("figure").toBool()) {
            const auto error = writePanelFigure(paths, figureTimes, QDir(result.directory).filePath("figure." + figureFormat), size, columns, dpi, background);
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
        }
        result.cancelled = cancelled();
        if (result.cancelled) result.error = "已取消批量导出；已完成文件保留。 / Batch cancelled; completed files retained.";
        saveManifest(result.cancelled ? "cancelled" : "complete");
        if (!result.cancelled && progress) progress(1.);
    } catch (const std::exception &error) {
        result.error = QString::fromUtf8(error.what()); result.cancelled = cancelled();
        if (!result.directory.isEmpty()) {
            try { saveManifest(result.cancelled ? "cancelled" : "failed"); }
            catch (const std::exception &manifestError) { result.error += ' ' + QString::fromUtf8(manifestError.what()); }
        }
    }
    return result;
}
}
