#include "trajectory_layer_item.h"
#include <QPainter>

TrajectoryLayerItem::TrajectoryLayerItem(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true); setOpaquePainting(false);
}
void TrajectoryLayerItem::setController(TrajectoryController *controller)
{
    if (controller_ == controller) return;
    QObject::disconnect(updateConnection_); controller_ = controller;
    if (controller_) updateConnection_ = connect(controller_, &TrajectoryController::changed, this, [this] { update(); });
    emit controllerChanged(); update();
}
void TrajectoryLayerItem::setLayerId(const QString &id)
{
    if (layerId_ == id) return;
    layerId_ = id; emit layerIdChanged(); update();
}
void TrajectoryLayerItem::paint(QPainter *painter)
{
    if (!controller_ || !painter || layerId_.isEmpty()) return;
    try { controller_->paintCanvasLayer(*painter,layerId_,boundingRect()); }
    catch (const std::exception &) { /* A missing cache is reported by the controller/export gate. */ }
}
