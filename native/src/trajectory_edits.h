#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <vector>

namespace replay {

// 编辑身份使用源视频时间（纳秒），不使用会随重采样改变的列表行号。
qint64 trajectoryTimestamp(double seconds);

enum ExclusionPart { DroneFrames = 1, TrajectoryLine = 2, Both = 3 };

struct TrajectoryRange {
    QString id;
    qint64 begin = 0, end = 0;
    int parts = DroneFrames;
    bool operator==(const TrajectoryRange &) const = default;
};

struct TrajectoryFrameEdit {
    bool excluded = false, manual = false;
    double opacity = 1.;
    bool operator==(const TrajectoryFrameEdit &) const = default;
};

struct TrajectoryEdits {
    std::vector<TrajectoryRange> ranges;
    QMap<qint64, TrajectoryFrameEdit> frames;
    std::optional<std::pair<qint64, qint64>> trim;
    bool showLine = false;

    QJsonObject toJson() const;
    static TrajectoryEdits fromJson(const QJsonObject &json);
    bool operator==(const TrajectoryEdits &) const = default;
};

// 保留管理器中每条区间的身份；只在求值时分别求两种排除类型的并集。
// 因此恢复一条重叠区间不会错误恢复另一条区间仍然排除的内容。
class TrajectoryExclusions {
public:
    explicit TrajectoryExclusions(const TrajectoryEdits &edits);
    bool contains(qint64 stamp, int part) const;
    bool intersects(qint64 begin, qint64 end, int part) const;
    qint64 duration(int part) const;
private:
    using Range = std::pair<qint64, qint64>;
    std::vector<Range> drone_, line_;
    const std::vector<Range> &ranges(int part) const;
};

}
