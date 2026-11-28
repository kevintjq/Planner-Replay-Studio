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
#include <QPdfWriter>
#include <QPageSize>
#include <QPageLayout>
#include <QSaveFile>
#include <QSvgGenerator>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace replay {
QString writeTrajectoryStillImage(const FixedCameraTrajectory &trajectory, const std::vector<size_t> &samples,
    const TrajectoryEdits &edits, double opacity, const QVariantMap &canvasState, const QString &path,
    QSize size, int dpi, double time, const QColor &background, const TrackingCancelled &cancel,
    std::optional<QStringList> selectedLayers, QVariantMap *report)
{
    auto details = report ? *report : QVariantMap{};
    details["scope"] = selectedLayers ? "layers" : "canvas";
    details["layerIds"] = selectedLayers.value_or(QStringList{});
    details["composedUavInstances"] = 0; details["visibleLineSegments"] = 0; details["paintedLayers"] = 0;
    details["foregroundGateApplied"] = !selectedLayers.has_value(); details["cleanForegroundAccepted"] = false;
    details["foregroundValidation"] = selectedLayers ? "not_applicable_selected_layers" : edits.lineOnly ? "line_only" : "visible_uav_composition";
    details["time"] = time; details["width"] = size.width(); details["height"] = size.height(); details["dpi"] = dpi;
    const auto finish = [&](const QString &error) {
        details["success"] = error.isEmpty(); details["error"] = error;
        if (report) *report = details;
        return error;
    };
    try {
        if (size.width() < 16 || size.height() < 16 || size.width() > 8192 || size.height() > 8192 ||
            qint64(size.width())*size.height() > 40'000'000 || dpi < 36 || dpi > 2400 || !std::isfinite(time) || !background.isValid())
            return finish(QStringLiteral("Invalid trajectory export dimensions, DPI, timestamp or background."));
        auto exportSamples = samples;
        if (selectedLayers) {
            selectedLayers->removeDuplicates();
            if (selectedLayers->isEmpty() || std::any_of(selectedLayers->cbegin(),selectedLayers->cend(),[](const auto &id){return id.trimmed().isEmpty();}))
                return finish(QStringLiteral("请选择至少一个有效图层。 / Select at least one valid layer."));
            details["layerIds"] = *selectedLayers;
            QSet<QString> spriteSources; const auto copies = canvasState.value("copies").toMap();
            for (const auto &id : *selectedLayers) spriteSources.insert(copies.value(id,id).toString());
            std::erase_if(exportSamples,[&](size_t index) {
                return index>=trajectory.frames.size() || !spriteSources.contains("sprite:"+QString::number(trajectoryTimestamp(trajectory.frames[index].seconds)));
            });
        }
        const auto layers = trajectoryLayers(trajectory,exportSamples,opacity,{},edits,cancel);
        if (cancel && cancel()) return finish(QStringLiteral("已取消。 / Cancelled."));
        const QSize canvas(canvasState.value("outputWidth",layers.canvasSize.width()).toInt(),canvasState.value("outputHeight",layers.canvasSize.height()).toInt());
        if (canvas.isEmpty()) return finish(QStringLiteral("Empty trajectory canvas."));
        auto state = canvasState; state["outputWidth"] = canvas.width(); state["outputHeight"] = canvas.height();
        if (selectedLayers) {
            auto rows = trajectoryCanvasLayerRows(trajectory,layers,edits,opacity,state); rows.append(state.value("elements").toList());
            for (const auto &id : *selectedLayers) if (std::none_of(rows.cbegin(),rows.cend(),[&](const auto &row){return row.toMap().value("id").toString()==id;}))
                return finish(QStringLiteral("选中图层不可用：%1 / Selected layer is unavailable.").arg(id));
        }
        auto content = canvas; content.scale(size,Qt::KeepAspectRatio);
        const QRectF area(QPointF((size.width()-content.width())*.5,(size.height()-content.height())*.5),content);
        const auto paint = [&](QPainter &painter) -> QString {
            painter.fillRect(QRectF(QPointF(),size),background);
            TrajectoryCompositionStats counts;
            paintTrajectoryLayers(painter,trajectory,layers,area,edits,cancel,TrajectoryAll,state,time,&counts,selectedLayers.value_or(QStringList{}));
            details["composedUavInstances"] = counts.composed; details["visibleLineSegments"] = counts.lineSegments; details["paintedLayers"] = counts.paintedLayers;
            details["failedSamples"] = counts.failed; details["filteredSamples"] = counts.filtered;
            if (cancel && cancel()) return QStringLiteral("已取消。 / Cancelled.");
            return selectedLayers ? counts.paintedLayers>0 ? QString() : QStringLiteral("选中图层均隐藏、为空或位于画布外。 / Selected layers have no visible canvas content.") : counts.exportError(edits);
        };
        const auto selectionDescription = QStringLiteral("export_scope=%1; selected_layer_ids=%2; clean_foreground_accepted=false")
            .arg(details.value("scope").toString(),selectedLayers.value_or(QStringList{}).join(','));
        const auto suffix = QFileInfo(path).suffix().toLower();
        if (suffix != "pdf" && suffix != "svg") {
            QImage output(size,QImage::Format_ARGB32_Premultiplied); if (output.isNull()) return finish(QStringLiteral("Insufficient memory for trajectory image."));
            output.fill(Qt::transparent); QPainter painter(&output); const auto error = paint(painter); painter.end();
            output.setText("SourceTimestamp",details.value("sourceTimestamp").toString()+"; "+selectionDescription+QStringLiteral("; composed_uav_instances=%1").arg(details.value("composedUavInstances").toInt()));
            return finish(error.isEmpty() ? writeStillImage(output,path,dpi,time) : error);
        }
        QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) return finish(file.errorString());
        const auto description = QStringLiteral("UAV Studio; experiment_time_seconds=%1; embedded raster background and UAV video sprites; vector trajectory, endpoint circles and captions; ").arg(time,0,'f',9)+selectionDescription;
        QString error;
        if (suffix == "pdf") {
            QPdfWriter writer(&file); writer.setResolution(dpi); writer.setTitle(description); writer.setCreator("UAV Studio");
            writer.setPageSize(QPageSize(QSizeF(size.width()*25.4/dpi,size.height()*25.4/dpi),QPageSize::Millimeter));
            writer.setPageMargins(QMarginsF(),QPageLayout::Millimeter);
            QPainter painter(&writer); if (!painter.isActive()) return finish(QStringLiteral("Cannot create trajectory PDF."));
            painter.scale(double(writer.width())/size.width(),double(writer.height())/size.height());
            error = paint(painter); if (!painter.end() && error.isEmpty()) error = QStringLiteral("Cannot finish trajectory PDF.");
        } else {
            QSvgGenerator writer; writer.setOutputDevice(&file); writer.setResolution(dpi); writer.setSize(size); writer.setViewBox(QRect(QPoint(),size));
            writer.setTitle("UAV Studio trajectory frame"); writer.setDescription(description);
            QPainter painter(&writer); if (!painter.isActive()) return finish(QStringLiteral("Cannot create trajectory SVG."));
            error = paint(painter); if (!painter.end() && error.isEmpty()) error = QStringLiteral("Cannot finish trajectory SVG.");
        }
        if (!error.isEmpty()) return finish(error);
        if (cancel && cancel()) return finish(QStringLiteral("已取消。 / Cancelled."));
        return finish(file.commit() ? QString() : file.errorString());
    } catch (const std::exception &error) { return finish(QString::fromUtf8(error.what())); }
}
TrajectoryBatchResult exportTrajectoryBatch(const QString &source, const QString &parentDirectory,
    std::shared_ptr<const FixedCameraTrajectory> trajectory, const std::vector<size_t> &samples,
    const TrajectoryEdits &edits, double opacity, double currentTime, const QVariantMap &options,
    const TrackingCancelled &cancel, const TrackingProgress &progress, const QVariantMap &canvasState)
{
    TrajectoryBatchResult result;
    QJsonArray completed;
    QJsonArray sampleTimes;
    QJsonArray foregroundWarnings;
    if (trajectory) for (auto index : samples) if (index < trajectory->frames.size())
        sampleTimes.append(QString::number(trajectoryTimestamp(trajectory->frames[index].seconds)));
    if (trajectory) for (auto index : samples) if (index < trajectory->frames.size() && !trajectory->frames[index].foregroundError.isEmpty())
        foregroundWarnings.append(QJsonObject{{"time", trajectory->frames[index].seconds}, {"reason", trajectory->frames[index].foregroundError}});
    QVector<FrameExportJob> jobs;
    const auto content = options.value("content", "source").toString();
    const auto cancelled = [&] { return cancel && cancel(); };
    const auto saveManifest = [&](const QString &status) {
        QSaveFile file(QDir(result.directory).filePath("manifest.json"));
        const QJsonObject document{{"status", status}, {"error", result.error}, {"content", content},
            {"time_basis", "source_video_seconds"}, {"source_file", QFileInfo(source).fileName()},
            {"original_preview_time", currentTime}, {"options", QJsonObject::fromVariantMap(options)},
            {"edits", edits.toJson()}, {"canvas", QJsonObject::fromVariantMap(canvasState)}, {"opacity", opacity}, {"completed", completed}, {"planned", int(jobs.size())},
            {"sample_timestamps_ns", sampleTimes},
            {"foreground_warnings", foregroundWarnings}, {"background_warning", trajectory ? trajectory->backgroundWarning : QString()},
            {"limitations", "Source time is relative to the first decoded presentation timestamp. Raw PTS and rational time_base are preserved; no FPS timestamp fallback. Unknown ordinals after seeking are null. Recovered timestamps are explicitly labelled. Sampling indices refer to the configured time grid. Trajectory snapshots preserve the current background, samples, edits and canvas, clipped at the requested time. Trajectory PDF/SVG frames retain vector paths/markers/captions with embedded raster video content. Figure panels are raster; PDF/SVG figure captions are vector."}};
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
        if (content == "source") reader = std::make_unique<VideoReader>(source, cancel);
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
        QStringList paths; QList<double> figureTimes; QTemporaryDir figureCache;
        for (const auto &job : jobs) {
            if (cancelled()) break;
            QImage frame; double sourceTime = job.time; int sourceIndex = -1;
            QJsonObject sourceStamp; std::optional<TrajectoryEdits> clippedEdits;
            if (reader) {
                if (!reader->select(job.time, cancel) || reader->frame.empty()) {
                    if (cancelled()) break;
                    throw std::runtime_error("No source video frame at the requested time.");
                }
                sourceTime = reader->seconds; sourceIndex = reader->index();
                sourceStamp = {{"pts", QString::number(reader->pts())}, {"origin_pts", QString::number(reader->originPts())},
                    {"time_base", QJsonArray{reader->timeBaseNumerator(), reader->timeBaseDenominator()}},
                    {"time_ns", QString::number(reader->timestampNs())}, {"kind", reader->hasSourcePts() ? "source_pts" : "decoder_recovered"},
                    {"coded_size", QJsonArray{reader->codedSize().width(), reader->codedSize().height()}},
                    {"sample_aspect_ratio", QJsonArray{reader->sampleAspectNumerator(), reader->sampleAspectDenominator()}},
                    {"display_rotation_degrees", reader->displayRotation()},
                    {"display_size", QJsonArray{reader->frame.cols, reader->frame.rows}},
                    {"pixel_normalization", "square_pixels_then_display_rotation"}};
                frame = imageOf(reader->frame).scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            } else {
                const auto stamp = trajectoryTimestamp(job.time);
                auto clipped = edits;
                const auto end = stamp + 1; // 编辑使用 [入点,出点)，加 1 纳秒以包含请求端点。
                if (clipped.trim) clipped.trim->second = std::min(clipped.trim->second, end);
                else clipped.trim = {trajectoryTimestamp(trajectory->frames.front().seconds), end};
                // 空交集用空的半开区间表达，不改变原始编辑范围。
                if (clipped.trim->first > clipped.trim->second) clipped.trim->first = clipped.trim->second;
                clippedEdits = clipped;
                TrajectoryCompositionStats counts;
                frame = renderTrajectory(*trajectory, samples, opacity, {}, size, clipped, cancel, true, &counts, true, canvasState, job.time);
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
            const bool vectorFrame = trajectory && clippedEdits && (QFileInfo(path).suffix().toLower() == "pdf" || QFileInfo(path).suffix().toLower() == "svg");
            const auto error = vectorFrame ? writeTrajectoryStillImage(*trajectory,samples,*clippedEdits,opacity,canvasState,path,size,dpi,outputTime,background,cancel) : writeStillImage(output, path, dpi, outputTime);
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
            QString panelPath = path;
            if (options.value("figure").toBool() && (QFileInfo(path).suffix().toLower() == "pdf" || QFileInfo(path).suffix().toLower() == "svg")) {
                if (!figureCache.isValid()) throw std::runtime_error("Cannot allocate temporary figure panels.");
                panelPath = figureCache.filePath(QString::number(result.completed)+".png");
                if (!output.save(panelPath,"PNG")) throw std::runtime_error("Cannot save a temporary figure panel.");
            }
            paths.append(panelPath); figureTimes.append(outputTime); ++result.completed;
            completed.append(QJsonObject{{"file", job.fileName}, {"requested_time", job.time}, {"output_time", outputTime},
                {"source_frame_time", sourceTime}, {"source_frame_index", sourceIndex < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(sourceIndex)},
                {"source_timestamp", sourceStamp}, {"width", size.width()}, {"height", size.height()}, {"dpi", dpi}});
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
