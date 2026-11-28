#include "video_reader.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
QByteArray run(const QString &program, const QStringList &args)
{
    QProcess process;
    process.start(program, args);
    require(process.waitForStarted(5000) && process.waitForFinished(20000), "Fixture process timed out");
    if (process.exitCode() != 0) throw std::runtime_error(process.readAllStandardError().toStdString());
    return process.readAllStandardOutput();
}
void matchesPixels(const replay::VideoReader &reader, const QByteArray &pixels, qsizetype index, int width, int height)
{
    require(reader.frame.cols == width && reader.frame.rows == height, "Wrong source display dimensions");
    const auto stride = width * 3;
    const qsizetype offset = index * stride * height;
    require(offset >= 0 && offset + stride * height <= pixels.size(), "Independent pixel fixture is incomplete");
    int maximum = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < stride; ++x)
        maximum = std::max(maximum, std::abs(int(reader.frame.ptr<uchar>(y)[x]) - int(uchar(pixels[offset + y * stride + x]))));
    require(maximum <= 2, "Selected image differs from the independent FFmpeg presentation frame");
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        const auto ffprobe = QStandardPaths::findExecutable("ffprobe");
        require(!ffmpeg.isEmpty() && !ffprobe.isEmpty(), "FFmpeg and FFprobe are required for timestamp regression");
        QTemporaryDir directory;
        const auto source = directory.filePath("VFR source.mp4");
        run(ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=30:duration=3",
            "-vf", "setpts='if(lt(N,30),N,30+2*(N-30))/(30*TB)+5/TB'", "-fps_mode", "vfr",
            "-c:v", "libx264", "-crf", "0", "-g", "30", "-bf", "3", "-video_track_timescale", "90000", source});
        const auto probe = QJsonDocument::fromJson(run(ffprobe, {"-v", "error", "-select_streams", "v:0",
            "-show_frames", "-show_streams", "-show_entries", "frame=pts,pict_type:stream=time_base,start_pts,duration_ts", "-of", "json", source})).object();
        const auto frames = probe.value("frames").toArray();
        const auto stream = probe.value("streams").toArray().first().toObject();
        require(frames.size() == 90 && stream.value("start_pts").toInteger() > 0, "Fixture needs 90 frames and a nonzero origin");
        const auto base = stream.value("time_base").toString().split('/');
        const int numerator = base[0].toInt(), denominator = base[1].toInt();
        QList<qint64> pts, nanoseconds;
        const auto origin = frames.first().toObject().value("pts").toInteger();
        bool bFrame = false;
        for (const auto &value : frames) {
            const auto object = value.toObject();
            const auto stamp = object.value("pts").toInteger();
            pts.append(stamp);
            nanoseconds.append(qint64(std::llround(double(stamp - origin) * numerator / denominator * 1.e9)));
            bFrame |= object.value("pict_type").toString() == "B";
        }
        // Lossless x264 may suppress B frames; a separate lossy fixture below forces them.
        const auto pixels = run(ffmpeg, {"-v", "error", "-i", source, "-fps_mode", "passthrough",
            "-sws_flags", "bicubic+accurate_rnd", "-f", "rawvideo", "-pix_fmt", "bgr24", "pipe:1"});
        constexpr qsizetype FrameBytes = 128 * 96 * 3;
        require(pixels.size() == FrameBytes * frames.size(), "Independent decoder frame count mismatch");
        replay::VideoReader sequential(source);
        require(sequential.originPts() == origin && sequential.timeBaseNumerator() == numerator && sequential.timeBaseDenominator() == denominator,
            "Source PTS origin/time_base changed");
        for (int i = 0; i < frames.size(); ++i) {
            require(sequential.advance() && sequential.index() == i, "Sequential ordinal mismatch");
            require(sequential.pts() == pts[i] && sequential.timestampNs() == nanoseconds[i] && sequential.hasSourcePts(), "Frame PTS differs from FFprobe");
            matchesPixels(sequential, pixels, i, 128, 96);
        }
        require(!sequential.advance(), "EOF should drain exactly once");
        const auto matroska = directory.filePath("nonzero.mkv");
        run(ffmpeg, {"-v", "error", "-y", "-copyts", "-i", source, "-c", "copy", matroska});
        replay::VideoReader mkv(matroska);
        const auto mkvProbe = QJsonDocument::fromJson(run(ffprobe, {"-v", "error", "-select_streams", "v:0",
            "-show_frames", "-show_entries", "frame=pts,duration:stream=time_base", "-of", "json", matroska})).object();
        const auto mkvFrames = mkvProbe.value("frames").toArray();
        const auto mkvBase = mkvProbe.value("streams").toArray().first().toObject().value("time_base").toString().split('/');
        const auto mkvFirst = mkvFrames.first().toObject().value("pts").toInteger();
        const auto mkvLast = mkvFrames.last().toObject();
        const auto mkvEnd = mkvLast.value("pts").toInteger() + mkvLast.value("duration").toInteger();
        const double expectedMkvDuration = double(mkvEnd - mkvFirst) * mkvBase[0].toDouble() / mkvBase[1].toDouble();
        // Remuxing can change the final packet duration. Compare to this container's
        // independent frame timestamps rather than assuming the MP4 endpoint survives.
        require(std::abs(mkv.duration() - expectedMkvDuration) < .002 && mkv.originPts() == mkvFirst && mkv.select(3.5),
            "Matroska track duration or origin differs from FFprobe");
        replay::VideoReader random(source);
        const QList<qint64> queries{3'500'000'000, 1'250'000'000, 0, 4'700'000'000, 333'333'333, 1'000'000'000, 2'166'666'667, 20'000'000, 4'900'000'000};
        for (const auto target : queries) {
            const auto selected = std::upper_bound(nanoseconds.begin(), nanoseconds.end(), target) - nanoseconds.begin() - 1;
            require(random.selectNanoseconds(target), "Random timestamp selection failed");
            require(random.pts() == pts[selected] && random.timestampNs() <= target, "Seek selected an incorrect presentation timestamp");
            matchesPixels(random, pixels, selected, 128, 96);
        }
        require(!random.select(-1) && !random.select(100) && !random.select(0, [] { return true; }), "Invalid/cancelled requests must fail");
        require(random.jump(37) && random.index() == 37 && random.pts() == pts[37], "Explicit ordinal jump is inaccurate");
        const auto rotated = directory.filePath("rotated.mp4");
        run(ffmpeg, {"-v", "error", "-y", "-display_rotation:v:0", "90", "-i", source, "-c", "copy", rotated});
        replay::VideoReader rotation(rotated);
        require(rotation.select(0) && rotation.frame.cols == 96 && rotation.frame.rows == 128, "Display matrix rotation lost");
        const auto reordered = directory.filePath("bframes.mp4");
        run(ffmpeg, {"-v", "error", "-y", "-i", source, "-c:v", "libx264", "-bf", "3", "-b_strategy", "0", "-crf", "18", "-fps_mode", "vfr", reordered});
        const auto bProbe = QJsonDocument::fromJson(run(ffprobe, {"-v", "error", "-show_frames", "-show_entries", "frame=pts,pict_type", "-of", "json", reordered})).object().value("frames").toArray();
        replay::VideoReader bReader(reordered);
        bFrame = false;
        for (const auto &value : bProbe) {
            const auto object = value.toObject();
            bFrame |= object.value("pict_type").toString() == "B";
            require(bReader.advance() && bReader.pts() == object.value("pts").toInteger(), "B frame presentation order differs from FFprobe");
        }
        require(bFrame && !bReader.advance(), "B frame fixture/flush regression");
        const auto bPixels = run(ffmpeg, {"-v", "error", "-i", reordered, "-fps_mode", "passthrough",
            "-sws_flags", "bicubic+accurate_rnd", "-f", "rawvideo", "-pix_fmt", "bgr24", "pipe:1"});
        QList<qint64> bNanoseconds;
        for (const auto &value : bProbe) bNanoseconds.append(qint64(std::llround(
            double(value.toObject().value("pts").toInteger() - bReader.originPts()) * bReader.timeBaseNumerator() / bReader.timeBaseDenominator() * 1.e9)));
        replay::VideoReader bRandom(reordered);
        for (const auto target : queries) {
            const auto selected = std::upper_bound(bNanoseconds.begin(), bNanoseconds.end(), target) - bNanoseconds.begin() - 1;
            require(selected >= 0 && bRandom.selectNanoseconds(target) && bRandom.pts() == bProbe[selected].toObject().value("pts").toInteger(),
                "Random B-frame seek selected incorrect PTS");
            matchesPixels(bRandom, bPixels, selected, 128, 96);
        }
        bool constructorRejected = false;
        int cancellationChecks = 0;
        try { replay::VideoReader cancelled(directory.filePath("never-opened.mp4"), [&] { ++cancellationChecks; return true; }); }
        catch (const std::runtime_error &error) { constructorRejected = QString::fromUtf8(error.what()).contains("cancelled"); }
        require(constructorRejected && cancellationChecks > 0, "Pre-cancelled constructor attempted source I/O");
        std::atomic_bool lifetimeCancelled = false, requestCancelled = false;
        replay::VideoReader cancellable(source, [&] { return lifetimeCancelled.load(); });
        require(cancellable.select(0., [&] { return requestCancelled.load(); }), "Initial cancellable selection failed");
        requestCancelled = true;
        require(cancellable.select(0.), "Temporary request cancellation leaked into the next selection");
        lifetimeCancelled = true;
        require(!cancellable.select(0.) && !cancellable.advance(), "Selection cleared constructor lifetime cancellation");
        const auto anamorphic = directory.filePath("anamorphic.mp4");
        run(ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=144x96:rate=30:duration=0.2",
            "-vf", "setsar=4/3", "-c:v", "libx264", "-crf", "0", anamorphic});
        replay::VideoReader sar(anamorphic);
        require(sar.select(0.) && sar.codedSize() == QSize(144, 96) && sar.sampleAspectNumerator() == 4 &&
            sar.sampleAspectDenominator() == 3 && sar.displayRotation() == 0, "Sample aspect provenance lost");
        const auto sarPixels = run(ffmpeg, {"-v", "error", "-i", anamorphic,
            "-vf", "scale=192:96:flags=bicubic+accurate_rnd,setsar=1", "-frames:v", "1", "-pix_fmt", "bgr24", "-f", "rawvideo", "pipe:1"});
        matchesPixels(sar, sarPixels, 0, 192, 96);
        const auto sarRotated = directory.filePath("anamorphic-rotated.mp4");
        run(ffmpeg, {"-v", "error", "-y", "-display_rotation:v:0", "90", "-i", anamorphic, "-c", "copy", sarRotated});
        replay::VideoReader rotatedSar(sarRotated);
        require(rotatedSar.select(0.) && rotatedSar.frame.cols == 96 && rotatedSar.frame.rows == 192 &&
            rotatedSar.codedSize() == QSize(144, 96) && rotatedSar.sampleAspectNumerator() == 4 &&
            rotatedSar.sampleAspectDenominator() == 3 && rotatedSar.displayRotation() % 180 == 90,
            "Sample aspect normalization must precede display rotation");
        const auto missing = directory.filePath("missing.h264");
        run(ffmpeg, {"-v", "error", "-y", "-i", source, "-c:v", "copy", "-bsf:v", "h264_mp4toannexb", "-f", "h264", missing});
        bool rejected = false;
        try { replay::VideoReader noPts(missing); } catch (const std::runtime_error &) { rejected = true; }
        require(rejected, "Timestamp-free source must not silently fall back to FPS");
        std::cout << "PTS/time_base, VFR, nonzero origin, random B-frame seeks/pixels, SAR/rotation, lifetime/request cancellation and missing PTS verified.\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
