#include "trajectory_frame_model.h"
#include <QUrl>

int TrajectoryCanvasModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(layers_.size());
}

QVariant TrajectoryCanvasModel::data(const QModelIndex &index, int role) const
{
    return role == LayerRow && index.isValid() && index.row() >= 0 && index.row() < layers_.size() ? layers_[index.row()] : QVariant{};
}

void TrajectoryCanvasModel::setLayers(const QVariantList &layers)
{
    const auto id = [](const QVariant &value) { return value.toMap().value("id").toString(); };
    QSet<QString> wanted;
    for (const auto &row : layers) {
        const auto key = id(row);
        if (key.isEmpty() || wanted.contains(key)) return;
        wanted.insert(key);
    }
    for (int row = int(layers_.size()) - 1; row >= 0; --row) if (!wanted.contains(id(layers_[row]))) {
        beginRemoveRows({}, row, row); layers_.removeAt(row); endRemoveRows();
    }
    for (int row = 0; row < layers.size(); ++row) {
        const auto key = id(layers[row]);
        int existing = row;
        while (existing < layers_.size() && id(layers_[existing]) != key) ++existing;
        if (existing == layers_.size()) {
            beginInsertRows({}, row, row); layers_.insert(row, layers[row]); endInsertRows();
        } else if (existing != row) {
            beginMoveRows({}, existing, existing, {}, row); layers_.move(existing, row); endMoveRows();
        }
        if (layers_[row] != layers[row]) {
            layers_[row] = layers[row]; emit dataChanged(index(row), index(row), {LayerRow});
        }
    }
}

int TrajectoryFrameModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(samples_.size());
}

QVariant TrajectoryFrameModel::data(const QModelIndex &index, int role) const
{
    if (!trajectory_ || !index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const auto source = samples_[size_t(index.row())];
    const auto &frame = trajectory_->frames[source];
    const auto stamp = replay::trajectoryTimestamp(frame.seconds);
    const auto edit = edits_.frames.value(stamp);
    switch (role) {
    case Seconds: return frame.seconds;
    case Excluded: return edit.excluded || exclusions_.contains(stamp, replay::DroneFrames);
    case Valid: return edit.maskStrokes.empty() ?
        (frame.alphaPixels >= 0 ? frame.alphaPixels > 0 : replay::trajectoryAlphaPixels(frame.sprite) > 0) :
        replay::trajectoryAlphaPixels(replay::trajectorySprite(frame, edit)) > 0;
    case ForegroundError: return frame.foregroundError;
    case ForegroundFraction: return frame.foregroundFraction;
    case Opacity: return edit.opacity;
    case Selected: return selection_.contains(stamp);
    case Manual: return edit.manual;
    case Thumbnail: return QUrl(QStringLiteral("image://trajectory/frame/%1/%2/%3").arg(revision_).arg(source).arg(replay::trajectoryMaskContentKey(edit)));
    default: return {};
    }
}

QHash<int, QByteArray> TrajectoryFrameModel::roleNames() const
{
    return {{Seconds, "seconds"}, {Excluded, "excluded"}, {Valid, "valid"}, {Opacity, "frameOpacity"},
            {Selected, "selected"}, {Thumbnail, "thumbnail"}, {Manual, "manual"},
            {ForegroundError, "foregroundError"}, {ForegroundFraction, "foregroundFraction"}};
}

void TrajectoryFrameModel::setFrames(std::shared_ptr<const replay::FixedCameraTrajectory> trajectory,
                                    const std::vector<size_t> &samples, quint64 revision)
{
    if (trajectory_ == trajectory && samples_ == samples && revision_ == revision)
        return;
    beginResetModel();
    trajectory_ = std::move(trajectory);
    samples_ = samples;
    revision_ = revision;
    endResetModel();
}

void TrajectoryFrameModel::setEdits(const replay::TrajectoryEdits &edits)
{
    edits_ = edits;
    exclusions_ = replay::TrajectoryExclusions(edits_);
    if (rowCount())
        emit dataChanged(index(0), index(rowCount() - 1), {Excluded, Opacity, Manual, Valid, Thumbnail});
}

void TrajectoryFrameModel::setSelection(const QSet<qint64> &selection)
{
    selection_ = selection;
    if (rowCount())
        emit dataChanged(index(0), index(rowCount() - 1), {Selected});
}

std::optional<qint64> TrajectoryFrameModel::stampAt(int row) const
{
    if (!trajectory_ || row < 0 || row >= rowCount())
        return {};
    return replay::trajectoryTimestamp(trajectory_->frames[samples_[size_t(row)]].seconds);
}
