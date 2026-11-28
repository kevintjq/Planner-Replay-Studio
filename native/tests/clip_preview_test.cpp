#include "clip_preview.h"
#include "video_reader.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void wait(const std::function<bool()> &done)
{
    QElapsedTimer deadline; deadline.start();
    while (!done() && deadline.elapsed() < 7000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    require(done(), "Asynchronous preview did not settle");
}
void fixture(const QString &path, const QString &filter, const QString &suffix = {})
{
    QProcess process;
    process.start(QStandardPaths::findExecutable("ffmpeg"), {"-v", "error", "-y", "-f", "lavfi", "-i", filter,
        "-vf", "setpts='if(lt(N,30),N,30+2*(N-30))/(30*TB)+5/TB'" + suffix, "-fps_mode", "vfr", "-c:v", "libx264", "-crf", "0", path});
    require(process.waitForStarted() && process.waitForFinished(10000) && process.exitCode() == 0, "Cannot generate preview fixture");
}
}
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        const auto source = directory.filePath(QString::fromUtf8("源视频.mp4"));
        fixture(source, "testsrc2=size=128x96:rate=30:duration=3");
        int uiTicks = 0;
        QTimer heartbeat; heartbeat.setInterval(1);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++uiTicks; }); heartbeat.start();
        ClipPreview item; item.setWidth(128); item.setHeight(96);
        item.setSource(QUrl::fromLocalFile(source)); item.setSourceSeconds(3.125);
        wait([&] { return item.ready() && !item.busy(); });
        replay::VideoReader reference(source);
        require(reference.select(3.125) && std::abs(item.actualSeconds() - reference.seconds) < 1.e-9 && item.sourcePts() == QString::number(reference.pts()), "Preview and export selected different PTS");
        QImage actual(128, 96, QImage::Format_ARGB32); actual.fill(Qt::transparent);
        QPainter painter(&actual); item.paint(&painter); painter.end();
        require(actual == replay::imageOf(reference.frame).convertToFormat(QImage::Format_ARGB32), "Preview pixels differ from the source-quality selector");
        item.setSourceSeconds(4.75); QCoreApplication::processEvents();
        for (double time : {1.7, 0.1, 4., .4, 2.125}) item.setSourceSeconds(time);
        wait([&] { return !item.busy() && reference.select(2.125) && item.sourcePts() == QString::number(reference.pts()); });
        require(uiTicks > 2 && item.error().isEmpty(), "Preview blocked the event loop or produced a decode error");
        const auto other = directory.filePath("replacement.mp4");
        fixture(other, "color=c=blue:size=128x96:rate=30:duration=3");
        item.setSourceSeconds(4.75); QCoreApplication::processEvents();
        item.setSource(QUrl::fromLocalFile(other)); item.setSourceSeconds(.25);
        wait([&] { return item.ready() && !item.busy(); });
        actual.fill(Qt::transparent); painter.begin(&actual); item.paint(&painter); painter.end();
        require(actual.pixelColor(50, 50).blue() > 240 && actual.pixelColor(50, 50).red() < 10, "Old-source job overwrote a replacement source");
        item.setPrecise(false); item.setSourceSeconds(.4); QCoreApplication::processEvents();
        const auto retainedPts = item.sourcePts();
        item.setDecodingEnabled(false);
        wait([&] { return !item.busy(); });
        require(item.sourcePts() == retainedPts, "Disabled preview applied an in-flight continuous-playback frame");
        item.setSourceSeconds(2.125); item.setDecodingEnabled(true);
        replay::VideoReader replacement(other); require(replacement.select(2.125), "Replacement reference failed");
        wait([&] { return !item.busy() && item.sourcePts() == QString::number(replacement.pts()); });
        const auto anamorphic = directory.filePath("anamorphic.mp4");
        fixture(anamorphic, "testsrc2=size=144x96:rate=30:duration=3", ",setsar=4/3");
        item.setSource(QUrl::fromLocalFile(anamorphic));
        wait([&] { return item.ready() && !item.busy(); });
        require(item.imageSize() == QSize(192, 96), "Preview ignored source sample aspect ratio");
        item.setSource(QUrl::fromLocalFile(directory.filePath("missing.mp4")));
        wait([&] { return !item.busy() && !item.error().isEmpty(); });
        require(!item.ready(), "A missing source retained a misleading old video frame");
        std::cout << "Exact preview PTS/pixels, VFR scrubs, source replacement, cancellation/resume, SAR and responsive event loop verified.\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
