#include "trajectory_controller.h"
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>
#include <opencv2/videoio.hpp>
#include <iostream>
#include <stdexcept>
#include <cmath>

void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(TrajectoryController &controller) {
    if (!controller.working()) return true;
    QEventLoop loop; bool completed = false;
    auto connection = QObject::connect(&controller, &TrajectoryController::changed, &loop, [&] {
        if (!controller.working()) { completed = true; loop.quit(); }
    });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit); loop.exec(); QObject::disconnect(connection); return completed;
}
QJsonObject manifest(const QString &directory) {
    QFile file(QDir(directory).filePath("manifest.json"));
    expect(file.open(QIODevice::ReadOnly), "Missing manifest");
    const auto result = QJsonDocument::fromJson(file.readAll()).object();
    expect(!result.isEmpty(), "Invalid manifest"); return result;
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        const auto input = directory.filePath("source.avi");
        cv::VideoWriter writer(input.toStdString(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 12., {240, 160});
        expect(writer.isOpened(), "Cannot create video fixture");
        for (int i = 0; i < 24; ++i) writer.write(cv::Mat(160, 240, CV_8UC3, i < 12 ? cv::Scalar(255, 0, 0) : cv::Scalar(0, 0, 255)));
        writer.release();
        PreviewImages images; TrajectoryController controller(images);
        controller.open(QUrl::fromLocalFile(input)); expect(waitFor(controller), "Initial decode timed out");
        controller.preview(1.25); expect(waitFor(controller), "Time selection timed out");
        const auto preview = controller.image(); const auto settings = controller.settings();
        bool exported = false, success = false;
        QObject::connect(&controller, &TrajectoryController::frameExported, &app, [&](bool ok, const QString &) { exported = true; success = ok; });
        for (const auto &format : {"png", "tiff", "pdf", "svg"}) {
            const auto path = directory.filePath(QString("frame.%1").arg(format));
            exported = false;
            controller.exportFrame(QUrl::fromLocalFile(path), {{"width", 3840}, {"height", 2160}, {"dpi", 600}, {"background", "transparent"}});
            expect(waitFor(controller) && exported && success, "Frame export failed");
            expect(controller.image() == preview && controller.settings() == settings && !controller.hasResult(), "Export changed preview or edits");
            if (QString(format) == "png" || QString(format) == "tiff") {
                const QImage image(path);
                expect(image.size() == QSize(3840, 2160) && image.pixelColor(0, 0).alpha() == 0 && image.pixelColor(1920, 1080).red() > 200,
                    "Wrong dimensions, alpha or experiment-time source frame");
                expect(std::abs(image.dotsPerMeterX() * .0254 - 600) < .1, "DPI changed");
                if (QString(format) == "png") expect(image.text("Description").contains("1.250000000"), "Wrong timestamp");
            } else expect(QFileInfo(path).size() > 1000, "Empty document");
        }
        exported = false; controller.exportFrame(QUrl::fromLocalFile(directory.filePath("invalid.png")), {{"width", 100000}});
        expect(exported && !success && !QFile::exists(directory.filePath("invalid.png")), "Unsafe dimensions accepted");
        bool batchDone = false, batchOk = false;
        QObject::connect(&controller, &TrajectoryController::batchExported, &app, [&](bool ok, const QString &) { batchDone = true; batchOk = ok; });
        QVariantMap options{{"width", 640}, {"height", 360}, {"dpi", 600}, {"background", "transparent"}, {"mode", "manual"},
            {"times", "1.25, .27, .25"}, {"format", "png"}, {"figure", true}, {"figureFormat", "svg"}, {"columns", 2}};
        controller.exportFrames(QUrl::fromLocalFile(directory.path()), options);
        expect(controller.working(), "Batch did not lock editor");
        controller.preview(0.); controller.open(QUrl::fromLocalFile(input)); controller.configure(.7, .3);
        controller.undo(); controller.redo(); controller.resetPreview();
        expect(controller.image() == preview && controller.settings() == settings, "Programmatic edit mutated active export snapshot");
        expect(waitFor(controller) && batchDone && batchOk, "Source batch failed");
        const auto batch = manifest(controller.exportDirectory());
        expect(batch["status"] == "complete" && batch["completed"].toArray().size() == 3, "Incomplete source manifest");
        const auto frames = batch["completed"].toArray();
        for (int i = 0; i < frames.size(); ++i) {
            const auto entry = frames[i].toObject(); const QImage image(QDir(controller.exportDirectory()).filePath(entry["file"].toString()));
            expect(image.size() == QSize(640, 360) && image.pixelColor(0, 0).alpha() == 0, "Batch dimensions or alpha changed");
            expect(i < 2 ? image.pixelColor(320, 180).blue() > 200 : image.pixelColor(320, 180).red() > 200, "Batch used preview time for source frames");
            expect(std::abs(entry["source_frame_time"].toDouble() - (i < 2 ? .25 : 1.25)) < 1.e-8, "Wrong selected decoder time");
        }
        expect(std::abs(frames[1].toObject()["requested_time"].toDouble() - .27) < 1.e-8, "Requested time was replaced by frame time");
        QFile figure(QDir(controller.exportDirectory()).filePath("figure.svg"));
        expect(figure.open(QIODevice::ReadOnly), "Figure missing"); const auto svg = figure.readAll();
        expect(svg.contains("<image") && svg.contains("<text") && svg.contains("(a)") && svg.contains("1.250000000"), "Missing raster panels or real vector captions");
        expect(controller.image() == preview && controller.settings() == settings, "Batch changed editor state");

        auto track = std::make_shared<replay::FixedCameraTrajectory>();
        track->background = QImage(240, 160, QImage::Format_ARGB32_Premultiplied); track->background.fill(Qt::black);
        QImage sprite(10, 10, QImage::Format_ARGB32_Premultiplied); sprite.fill(Qt::red);
        for (int i = 0; i < 4; ++i) track->frames.push_back({i * .5, QRect(10 + 50 * i, 70, 10, 10), sprite, {}});
        replay::TrajectoryEdits edits; edits.showLine = true;
        edits.ranges = {{"excluded", replay::trajectoryTimestamp(.5), replay::trajectoryTimestamp(.75), replay::Both}};
        edits.frames[replay::trajectoryTimestamp(1.)].opacity = .5;
        QVariantMap tracked{{"content", "trajectory"}, {"mode", "manual"}, {"times", ".25, 1, 1.5"},
            {"width", 240}, {"height", 160}, {"background", "white"}};
        const auto snapshots = replay::exportTrajectoryBatch(input, directory.path(), track, {0, 1, 2, 3}, edits, 1., 1.25, tracked);
        expect(snapshots.error.isEmpty() && snapshots.completed == 3, "Trajectory snapshot batch failed");
        const auto snapshotsManifest = manifest(snapshots.directory)["completed"].toArray();
        for (int i = 0; i < 3; ++i) {
            const QImage image(QDir(snapshots.directory).filePath(snapshotsManifest[i].toObject()["file"].toString()));
            expect(image.pixelColor(15, 75).red() > 240 && image.pixelColor(65, 75) == QColor(Qt::black), "Snapshot lost first frame or ignored typed exclusion");
            expect(i == 0 ? image.pixelColor(115, 75) == QColor(Qt::black) : std::abs(image.pixelColor(115, 75).red() - 128) <= 1, "Future frame leaked or opacity changed");
            expect(i == 2 ? image.pixelColor(165, 75).red() > 240 : image.pixelColor(165, 75) == QColor(Qt::black), "Snapshot cutoff excluded endpoint or included future sprite");
        }
        tracked.insert("times", ".25"); edits.trim = {replay::trajectoryTimestamp(1.), replay::trajectoryTimestamp(1.5)};
        const auto trimmed = replay::exportTrajectoryBatch(input, directory.path(), track, {0, 1, 2, 3}, edits, 1., 1.25, tracked);
        expect(trimmed.error.isEmpty(), "Snapshot before generation trim failed");
        const QImage empty(QDir(trimmed.directory).filePath(manifest(trimmed.directory)["completed"].toArray()[0].toObject()["file"].toString()));
        expect(empty.pixelColor(15, 75) == QColor(Qt::black) && empty.pixelColor(115, 75) == QColor(Qt::black), "Generation trim changed by batch cutoff");

        bool cancel = false;
        options.insert("figure", false); options.insert("mode", "interval"); options.insert("begin", 0.); options.insert("end", 1.5); options.insert("interval", .1);
        const auto cancelled = replay::exportTrajectoryBatch(input, directory.path(), {}, {}, {}, 1., 1.25, options,
            [&] { return cancel; }, [&](double) { cancel = true; });
        expect(cancelled.cancelled && cancelled.completed == 1 && manifest(cancelled.directory)["status"] == "cancelled", "Cancellation did not retain completed files and manifest");
        const auto previousDirectories = QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        options.insert("mode", "manual"); options.insert("times", ".25"); options.insert("pattern", "../unsafe");
        const auto rejected = replay::exportTrajectoryBatch(input, directory.path(), {}, {}, {}, 1., 1.25, options);
        expect(!rejected.error.isEmpty() && rejected.directory.isEmpty() && QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot) == previousDirectories, "Invalid batch created files");
        options.remove("pattern"); options.insert("mode", "samples"); options.insert("times", "3, 15"); options.insert("sampleFps", 12.);
        const auto grid = replay::exportTrajectoryBatch(input, directory.path(), {}, {}, {}, 1., 1.25, options);
        expect(grid.error.isEmpty() && grid.completed == 2, "Sampling-grid batch failed");
        tracked.insert("mode", "tracked");
        const auto actualSamples = replay::exportTrajectoryBatch(input, directory.path(), track, {0, 2, 3}, {}, 1., 1.25, tracked);
        expect(actualSamples.error.isEmpty() && actualSamples.completed == 3, "Actual tracked-sample batch failed");
        track->frames[0].cachedSprite = directory.filePath("missing-sprite.png");
        const auto missing = replay::exportTrajectoryBatch(input, directory.path(), track, {0}, {}, 1., 1.25, tracked);
        expect(!missing.error.isEmpty() && missing.completed == 0 && manifest(missing.directory)["status"] == "failed", "Missing cached pixels silently exported incomplete trajectory");
        options.insert("mode", "interval"); options.insert("begin", 0.); options.insert("end", 1.5); options.insert("interval", .01);
        controller.exportFrames(QUrl::fromLocalFile(directory.path()), options);
        auto restoredSettings = settings; restoredSettings.insert("start", .5);
        controller.restoreSettings(restoredSettings);
        expect(waitFor(controller), "Project change did not cancel active batch");
        expect(waitFor(controller) && std::abs(controller.startTime() - .5) < 1.e-9 && controller.settings() == restoredSettings,
            "Pending project restore was lost or raced the export worker");
        std::cout << "Trajectory 4K formats, source-time batches, vector figure captions, cutoff/exclusion/opacity, cancellation, naming, sampling and editor preservation passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
