#include "trajectory_image.h"

#include <QCoreApplication>
#include <QDir>
#include <QPainter>
#include <QTemporaryDir>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
bool transparent(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).alpha()) return false;
    return !image.isNull();
}
QImage background(int exposure = 0)
{
    QImage image(96, 64, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(35 + x / 3 + exposure, 50 + y / 3 + exposure, 70 + (x+y) / 8 + exposure));
    return image;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const auto plate = background();
        const auto noReference = replay::extractTrajectoryForeground(plate, {});
        require(!noReference.error.isEmpty() && transparent(noReference.sprite), "Missing reference became an opaque first-frame crop");
        const auto noDifference = replay::extractTrajectoryForeground(plate, plate);
        require(!noDifference.error.isEmpty() && transparent(noDifference.sprite), "No foreground difference became a successful opaque crop");

        auto source = background(22);
        {
            QPainter painter(&source);
            painter.setPen(QPen(QColor(220, 110, 40), 5));
            painter.drawLine(29, 18, 66, 45);
            painter.drawLine(29, 45, 66, 18);
            painter.setBrush(QColor(230, 180, 30));
            painter.drawEllipse(QPoint(48, 32), 8, 6);
        }
        const auto found = replay::extractTrajectoryForeground(source, plate);
        require(found.error.isEmpty(), "Exposure-compensated foreground extraction failed");
        require(found.foregroundFraction > .04 && found.foregroundFraction < .3, "Mask area does not match the test subject");
        require(found.sprite.pixelColor(48, 32).alpha() == 255 && found.sprite.pixelColor(2, 2).alpha() == 0,
                "Mask lost the subject or retained the crop background");
        require(found.sprite.pixelColor(48, 32).red() == source.pixelColor(48, 32).red(), "Extraction altered source foreground color");

        // A mouse click arrives as repeated identical press/release points.
        // It must paint the same precise source-pixel dot as a single point.
        replay::TrajectoryFrame manualFrame;
        manualFrame.sourceCrop = source;
        manualFrame.sprite = QImage(source.size(), QImage::Format_RGBA8888);
        manualFrame.sprite.fill(Qt::transparent);
        replay::TrajectoryMaskStroke dot;
        dot.radius = .005;
        dot.points = {{20./(source.width()-1), 10./(source.height()-1)}};
        replay::TrajectoryFrameEdit singleDot;
        singleDot.maskStrokes = {dot};
        const auto singleDotImage = replay::trajectorySprite(manualFrame, singleDot);
        require(replay::trajectoryAlphaPixels(singleDotImage) > 0 && replay::trajectoryAlphaPixels(singleDotImage) <= 9,
                "Small manual brush did not paint a localized source-pixel dot");
        auto repeatedDot = singleDot;
        repeatedDot.maskStrokes.front().points.append(dot.points.front());
        repeatedDot.maskStrokes.front().points.append(dot.points.front());
        require(replay::trajectorySprite(manualFrame, repeatedDot) == singleDotImage,
                "A stationary press/release stroke lost its manual repair dot");
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x)
            if (singleDotImage.pixelColor(x,y).alpha())
                require(singleDotImage.pixelColor(x,y).rgb() == source.pixelColor(x,y).rgb(),
                        "Manual edge repair changed the original source RGB");
        manualFrame.sprite = singleDotImage;
        auto eraseDot = singleDot;
        eraseDot.maskStrokes.front().erase = true;
        auto repeatedErase = repeatedDot;
        repeatedErase.maskStrokes.front().erase = true;
        const auto erasedDotImage = replay::trajectorySprite(manualFrame, eraseDot);
        require(replay::trajectorySprite(manualFrame, repeatedErase) == erasedDotImage &&
                replay::trajectoryAlphaPixels(erasedDotImage) < replay::trajectoryAlphaPixels(singleDotImage),
                "Stationary eraser click lost its precise source-pixel effect");
        for (int x = 0; x < source.width(); ++x)
            require(found.sprite.pixelColor(x, 0).alpha() == 0 && found.sprite.pixelColor(x, source.height()-1).alpha() == 0,
                    "Transparent border became an opaque crop boundary");

        // A target with colored body, connected neutral thin arms and hollow
        // guards. The detached chair and net both overlap the old central area,
        // so a bounding-box/interior test would incorrectly retain them.
        QImage detailedPlate(160, 120, QImage::Format_RGB32);
        detailedPlate.fill(QColor(62, 65, 70));
        auto detailedSource = detailedPlate;
        {
            QPainter painter(&detailedSource);
            painter.fillRect(QRect(25, 40, 15, 40), QColor(145, 147, 150));
            painter.setPen(QPen(QColor(180, 182, 184), 1));
            painter.drawLine(47, 25, 47, 97);
            painter.setPen(QPen(QColor(190, 192, 194), 2));
            painter.drawLine(72, 42, 108, 78);
            painter.drawLine(72, 78, 108, 42);
            for (const auto &center : {QPoint(65, 35), QPoint(115, 35), QPoint(65, 85), QPoint(115, 85)})
                painter.drawEllipse(center, 10, 10);
            painter.fillRect(QRect(82, 54, 17, 13), QColor(225, 120, 35));
        }
        detailedSource.setPixelColor(90, 60, detailedPlate.pixelColor(90, 60));
        QImage seeds(detailedPlate.size(), QImage::Format_Grayscale8);
        seeds.fill(0);
        for (int y = 55; y < 66; ++y) for (int x = 83; x < 98; ++x) seeds.scanLine(y)[x] = 255;
        const auto generic = replay::extractTrajectoryForeground(detailedSource, detailedPlate);
        require(generic.error.isEmpty() && generic.sprite.pixelColor(30, 55).alpha() > 128 &&
                generic.sprite.pixelColor(47, 60).alpha() > 32,
                "Detached-background fixture no longer exercises the central-area false positive");
        const auto seeded = replay::extractTrajectoryForeground(detailedSource, detailedPlate, {}, seeds);
        require(seeded.error.isEmpty(), "Reliable target seeds failed to produce a foreground");
        require(seeded.sprite.pixelColor(88, 60).alpha() > 128 && seeded.sprite.pixelColor(80, 50).alpha() > 128 &&
                seeded.sprite.pixelColor(55, 35).alpha() > 128,
                "Target seeds lost the colored body, connected neutral thin arm or rotor guard");
        require(seeded.sprite.pixelColor(30, 55).alpha() == 0 && seeded.sprite.pixelColor(47, 60).alpha() == 0,
                "Target seeds retained detached chair or net pixels");
        require(seeded.sprite.pixelColor(65, 35).alpha() == 0 && seeded.sprite.pixelColor(90, 60).alpha() == 0,
                "Morphological closing or feathering filled a rotor opening or single-pixel frame gap");
        require(seeded.sprite.pixelColor(88, 60).red() == detailedSource.pixelColor(88, 60).red() &&
                seeded.sprite.pixelColor(80, 50).green() == detailedSource.pixelColor(80, 50).green(),
                "Seed filtering altered the original colored body or neutral frame RGB");
        QImage noSeeds(seeds.size(), QImage::Format_Grayscale8); noSeeds.fill(0);
        const auto missingSeeds = replay::extractTrajectoryForeground(detailedSource, detailedPlate, {}, noSeeds);
        require(!missingSeeds.error.isEmpty() && transparent(missingSeeds.sprite), "Absent observed seeds fell back to an arbitrary central region");
        noSeeds.scanLine(10)[10] = 255; noSeeds.scanLine(10)[11] = 255; noSeeds.scanLine(10)[12] = 255;
        const auto backgroundSeeds = replay::extractTrajectoryForeground(detailedSource, detailedPlate, {}, noSeeds);
        require(!backgroundSeeds.error.isEmpty() && transparent(backgroundSeeds.sprite), "Unchanged background seeds selected unrelated foreground");
        const auto wrongSeeds = replay::extractTrajectoryForeground(detailedSource, detailedPlate, {}, QImage(12, 12, QImage::Format_Grayscale8));
        require(!wrongSeeds.error.isEmpty() && transparent(wrongSeeds.sprite), "Wrong-sized target seeds were accepted");
        if (argc == 2) {
            const QString evidence = QString::fromLocal8Bit(argv[1]);
            require(QDir().mkpath(evidence), "Cannot create synthetic fixture evidence directory");
            require(detailedSource.save(QDir(evidence).filePath("synthetic-source.png")) &&
                    seeds.save(QDir(evidence).filePath("synthetic-seeds.png")) &&
                    generic.sprite.save(QDir(evidence).filePath("synthetic-generic.png")) &&
                    seeded.sprite.save(QDir(evidence).filePath("synthetic-seeded.png")), "Cannot save synthetic fixture evidence");
        }

        QImage coverage(plate.size(), QImage::Format_Grayscale8);
        coverage.fill(255);
        for (int y = 20; y < 45; ++y)
            for (int x = 28; x < 68; ++x) coverage.scanLine(y)[x] = 0;
        const auto occluded = replay::extractTrajectoryForeground(source, plate, coverage);
        require(!occluded.error.isEmpty() && transparent(occluded.sprite), "Occluded reference was accepted as clean background");
        require(!replay::extractTrajectoryForeground(source, QImage(12, 12, QImage::Format_RGB32)).error.isEmpty(),
                "Reference dimensions were not validated");

        auto wrong = plate;
        for (int y = 0; y < wrong.height(); ++y)
            for (int x = 0; x < wrong.width(); ++x)
                wrong.setPixelColor(x, y, (x+y)%2 ? Qt::white : Qt::black);
        const auto mismatch = replay::extractTrajectoryForeground(source, wrong);
        require(!mismatch.error.isEmpty() && transparent(mismatch.sprite), "Mismatched background produced a successful mask");

        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create a temporary stationary-target video");
        const auto neutralFile = temporary.filePath("neutral-moving-target.avi");
        cv::VideoWriter neutralWriter(neutralFile.toStdString(), cv::VideoWriter::fourcc('M','J','P','G'), 12., {160, 120});
        require(neutralWriter.isOpened(), "Cannot write the neutral moving-target fixture");
        for (int i = 0; i < 24; ++i) {
            cv::Mat frame(120, 160, CV_8UC3, cv::Scalar(35, 35, 35));
            const cv::Point center(45 + 3*i, 60);
            for (int sx : {-1, 1}) for (int sy : {-1, 1}) {
                const cv::Point rotor = center + cv::Point(13*sx, 13*sy);
                cv::line(frame, center, rotor, cv::Scalar(195, 195, 195), 2);
                cv::circle(frame, rotor, 5, cv::Scalar(225, 225, 225), 2);
            }
            cv::rectangle(frame, cv::Rect(center.x-6, center.y-4, 12, 8), cv::Scalar(170, 170, 170), cv::FILLED);
            neutralWriter.write(frame);
        }
        neutralWriter.release();
        const auto neutralTrack = replay::trackTrajectory(neutralFile, 0., 1.75, {24./160, 39./120, 42./160, 42./120});
        require(neutralTrack.frames.size() >= 18 && !neutralTrack.lostAt, "Unsaturated moving target lost its existing generic tracking support");
        const auto neutralVisible = std::count_if(neutralTrack.frames.cbegin(), neutralTrack.frames.cend(), [](const auto &frame) {
            return frame.foregroundError.isEmpty() && frame.alphaPixels > 0;
        });
        require(neutralVisible >= 18, "Unsaturated moving target was incorrectly rejected for lacking an HSV appearance peak");
        const auto file = temporary.filePath("stationary-target.avi");
        cv::VideoWriter writer(file.toStdString(), cv::VideoWriter::fourcc('M','J','P','G'), 12., {160, 120});
        require(writer.isOpened(), "Cannot write the stationary-target fixture");
        cv::Mat scene(120, 160, CV_8UC3, cv::Scalar(35, 55, 65));
        cv::RNG random(6);
        random.fill(scene(cv::Rect(62, 48, 24, 20)), cv::RNG::UNIFORM, 90, 230);
        for (int i = 0; i < 10; ++i) writer.write(scene);
        writer.release();
        const auto track = replay::trackTrajectory(file, 0., .5, {62./160, 48./120, 24./160, 20./120});
        require(!track.frames.empty() && !track.backgroundWarning.isEmpty(), "Unobserved background approximation was not reported");
        for (const auto &frame : track.frames)
            require(!frame.foregroundError.isEmpty() && transparent(frame.sprite), "Stationary occluded target became a successful rectangular sprite");
        require(!replay::renderTrajectory(track, {0}, 1., {}).isNull(), "Reported failed extraction became a missing-cache error during rendering");
        replay::TrajectoryCompositionStats counts;
        replay::renderTrajectory(track, {0}, 1., {}, {}, {}, {}, false, &counts);
        require(counts.composed == 0 && counts.failed == 1 && !counts.exportError({}).isEmpty(), "Fully transparent extraction passed the visible-foreground gate");
        bool refused = false;
        try { replay::renderTrajectory(track, {0}, 1., {}, {}, {}, {}, false, nullptr, true); }
        catch (const std::exception &) { refused = true; }
        require(refused, "Background-only image was exported as a successful UAV composite");
        replay::TrajectoryEdits corrected;
        corrected.frames[replay::trajectoryTimestamp(track.frames[0].seconds)].maskStrokes.push_back({{QPointF(.4, .4), QPointF(.6, .6)}, .06, false});
        replay::renderTrajectory(track, {0}, 1., {}, {}, corrected, {}, false, &counts, true);
        require(counts.composed == 1 && counts.failed == 0, "Manual original-pixel mask did not restore a visible instance");
        require(replay::TrajectoryEdits::fromJson(corrected.toJson()) == corrected, "Manual mask editing was not preserved losslessly");
        auto erased = corrected;
        erased.frames.begin()->maskStrokes.push_back({{QPointF(.5, .5)}, .5, true});
        replay::renderTrajectory(track, {0}, 1., {}, {}, erased, {}, false, &counts);
        require(counts.composed == 0 && !counts.exportError(erased).isEmpty(), "Erasing all mask pixels kept an exportable empty instance");
        replay::FixedCameraTrajectory mixed = track;
        mixed.frames[0].sprite = found.sprite; mixed.frames[0].alphaPixels = replay::trajectoryAlphaPixels(found.sprite);
        mixed.frames[0].foregroundError.clear();
        replay::renderTrajectory(mixed, {0, 1}, 1., {}, {}, {}, {}, false, &counts);
        require(counts.composed == 1 && counts.failed == 1 && !counts.exportError({}).isEmpty(), "Invalid samples were silently skipped without user choice");
        replay::TrajectoryEdits skip; skip.skipInvalid = true;
        require(counts.exportError(skip).isEmpty(), "Explicit skip-invalid did not permit a valid nonempty composite");
        replay::renderTrajectory(mixed, {0}, 0., {}, {}, skip, {}, false, &counts);
        require(counts.composed == 0 && !counts.exportError(skip).isEmpty(), "Zero opacity incorrectly counted as visible foreground");
        std::cout << "Foreground masks: observed target seeds reject detached chair/net; connected neutral frame and real openings retained; unsaturated video tracking, transparency, exposure compensation and reference failures passed. Synthetic fixtures only.\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
