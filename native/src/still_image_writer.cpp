#include "still_image_writer.h"
#include <QFileInfo>
#include <QImageWriter>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSvgGenerator>
#include <cmath>
#include <functional>

QString replay::writeStillImage(QImage image, const QString &path, int dpi, double time)
{
    if (image.isNull() || dpi < 36 || dpi > 2400 || !std::isfinite(time))
        return QStringLiteral("无效图像、DPI 或实验时间。 / Invalid image, DPI or time.");
    const auto format = QFileInfo(path).suffix().toLower().toLatin1();
    const auto description = QStringLiteral("UAV Studio; experiment_time_seconds=%1; embedded raster composition")
                                 .arg(time, 0, 'f', 9);
    image.setDotsPerMeterX(qRound(dpi / .0254));
    image.setDotsPerMeterY(qRound(dpi / .0254));
    image.setText("Description", description);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    if (format == "pdf" || format == "svg") {
        bool painted = false;
        if (format == "pdf") {
            QPdfWriter writer(&file);
            writer.setResolution(dpi);
            writer.setTitle(description);
            writer.setCreator("UAV Studio");
            writer.setPageSize(QPageSize(QSizeF(image.width() * 25.4 / dpi, image.height() * 25.4 / dpi), QPageSize::Millimeter));
            writer.setPageMargins(QMarginsF(), QPageLayout::Millimeter);
            QPainter painter(&writer);
            if (painter.isActive()) {
                painter.drawImage(QRect(0, 0, writer.width(), writer.height()), image);
                painted = painter.end();
            }
        } else {
            QSvgGenerator writer;
            writer.setOutputDevice(&file);
            writer.setResolution(dpi);
            writer.setSize(image.size());
            writer.setViewBox(QRect(QPoint(), image.size()));
            writer.setTitle("UAV Studio experiment frame");
            writer.setDescription(description);
            QPainter painter(&writer);
            if (painter.isActive()) { painter.drawImage(QPoint(), image); painted = painter.end(); }
        }
        if (!painted) return QStringLiteral("无法生成文档。 / Cannot render document.");
    } else {
        const auto codec = format == "jpg" ? QByteArray("jpeg") : format == "tif" ? QByteArray("tiff") : format;
        if (codec != "png" && codec != "jpeg" && codec != "tiff")
            return QStringLiteral("请选择 PNG/JPG/TIFF/PDF/SVG。 / Unsupported format.");
        if (!QImageWriter::supportedImageFormats().contains(codec))
            return QStringLiteral("缺少 %1 图像编码插件。 / Missing image codec.").arg(QString::fromLatin1(codec));
        QImageWriter writer(&file, codec);
        writer.setQuality(100);
        if (codec == "png") writer.setCompression(6);
        writer.setText("Description", description);
        if (!writer.write(image)) return writer.errorString();
    }
    return file.commit() ? QString() : file.errorString();
}

