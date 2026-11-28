#pragma once
#include <QQuickPaintedItem>
#include <QFutureWatcher>
#include <QImage>
#include <QVariantMap>

class CanvasScientificItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QVariantMap dataMap READ dataMap WRITE setDataMap NOTIFY dataMapChanged)
    Q_PROPERTY(qreal contentOpacity READ contentOpacity WRITE setContentOpacity NOTIFY contentOpacityChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY imageChanged)
    Q_PROPERTY(QString error READ error NOTIFY imageChanged)
public:
    explicit CanvasScientificItem(QQuickItem *parent=nullptr);
    ~CanvasScientificItem() override;
    QVariantMap dataMap() const { return data_; }
    void setDataMap(const QVariantMap &value);
    qreal contentOpacity() const { return contentOpacity_; }
    void setContentOpacity(qreal value);
    bool loading() const { return watcher_.isRunning() || loadedSource_ != requestedSource_; }
    QString error() const { return error_; }
    void paint(QPainter *painter) override;
signals:
    void dataMapChanged();
    void contentOpacityChanged();
    void imageChanged();
private:
    void loadImage();
    QVariantMap data_;
    qreal contentOpacity_=1.;
    QString requestedSource_,loadedSource_,workingSource_,error_;
    QImage image_;
    QFutureWatcher<QPair<QImage,QString>> watcher_;
};
