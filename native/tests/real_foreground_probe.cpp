#include "trajectory_image.h"

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <iostream>
#include <stdexcept>

// Explicitly supplied real media only. Outputs are private inspection evidence;
// this executable is intentionally not registered as an automatic fixture test.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "inspect") {
        const QImage image(QString::fromLocal8Bit(argv[2]));
        std::array<int, 18> bins{};
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x,y);
            if (color.alpha() >= 128 && color.hsvSaturation() >= 75 && color.value() >= 40 && color.hsvHue() >= 0) ++bins[color.hsvHue()/20];
        }
        for (int i = 0; i < 18; ++i) std::cout << i*20 << ": " << bins[i] << '\n';
        return 0;
    }
    if (argc < 9) {
        std::cerr << "Usage: real_foreground_probe input output-dir start end roi-x roi-y roi-width roi-height [register|fixed] [background-seconds|auto] [project-path]\n";
        return 2;
    }
    try {
        const QString input = QString::fromLocal8Bit(argv[1]), directory = QString::fromLocal8Bit(argv[2]);
        QDir().mkpath(directory);
        auto number = [&](int i) { bool ok = false; const double value = QString::fromLocal8Bit(argv[i]).toDouble(&ok); if (!ok) throw std::runtime_error("Invalid numeric argument."); return value; };
        replay::TrajectoryEdits edits; edits.skipInvalid = true;
        QVariantMap canvasState;
        double opacity = 1.;
        QString projectPath;
        if (argc > 11) {
            projectPath = QString::fromLocal8Bit(argv[11]);
            QFile project(projectPath);
            if (!project.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read inspection project.");
            const auto settings = QJsonDocument::fromJson(project.readAll()).object().value("trajectory_studio").toObject();
            if (settings.isEmpty() || QFileInfo(settings.value("path").toString()).canonicalFilePath().compare(
                    QFileInfo(input).canonicalFilePath(), Qt::CaseInsensitive) != 0)
                throw std::runtime_error("Inspection project does not reference the supplied real source.");
            edits = replay::TrajectoryEdits::fromJson(settings.value("edits").toObject());
            canvasState = settings.value("canvas").toObject().toVariantMap();
            opacity = settings.value("opacity").toDouble(1.);
        }
        replay::TrajectoryTrackingOptions options;
        options.registerCamera = argc > 9 && QString::fromLocal8Bit(argv[9]) == "register";
        if (argc > 10 && QString::fromLocal8Bit(argv[10]) != "auto") options.backgroundSeconds = number(10);
        int last = -1;
        auto track = replay::trackTrajectory(input, number(3), number(4), {number(5), number(6), number(7), number(8)}, {},
            [&](double progress) { const int step = int(progress * 20); if (step != last) { last = step; std::cout << step*5 << "%\n" << std::flush; } }, options);
        const auto samples = replay::sampleTrajectory(track, 1.);
        replay::TrajectoryCompositionStats counts;
        auto composite = replay::renderTrajectory(track, samples, opacity, {}, {}, edits, {}, false, &counts, false, canvasState);
        QJsonArray frames;
        int extracted = 0;
        for (size_t i = 0; i < track.frames.size(); ++i) extracted += track.frames[i].alphaPixels > 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &frame = track.frames[samples[i]];
            const QString stem = QDir(directory).filePath(QString("sample-%1-%2s").arg(i, 3, 10, QChar('0')).arg(frame.seconds, 0, 'f', 6));
            const auto sprite = replay::trajectorySprite(frame, edits.frames.value(replay::trajectoryTimestamp(frame.seconds)));
            replay::trajectorySourceCrop(frame).save(stem + "-source.png"); sprite.save(stem + "-foreground.png");
            QImage checker(sprite.size(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter painter(&checker);
                for (int y = 0; y < checker.height(); y += 8) for (int x = 0; x < checker.width(); x += 8)
                    painter.fillRect(x,y,8,8,((x/8+y/8)%2) ? QColor(190,190,190) : QColor(240,240,240));
                painter.drawImage(QPoint(),sprite);
            }
            checker.save(stem + "-foreground-checker.png");
            QImage mask(sprite.size(), QImage::Format_Grayscale8);
            for (int y = 0; y < sprite.height(); ++y) for (int x = 0; x < sprite.width(); ++x) mask.scanLine(y)[x] = sprite.pixelColor(x, y).alpha();
            mask.save(stem + "-mask.png");
            frames.append(QJsonObject{{"seconds", frame.seconds}, {"alpha_pixels", QString::number(replay::trajectoryAlphaPixels(sprite))},
                {"automatic_alpha_pixels", QString::number(frame.alphaPixels)}, {"foreground_fraction", frame.foregroundFraction}, {"reason", frame.foregroundError},
                {"bounds", QJsonArray{frame.bounds.x(), frame.bounds.y(), frame.bounds.width(), frame.bounds.height()}}});
        }
        composite.save(QDir(directory).filePath("composite-preview.png"));
        track.background.save(QDir(directory).filePath("background.png"));
        const auto gate = counts.exportError(edits);
        if (gate.isEmpty()) {
            replay::renderTrajectory(track, samples, opacity, {}, {3840, 2160}, edits, {}, true, nullptr, true, canvasState).save(QDir(directory).filePath("composite-4k.png"));
            replay::renderTrajectory(track, samples, opacity, {}, {7680, 4320}, edits, {}, true, nullptr, true, canvasState).save(QDir(directory).filePath("composite-8k.png"));
        }
        QJsonObject report{{"source_file", QFileInfo(input).fileName()}, {"tracked", int(track.frames.size())}, {"sampled", counts.sampled}, {"extracted", extracted},
            {"extraction_failed", int(track.frames.size())-extracted}, {"sample_successful", counts.successful}, {"sample_failed", counts.failed},
            {"filtered", counts.filtered}, {"composed", counts.composed}, {"registered", track.registeredFrames}, {"registration_rejected", track.registrationRejected},
            {"background_warning", track.backgroundWarning}, {"export_error", gate}, {"samples", frames},
            {"edits_project",projectPath.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QFileInfo(projectPath).fileName())},
            {"clean_foreground_accepted",false}};
        QFile file(QDir(directory).filePath("report.json"));
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write report.");
        file.write(QJsonDocument(report).toJson());
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
        return gate.isEmpty() ? 0 : 1;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
