#pragma once

#include <QJsonObject>
#include <QMap>
#include <QPointF>
#include <QVector>
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

struct TrajectoryMaskStroke {
    QVector<QPointF> points;
    double radius = .03;
    bool erase = false;
    bool operator==(const TrajectoryMaskStroke &) const = default;
};

struct TrajectoryFrameEdit {
    bool excluded = false, manual = false;
    double opacity = 1.;
    std::vector<TrajectoryMaskStroke> maskStrokes;
    bool removed = false;
    bool operator==(const TrajectoryFrameEdit &) const = default;
};

// Only source-pixel mask changes alter this immutable image content identity.
QString trajectoryMaskContentKey(const TrajectoryFrameEdit &edit);

// Output colours are document settings, independent of application appearance.
struct TrajectoryLineStyle {
    QString color = "#38a5eb";
    double width = 2., opacity = 1.;
    int pattern = 0; // solid, dash, dot, dash-dot
    bool operator==(const TrajectoryLineStyle &) const = default;
};

struct TrajectoryEndpointStyle {
    bool enabled = false, showText = true, showTime = false;
    QString fill = "#34c759", stroke = "#ffffff", textColor = "#202124", text = "Start";
    double size = 12., strokeWidth = 1.5, opacity = 1., fontSize = 12.;
    bool operator==(const TrajectoryEndpointStyle &) const = default;
};

struct TrajectoryEdits {
    std::vector<TrajectoryRange> ranges;
    QMap<qint64, TrajectoryFrameEdit> frames;
    std::optional<std::pair<qint64, qint64>> trim;
    bool showLine = false;
    bool lineOnly = false;
    bool skipInvalid = false;
    TrajectoryLineStyle line;
    TrajectoryEndpointStyle start;
    TrajectoryEndpointStyle goal = [] { TrajectoryEndpointStyle style; style.fill = "#ff453a"; style.text = "Goal"; return style; }();

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
