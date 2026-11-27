#include "frame_batch_exporter.h"
#include "movie_exporter.h"
#include "replay_controller.h"
#include "still_image_writer.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>
#include <stdexcept>

FrameBatchExporter::FrameBatchExporter(ReplayController &replay, MovieExporter &movie) : replay_(replay), movie_(movie)
{
    connect(&movie_, &MovieExporter::changed, this, [this] { if (active_) emit changed(); });
    connect(&movie_, &MovieExporter::finished, this, [this](bool success, const QString &message) {
        if (!active_ || !waiting_) return;
        waiting_ = false;
        const auto &job = jobs_.at(index_);
        results_.append(QJsonObject{{"file", job.fileName}, {"experiment_time", job.time}, {"view", job.viewId},
            {"passed", success}, {"message", message}, {"cloud_framebuffers", QJsonArray::fromVariantList(movie_.captureMetrics())}});
        if (!saveManifest("running")) { finish(tr("无法更新批量导出清单。 / Cannot update manifest.")); return; }
        if (!success) { finish(message); return; }
        ++index_; emit changed();
        QTimer::singleShot(0, this, &FrameBatchExporter::next);
    });
    connect(&figureWriter_, &QFutureWatcher<QString>::finished, this, [this] { finish(figureWriter_.result()); });
}
FrameBatchExporter::~FrameBatchExporter() { figureWriter_.waitForFinished(); }
double FrameBatchExporter::progress() const {
    return jobs_.isEmpty() ? 0. : (index_ + (waiting_ ? movie_.progress() : 0.)) / jobs_.size();
}
void FrameBatchExporter::start(const QUrl &parentDirectory, QQuickItem *canvas, const QVariantMap &options)
{
    if (active_ || movie_.exporting() || replay_.loading() || replay_.capturing() || !canvas) return;
    try {
        if (!parentDirectory.isLocalFile() || !QDir(parentDirectory.toLocalFile()).exists()) throw std::runtime_error("Select an existing output directory.");
        jobs_ = replay::planFrameExport(replay_.duration(), options.value("currentTime", replay_.position()).toDouble(), replay_.markers(), replay_.cloudViews(), options);
        if (options.value("figure").toBool()) {
            const auto figureFormat = options.value("figureFormat", "pdf").toString();
            if (!QStringList{"png", "jpg", "tiff", "pdf", "svg"}.contains(figureFormat)) throw std::runtime_error("Invalid figure format.");
            for (const auto &job : jobs_) if (job.fileName.compare("figure." + figureFormat, Qt::CaseInsensitive) == 0)
                throw std::runtime_error("Frame filename conflicts with figure output.");
            const auto error = replay::validatePanelFigure(jobs_.size(), QSize(options.value("width", 1920).toInt(), options.value("height", 1080).toInt()), options.value("columns", 2).toInt());
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
        }
        // 每次创建新子目录，避免批量任务静默覆盖已有结果或用户素材。
        directory_ = QDir(parentDirectory.toLocalFile()).filePath("frames-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + '-' + QUuid::createUuid().toString(QUuid::Id128).left(8));
        if (!QDir().mkdir(directory_)) throw std::runtime_error("Cannot create output directory.");
        options_ = options; canvas_ = canvas; index_ = 0; results_ = {}; cancelled_ = false;
        previous_ = replay_.position(); wasPlaying_ = replay_.playing(); replay_.pause(); active_ = true;
        if (!saveManifest("running")) { finish(tr("无法创建导出清单。")); return; }
        emit changed(); QTimer::singleShot(0, this, &FrameBatchExporter::next);
    } catch (const std::exception &error) {
        message_ = QString::fromUtf8(error.what()); emit changed(); emit finished(false, message_);
    }
}
void FrameBatchExporter::next()
{
    if (!active_) return;
    if (cancelled_) { finish(tr("已取消批量导出；已完成文件保留。 / Batch cancelled; completed files retained.")); return; }
    if (!canvas_) { finish(tr("画布已关闭。 / Canvas closed.")); return; }
    if (index_ == jobs_.size()) {
        if (options_.value("figure").toBool()) {
            QStringList paths; QList<double> times;
            for (const auto &job : jobs_) { paths.append(QDir(directory_).filePath(job.fileName)); times.append(job.time); }
            const auto path = QDir(directory_).filePath("figure." + options_.value("figureFormat", "pdf").toString());
            const QSize size(options_.value("width", 1920).toInt(), options_.value("height", 1080).toInt());
            const int columns = options_.value("columns", 2).toInt(), dpi = options_.value("dpi", 300).toInt();
            const QColor background(options_.value("background", "white").toString());
            message_ = tr("正在生成多联科研图… / Composing figure…"); emit changed();
            figureWriter_.setFuture(QtConcurrent::run([paths, times, path, size, columns, dpi, background] {
                return replay::writePanelFigure(paths, times, path, size, columns, dpi, background);
            }));
        } else finish({});
        return;
    }
    auto options = options_;
    const auto &job = jobs_.at(index_);
    if (job.viewId != "canvas") { options.insert("scope", "view"); options.insert("viewId", job.viewId); }
    waiting_ = true;
    message_ = tr("导出 %1 / %2 · %3 s · %4").arg(index_ + 1).arg(jobs_.size()).arg(job.time, 0, 'f', 9).arg(job.viewId);
    emit changed();
    movie_.startStill(QUrl::fromLocalFile(QDir(directory_).filePath(job.fileName)), canvas_, job.time, options);
}
void FrameBatchExporter::cancel()
{
    if (!active_) return;
    cancelled_ = true;
    if (figureWriter_.isRunning()) { message_ = tr("正在完成科研图写入…"); emit changed(); return; }
    if (waiting_) movie_.cancel();
    else next();
}
bool FrameBatchExporter::saveManifest(const QString &status, const QString &error)
{
    QSaveFile file(QDir(directory_).filePath("manifest.json"));
    const QJsonObject document{{"status", status}, {"error", error}, {"options", QJsonObject::fromVariantMap(options_)},
        {"completed", results_}, {"planned", int(jobs_.size())}, {"original_experiment_time", previous_},
        {"limitations", "Video PTS certification pending; scene composition embedded as raster; PDF/SVG panel labels are vector."}};
    const auto bytes = QJsonDocument(document).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
void FrameBatchExporter::finish(const QString &error)
{
    if (!active_) return;
    auto finalError = error;
    if (cancelled_ && finalError.isEmpty()) finalError = tr("已取消批量导出；已完成文件保留。");
    if (!saveManifest(finalError.isEmpty() ? "complete" : cancelled_ ? "cancelled" : "failed", finalError)) finalError += tr(" 导出清单写入失败。");
    active_ = false; waiting_ = false; replay_.seek(previous_);
    if (wasPlaying_ && !replay_.playing()) replay_.togglePlayback();
    message_ = finalError.isEmpty() ? tr("已导出 %1 张图片：%2").arg(jobs_.size()).arg(directory_) : finalError;
    emit changed(); emit finished(finalError.isEmpty(), message_);
}
