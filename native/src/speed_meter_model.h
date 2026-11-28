#pragma once
#include "speed_meter_data.h"
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QUrl>
#include <atomic>

// Runtime data/cache only. Project/layer settings remain owned by ProjectDocument.
class SpeedMeterModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(bool working READ working NOTIFY changed)
    Q_PROPERTY(bool exporting READ exporting NOTIFY changed)
public:
    explicit SpeedMeterModel(QObject *parent = nullptr);
    ~SpeedMeterModel() override;
    QVariantList sources() const;
    bool working() const { return !jobs_.isEmpty(); }
    bool exporting() const { return exportWatcher_.isRunning(); }
    void setTransforms(std::shared_ptr<const replay::TransformTree> transforms);
    void useRecording(const QString &meterId, std::shared_ptr<const replay::Recording> recording);
    Q_INVOKABLE void loadSource(const QString &meterId, const QUrl &bag, const QVariantMap &source);
    Q_INVOKABLE void removeMeter(const QString &meterId);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap sourceState(const QString &meterId) const;
    Q_INVOKABLE QVariantMap frame(const QString &meterId, const QString &exactStampNs,
                                 const QVariantMap &settings = {}) const;
    Q_INVOKABLE QVariantList plot(const QString &meterId, const QVariantMap &settings = {}, int maximumPoints = 2000) const;
    Q_INVOKABLE void exportCsv(const QString &meterId, const QUrl &destination, const QVariantMap &settings = {});
signals:
    void changed();
    void exportFinished(bool success, const QString &message);
private:
    struct Bundle {
        replay::SpeedSeries series;
        std::shared_ptr<const replay::Recording> recordingOwner;
        std::array<std::shared_ptr<const replay::SpeedIndex>, 7> indices;
    };
    struct Result { std::shared_ptr<const Bundle> bundle; QString error; };
    struct Job {
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
        QFutureWatcher<Result> *watcher = nullptr;
        quint64 generation = 0;
    };
    static std::shared_ptr<const Bundle> makeBundle(replay::SpeedSeries series, const replay::Cancel &cancel = {},
                                                  std::shared_ptr<const replay::Recording> owner = {});
    std::shared_ptr<const Bundle> data(const QString &id) const;
    QHash<QString, QString> meters_, errors_;
    QHash<QString, std::shared_ptr<const Bundle>> cache_;
    QHash<QString, std::shared_ptr<Job>> jobs_;
    std::shared_ptr<const replay::TransformTree> transforms_;
    QFutureWatcher<QString> exportWatcher_;
    quint64 generation_ = 0;
};
