#pragma once
#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>

namespace replay {
// 图像编码在工作线程执行；PDF/SVG 当前明确为包含位图的文档，不声称纯矢量。
QString writeStillImage(QImage image, const QString &path, int dpi, double experimentTime);
QString validatePanelFigure(int count, QSize cellSize, int columns);
QString writePanelFigure(const QStringList &paths, const QList<double> &times, const QString &path,
                         QSize cellSize, int columns, int dpi, const QColor &background);
}
