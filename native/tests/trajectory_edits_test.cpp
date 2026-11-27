#include "trajectory_image.h"
#include "trajectory_frame_model.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <iostream>
#include <stdexcept>

void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace replay;
    try {
        TrajectoryEdits edits;
        edits.ranges = {{"a", 100, 300, DroneFrames}, {"b", 200, 400, Both}, {"c", 500, 600, TrajectoryLine}};
        TrajectoryExclusions excluded(edits);
        expect(!excluded.contains(99, DroneFrames) && excluded.contains(100, DroneFrames) && !excluded.contains(400, DroneFrames), "Half-open boundaries");
        expect(excluded.duration(DroneFrames) == 300 && excluded.duration(TrajectoryLine) == 300, "Independent union durations");
        edits.ranges.erase(edits.ranges.begin());
        TrajectoryExclusions restored(edits);
        expect(!restored.contains(100, DroneFrames) && restored.contains(250, DroneFrames), "Restore must preserve overlapping exclusion");
        edits.frames[9007199254740993LL] = {true, true, .3};
        edits.trim = {100, 800}; edits.showLine = true;
        expect(TrajectoryEdits::fromJson(edits.toJson()) == edits, "Nanosecond JSON must be lossless beyond double precision");
        auto invalid = edits.toJson(); invalid.insert("frames", QJsonArray{QJsonObject{{"stamp_ns", "2"}, {"opacity", 2.}}});
        bool refused = false;
        try { TrajectoryEdits::fromJson(invalid); } catch (...) { refused = true; }
        expect(refused, "Reject invalid frame state");

        auto track = std::make_shared<FixedCameraTrajectory>();
        track->background = QImage(120, 30, QImage::Format_ARGB32_Premultiplied); track->background.fill(Qt::black);
        QImage sprite(4, 4, QImage::Format_ARGB32_Premultiplied); sprite.fill(Qt::red);
        for (int i = 0; i < 11; ++i) track->frames.push_back({i / 10., QRect(5 + i * 10, 12, 4, 4), sprite, {}});
        TrajectoryEdits state;
        state.frames[trajectoryTimestamp(.3)] = {true, true, .5};
        const auto half = editedTrajectorySamples(*track, .5, 0, 12, state);
        expect(half == std::vector<size_t>({0, 3, 5, 10}), "Manual source frame survives resampling");
        const auto tenth = editedTrajectorySamples(*track, .1, 0, 12, state);
        expect(tenth.size() == 11 && state.frames[trajectoryTimestamp(.3)].excluded, "Resampling must preserve exclusion identity");
        auto image = renderTrajectory(*track, tenth, 1., {}, {}, state);
        expect(image.pixelColor(36, 13).red() == 0, "Timestamp exclusion must remove rendered frame");
        state.frames[trajectoryTimestamp(.3)].excluded = false;
        image = renderTrajectory(*track, {3}, .5, {}, {}, state);
        expect(std::abs(image.pixelColor(36, 13).red() - 64) <= 1, "Frame and global opacity multiply");
        state.showLine = true; state.ranges = {{"gap", trajectoryTimestamp(.45), trajectoryTimestamp(.55), TrajectoryLine}};
        image = renderTrajectory(*track, {}, 1., {}, {}, state);
        expect(image.pixelColor(57, 13) == QColor(Qt::black) && image.pixelColor(25, 13) != QColor(Qt::black), "No bridge across excluded line interval");
        state.trim = {trajectoryTimestamp(.2), trajectoryTimestamp(.7)};
        const auto trimmed = editedTrajectorySamples(*track, .1, 1, 3, state);
        expect(trimmed.front() == 2 && trimmed.back() == 6, "Count sampling respects half-open trim");

        // 一小时 10 Hz 列表：模型只保存索引，缩略图由可见项按需请求。
        auto longTrack = std::make_shared<FixedCameraTrajectory>();
        for (int i = 0; i < 36000; ++i) longTrack->frames.push_back({i / 10., {}, {}, {}});
        TrajectoryFrameModel model;
        const auto samples = editedTrajectorySamples(*longTrack, .1, 0, 12, {});
        model.setFrames(longTrack, samples, 7);
        model.setSelection({trajectoryTimestamp(3599.9)});
        expect(model.rowCount() == 36000 && model.stampAt(35999) == trajectoryTimestamp(3599.9), "Long frame model preserves source time");
        std::cout << "Ranges, overlap recovery, JSON, resampling, alpha, line gaps and 36k frame model passed.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
