#pragma once
#include <QQuickPaintedItem>
#include <QImage>
#include <QFutureWatcher>
#include <QTimer>
#include <QUrl>
#include <memory>

// Source-quality decoding runs on a worker. Preview and export use the same PTS selector.
class ClipPreview : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(double sourceSeconds READ sourceSeconds WRITE setSourceSeconds NOTIFY requestChanged)
    Q_PROPERTY(bool decodingEnabled READ decodingEnabled WRITE setDecodingEnabled NOTIFY requestChanged)
    Q_PROPERTY(bool precise READ precise WRITE setPrecise NOTIFY requestChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY frameChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY frameChanged)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY frameChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double actualSeconds READ actualSeconds NOTIFY frameChanged)
    Q_PROPERTY(QString sourcePts READ sourcePts NOTIFY frameChanged)
    Q_PROPERTY(QString error READ error NOTIFY frameChanged)
public:
    explicit ClipPreview(QQuickItem *parent = nullptr);
    ~ClipPreview() override;
    QUrl source() const { return source_; }
    void setSource(const QUrl &source);
    double sourceSeconds() const { return requested_; }
    void setSourceSeconds(double time);
    bool decodingEnabled() const { return enabled_; }
    void setDecodingEnabled(bool value);
    bool precise() const { return precise_; }
    void setPrecise(bool value);
    bool ready() const { return !image_.isNull(); }
    bool busy() const { return watcher_.isRunning() || timer_.isActive(); }
    QSize imageSize() const { return image_.size(); }
    double duration() const { return duration_; }
    double actualSeconds() const { return actual_; }
    QString sourcePts() const { return pts_; }
    QString error() const { return error_; }
    void paint(QPainter *painter) override;
signals:
    void sourceChanged();
    void durationChanged();
    void requestChanged();
    void frameChanged();
private:
    struct Decoder;
    struct Result {
        QImage image;
        QString error, pts;
        double duration = 0., actual = 0., requested = 0.;
        quint64 revision = 0;
        QUrl source;
    };
    void request();
    void decode();
    QUrl source_;
    double requested_ = 0., duration_ = 0., actual_ = 0.;
    bool enabled_ = true, precise_ = true;
    quint64 revision_ = 0;
    QImage image_;
    QString error_, pts_;
    QTimer timer_;
    QFutureWatcher<Result> watcher_;
    std::shared_ptr<Decoder> decoder_;
};
