#include "trajectory_frame_model.h"
#include <QUrl>

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
    case Valid: return !frame.sprite.isNull() || !frame.cachedSprite.isEmpty();
    case Opacity: return edit.opacity;
    case Selected: return selection_.contains(stamp);
    case Manual: return edit.manual;
    case Thumbnail: return QUrl(QStringLiteral("image://trajectory/frame/%1/%2").arg(revision_).arg(source));
    default: return {};
    }
}

QHash<int, QByteArray> TrajectoryFrameModel::roleNames() const
{
    return {{Seconds, "seconds"}, {Excluded, "excluded"}, {Valid, "valid"}, {Opacity, "frameOpacity"},
            {Selected, "selected"}, {Thumbnail, "thumbnail"}, {Manual, "manual"}};
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
        emit dataChanged(index(0), index(rowCount() - 1), {Excluded, Opacity, Manual});
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
