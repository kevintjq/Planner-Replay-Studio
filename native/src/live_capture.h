#pragma once

#include <QString>

namespace replay {

struct LiveCaptureOptions {
    quint16 port = 8765;
    QString cloudTopic, odometryTopic;
    double seconds = 6., delay = .3;
};

void captureLiveBag(const QString &destination, const LiveCaptureOptions &options);
}
