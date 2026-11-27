#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace replay {
struct VideoTiming {
    using Range = std::pair<double, std::optional<double>>;
    double offset = 0;
    double trimIn = 0;
    std::optional<double> trimOut;
    std::vector<std::pair<double, double>> anchors;
    std::optional<std::vector<Range>> segments;
    std::array<double, 4> crop{0, 0, 1, 1};

    static VideoTiming fromJson(const QJsonObject &edit);
    double rate() const;
    double sourceSeconds(double replaySeconds) const;
    double replaySeconds(double sourceSeconds) const;
    bool includes(double sourceSeconds) const;
    std::vector<Range> spans() const;
    static QJsonArray encodeSpans(const std::vector<Range> &ranges);
};

struct VideoClip {
    QString path;
    QString id;
    QString label;
    VideoTiming timing;
    double width = .29;
    double opacity = .96;
    double horizontal = .055;
    double vertical = .84;
    bool backdrop = false;
    double backdropScale = 1.;
    double backdropX = .5;
    double backdropY = .5;
};
}
