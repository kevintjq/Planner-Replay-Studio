#pragma once
#include <QQuickPaintedItem>
#include <QPointer>
#include "trajectory_controller.h"

// Live canvas representation of the same vector painter used by still export.
class TrajectoryLayerItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(TrajectoryController *controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(QString layerId READ layerId WRITE setLayerId NOTIFY layerIdChanged)
public:
    explicit TrajectoryLayerItem(QQuickItem *parent = nullptr);
    TrajectoryController *controller() const { return controller_; }
    void setController(TrajectoryController *controller);
    QString layerId() const { return layerId_; }
    void setLayerId(const QString &id);
    void paint(QPainter *painter) override;
signals:
    void controllerChanged();
    void layerIdChanged();
private:
    QPointer<TrajectoryController> controller_;
    QString layerId_;
    QMetaObject::Connection updateConnection_;
};
