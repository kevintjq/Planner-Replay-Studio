#pragma once

#include "trajectory_image.h"
#include <QAbstractListModel>

class TrajectoryCanvasModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { LayerRow = Qt::UserRole + 1 };
    explicit TrajectoryCanvasModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return {{LayerRow, "layerRow"}}; }
    void setLayers(const QVariantList &layers);
private:
    QVariantList layers_;
};

class TrajectoryFrameModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { Seconds = Qt::UserRole + 1, Excluded, Valid, Opacity, Selected, Thumbnail, Manual, ForegroundError, ForegroundFraction };
    explicit TrajectoryFrameModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setFrames(std::shared_ptr<const replay::FixedCameraTrajectory> trajectory,
                   const std::vector<size_t> &samples, quint64 revision);
    void setEdits(const replay::TrajectoryEdits &edits);
    void setSelection(const QSet<qint64> &selection);
    std::optional<qint64> stampAt(int row) const;
private:
    std::shared_ptr<const replay::FixedCameraTrajectory> trajectory_;
    std::vector<size_t> samples_;
    replay::TrajectoryEdits edits_;
    replay::TrajectoryExclusions exclusions_{edits_};
    QSet<qint64> selection_;
    quint64 revision_ = 0;
};
