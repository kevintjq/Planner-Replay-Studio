#pragma once

#include <QFutureWatcher>
#include <QPainter>
#include <QQuickPaintedItem>
#include <QUrl>
#include <QVariantMap>

// The same vector commands paint the editable preview and standalone files.
// Input maps are snapshots of SpeedMeterModel::frame() and project-owned style.
class SpeedGaugeItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QVariantMap dataMap READ dataMap WRITE setDataMap NOTIFY dataMapChanged)
    Q_PROPERTY(QVariantMap styleMap READ styleMap WRITE setStyleMap NOTIFY styleMapChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
public:
    explicit SpeedGaugeItem(QQuickItem *parent = nullptr);
    ~SpeedGaugeItem() override;
    QVariantMap dataMap() const { return data_; }
    QVariantMap styleMap() const { return style_; }
    void setDataMap(const QVariantMap &value);
    void setStyleMap(const QVariantMap &value);
    bool exporting() const { return exporting_; }
    void paint(QPainter *painter) override;

    Q_INVOKABLE QVariantMap presetStyle(const QString &name) const;
    // Returns an ID immediately. Completion identifies the same immutable snapshot.
    Q_INVOKABLE QString exportElement(const QUrl &destination, const QSize &pixelSize,
                                     int dpi = 300, const QString &format = {});
    static QVariantMap defaultStyle(const QString &preset = QStringLiteral("gradient-gauge"));
    static void drawGauge(QPainter &painter, const QSizeF &size,
                          const QVariantMap &data, const QVariantMap &style);
    static QVariantMap writeElement(const QString &path, const QSize &pixelSize, int dpi,
                                    const QString &format, const QVariantMap &data,
                                    const QVariantMap &style);
signals:
    void dataMapChanged();
    void styleMapChanged();
    void exportingChanged();
    void exportFinished(const QString &jobId, bool success, const QVariantMap &metadata,
                        const QString &error);
private:
    QVariantMap data_, style_;
    QFutureWatcher<QVariantMap> watcher_;
    QString jobId_;
    bool exporting_ = false;
};