namespace {
int captionHeight(QSize cell) { return qBound(28, cell.height() / 12, 120); }
QString panelLetter(int index) {
    QString label;
    do { label.prepend(QChar('a' + index % 26)); index = index / 26 - 1; } while (index >= 0);
    return label;
}
}
QString replay::validatePanelFigure(int count, QSize cell, int columns)
{
    if (count < 1 || count > 1000 || columns < 1 || columns > 12 || cell.width() < 16 || cell.height() < 16 || cell.width() > 8192 || cell.height() > 8192)
        return QStringLiteral("无效科研图尺寸或列数。 / Invalid figure dimensions or columns.");
    columns = std::min(columns, count);
    const qint64 width = qint64(columns) * cell.width(), height = qint64((count + columns - 1) / columns) * (cell.height() + captionHeight(cell));
    if (width > 16384 || height > 16384 || width * height > 40'000'000)
        return QStringLiteral("多联图最多 4000 万像素、单边 16384；请减小每张尺寸或分批。 / Figure exceeds memory budget.");
    return {};
}
QString replay::writePanelFigure(const QStringList &paths, const QList<double> &times, const QString &path,
    QSize cell, int columns, int dpi, const QColor &background)
{
    const auto validation = validatePanelFigure(paths.size(), cell, columns);
    if (!validation.isEmpty()) return validation;
    if (paths.size() != times.size() || dpi < 36 || dpi > 2400 || !background.isValid()) return QStringLiteral("Invalid figure metadata.");
    const auto format = QFileInfo(path).suffix().toLower();
    if (!QStringList{"png", "jpg", "tiff", "pdf", "svg"}.contains(format)) return QStringLiteral("Invalid figure format.");
    for (int i = 0; i < paths.size(); ++i)
        if (!std::isfinite(times[i]) || QFileInfo(paths[i]).absoluteFilePath().compare(QFileInfo(path).absoluteFilePath(), Qt::CaseInsensitive) == 0)
            return QStringLiteral("Figure output must not overwrite an input panel.");
    columns = std::min(columns, int(paths.size()));
    const int caption = captionHeight(cell);
    const QSize size(columns * cell.width(), ((paths.size() + columns - 1) / columns) * (cell.height() + caption));
    const auto draw = [&](QPainter &painter) -> QString {
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        painter.fillRect(QRect(QPoint(), size), background);
        const QColor text = background.alpha() == 0 || background.lightnessF() > .45 ? QColor(Qt::black) : QColor(Qt::white);
        painter.setPen(text);
        QFont font("Arial"); font.setPixelSize(std::max(14, caption / 2)); painter.setFont(font);
        for (int i = 0; i < paths.size(); ++i) {
            const QImage panel(paths[i]);
            if (panel.isNull() || panel.size() != cell) return QStringLiteral("Panel is missing or dimensions differ: ") + paths[i];
            const QPoint origin((i % columns) * cell.width(), (i / columns) * (cell.height() + caption));
            painter.drawImage(origin, panel);
            // PDF / SVG 中的编号与实验时间保持真实文字；三维/视频合成明确嵌入为位图。
            painter.drawText(QRect(origin.x() + caption / 3, origin.y() + cell.height(), cell.width() - caption * 2 / 3, caption),
                Qt::AlignVCenter | Qt::AlignLeft, QString("(%1)  t = %2 s").arg(panelLetter(i), QString::number(times[i], 'f', 9)));
        }
        return {};
    };
    if (format != "pdf" && format != "svg") {
        QImage figure(size, QImage::Format_ARGB32_Premultiplied);
        if (figure.isNull()) return QStringLiteral("Insufficient memory for figure.");
        figure.fill(Qt::transparent); QPainter painter(&figure);
        const auto error = draw(painter); painter.end();
        return error.isEmpty() ? writeStillImage(figure, path, dpi, times.first()) : error;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    QString error;
    const auto description = QStringLiteral("UAV Studio; embedded raster panels; vector panel labels and experiment timestamps");
    if (format == "pdf") {
        QPdfWriter writer(&file); writer.setResolution(dpi); writer.setTitle(description); writer.setCreator("UAV Studio");
        writer.setPageSize(QPageSize(QSizeF(size.width() * 25.4 / dpi, size.height() * 25.4 / dpi), QPageSize::Millimeter));
        writer.setPageMargins(QMarginsF(), QPageLayout::Millimeter);
        QPainter painter(&writer);
        if (!painter.isActive()) return QStringLiteral("Cannot create PDF figure.");
        painter.scale(double(writer.width()) / size.width(), double(writer.height()) / size.height());
        error = draw(painter); if (!painter.end() && error.isEmpty()) error = QStringLiteral("Cannot finish PDF figure.");
    } else {
        QSvgGenerator writer; writer.setOutputDevice(&file); writer.setResolution(dpi); writer.setSize(size);
        writer.setViewBox(QRect(QPoint(), size)); writer.setTitle("UAV Studio scientific figure"); writer.setDescription(description);
        QPainter painter(&writer);
        if (!painter.isActive()) return QStringLiteral("Cannot create SVG figure.");
        error = draw(painter); if (!painter.end() && error.isEmpty()) error = QStringLiteral("Cannot finish SVG figure.");
    }
    if (!error.isEmpty()) return error;
    return file.commit() ? QString() : file.errorString();
}
