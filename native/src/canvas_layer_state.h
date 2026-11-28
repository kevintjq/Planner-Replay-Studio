#pragma once
#include <QPainter>
#include <QRectF>
#include <QVariantMap>
#include <QHash>
#include <QImage>

// One deterministic evaluator for QML and QPainter; no controller/history.
namespace replay {
QVariantMap canvasItemSettings(const QVariantMap &state,const QString &id,double time=-1.,const QVariantMap &defaults={});
QRectF canvasItemGeometry(const QVariantMap &state,const QString &id,const QRectF &base,const QSizeF &canvas,double time,const QVariantMap &defaults={});
bool canvasItemActive(const QVariantMap &state,const QString &id,double time,bool exists=true,const QVariantMap &defaults={});
// save/restore at the call site; draw local content in [0,width] x [0,height].
void applyCanvasItemTransform(QPainter &painter,const QVariantMap &evaluated,const QRectF &geometry,double baseOpacity=1.);
// Background and inset border only; content opacity/transform is the caller's.
void paintCanvasItemAppearance(QPainter &painter,const QVariantMap &evaluated,const QSizeF &size,double pixelsPerOutputUnit=1.);
// Local files/file URLs only. The reader honors EXIF display orientation.
QImage loadCanvasElementImage(const QString &source,QString *error=nullptr);
QSize canvasElementImageSize(const QString &source);
QStringList canvasElementKinds();
QVariantMap canvasElementDefaults(const QString &kind);
// Content only: caller applies geometry, rotation, reflection and opacity.
bool paintCanvasElement(QPainter &painter,const QVariantMap &element,const QSizeF &size,const QImage &image={});
// Common scientific elements in z order. Bitmap images are embedded as bitmaps;
// text and shapes remain native QPainter primitives on PDF/SVG devices.
void paintCanvasElements(QPainter &painter,const QVariantMap &state,const QSizeF &size,double time,const QHash<QString,QImage> &images={});
}
