#include "replay_data.h"
#include "live_capture.h"
#include "project_document.h"
#include "app_info.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <cmath>
#include <QTextStream>
#include <exception>

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    // The command-line tool must report usage through standard streams on CI and in pipes.
    qputenv("QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES", "1");
#endif
    QCoreApplication app(argc, argv);
    AppInfo appInfo;
    app.setApplicationName(appInfo.executableName() + "-cli");
    app.setApplicationVersion(appInfo.version());
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Inspect ROS1 bags and export UAV Studio scenes."));
    const QCommandLineOption output("output", "Save a PNG, MP4, or RViz round-trip configuration to this path.", "file");
    const QCommandLineOption start("start", "Start at this playback time in seconds.", "seconds", "0");
    const QCommandLineOption duration("seconds", "Export this many seconds, or 0 for the entire remaining interval.", "seconds", "0");
    const QCommandLineOption port("port", "Loopback port for the ROS relay.", "port", "8765");
    const QCommandLineOption delay("delay", "Additional ROS seconds for TF and odometry lookahead.", "seconds", "0.3");
    const QCommandLineOption cloud("cloud-topic", "ROS PointCloud2 topic.", "topic");
    const QCommandLineOption odometry("odom-topic", "ROS Odometry topic.", "topic");
    const QCommandLineOption world("world", "World frame for replay.", "frame");
    const QCommandLineOption body("body", "Body frame for replay.", "frame");
    const QCommandLineOption pose("pose-source", "Pose source: odom or tf.", "source", "odom");
    const QCommandLineOption gap("max-gap", "Maximum interpolation gap in seconds.", "seconds", "0.5");
    const QCommandLineOption extrinsics("extrinsics", "JSON file with static transforms.", "file");
    const QCommandLineOption width("width", "MP4 width (multiple of four).", "pixels", "1920");
    const QCommandLineOption height("height", "MP4 height (even).", "pixels", "1080");
    const QCommandLineOption fps("fps", "MP4 frame rate.", "fps", "60");
    const QCommandLineOption supersample("supersample", "MP4 rendering scale (1 or 2).", "scale", "1");
    const QCommandLineOption encoder("encoder", "MP4 H.264 encoder (cpu or nvenc).", "encoder", "cpu");
    const QCommandLineOption crf("crf", "H.264 constant rate factor.", "quality", "18");
    parser.addOptions({output, start, duration, port, delay, cloud, odometry, world, body, pose, gap, extrinsics,
                       width, height, fps, crf, supersample, encoder});
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "inspect, export, bag, demo, listen, or rviz-roundtrip.");
    parser.addPositionalArgument("source", "ROS1 bag or .slamfpv project.", "[source]");
    parser.process(app);

    const auto args = parser.positionalArguments();
    const auto command = args.value(0);
    if (command == "rviz-roundtrip") {
        try {
            if (args.size() != 2 || !parser.isSet(output)) parser.showHelp(2);
            const QFileInfo source(args[1]), destination(parser.value(output));
            if (destination.suffix().compare("rviz", Qt::CaseInsensitive) != 0 ||
                source.absoluteFilePath() == destination.absoluteFilePath() ||
                (destination.exists() && source.canonicalFilePath() == destination.canonicalFilePath()))
                throw std::runtime_error("Select a separate .rviz output file; source configurations are read-only.");
            const auto layout = replay::readRviz(source.absoluteFilePath());
            QTemporaryDir scratch;
            if (!scratch.isValid()) throw std::runtime_error("Cannot create project round-trip staging directory.");
            auto document = replay::ProjectDocument::create("demo"); document.setTopicLayout(layout);
            const auto stored = document.saveAs(scratch.filePath("roundtrip.slamfpv"));
            const auto restored = replay::ProjectDocument::load(stored.path()).topicLayout();
            const auto bytes = replay::serializeRviz(restored);
            QSaveFile file(destination.absoluteFilePath());
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) throw std::runtime_error("Cannot write RViz output.");
            const QJsonObject report{{"output", destination.absoluteFilePath()}, {"native_display_count", int(restored.displays.size())},
                {"unrendered_plugins", QJsonArray::fromStringList(restored.skipped)}, {"byte_identical", bytes == layout.sourceYaml.toUtf8()},
                {"project_roundtrip", true}, {"native_plugin_parity", false}};
            QTextStream(stdout) << QString::fromUtf8(QJsonDocument(report).toJson()); return 0;
        } catch (const std::exception &error) { QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl; return 1; }
    }
    if (command == "export" || command == "bag" || command == "demo" || command == "listen") {
        const bool demo = command == "demo";
        const bool live = command == "listen";
        if (args.size() != (demo || live ? 1 : 2) || !parser.isSet(output))
            parser.showHelp(2);
        bool startOk = false, durationOk = false;
        const double begin = parser.value(start).toDouble(&startOk);
        const double seconds = parser.value(duration).toDouble(&durationOk);
        const QFileInfo destination(parser.value(output));
        const auto suffix = destination.suffix().toLower();
        if (!startOk || !durationOk || !std::isfinite(begin) || !std::isfinite(seconds) || begin < 0. || seconds < 0. ||
            (suffix != "png" && suffix != "mp4")) {
            QTextStream(stderr) << "Expected a valid PNG or MP4 destination and nonnegative times." << Qt::endl;
            return 2;
        }
        QString source = demo ? QStringLiteral(":demo:") : QFileInfo(args.value(1)).absoluteFilePath();
        std::unique_ptr<QTemporaryDir> capture;
        double exportSeconds = seconds;
        if (live || command == "bag") {
            try {
                const auto poseSource = parser.value(pose);
                bool gapOk = false;
                const double maximumGap = parser.value(gap).toDouble(&gapOk);
                if ((poseSource != "odom" && poseSource != "tf") || !gapOk ||
                    !std::isfinite(maximumGap) || maximumGap <= 0. || maximumGap > 60.)
                    throw std::runtime_error("Invalid pose source or interpolation gap.");

                const auto cloudTopic = parser.isSet(cloud) ? parser.value(cloud) : live ? QStringLiteral("/cloud_registered") : QString();
                const auto odomTopic = parser.isSet(odometry) ? parser.value(odometry) : live ? QStringLiteral("/Odometry") : QString();
                capture = std::make_unique<QTemporaryDir>();
                if (!capture->isValid())
                    throw std::runtime_error("Cannot create the temporary replay project.");
                if (live) {
                    bool portOk = false, delayOk = false;
                    const int portNumber = parser.value(port).toInt(&portOk);
                    const double lookahead = parser.value(delay).toDouble(&delayOk);
                    exportSeconds = seconds > 0. ? seconds : 6.;
                    if (!portOk || portNumber < 1 || portNumber > 65535 || !delayOk || !std::isfinite(lookahead) ||
                        lookahead < 0. || lookahead > 60. || !cloudTopic.startsWith('/') || !odomTopic.startsWith('/') ||
                        exportSeconds > 86'400. || begin > 86'400.)
                        throw std::runtime_error("Invalid relay port, lookahead, topics or recording duration.");
                    if (suffix == "mp4" && !parser.isSet(duration))
                        exportSeconds = 6.;
                    const double captureSeconds = begin + (suffix == "png" ? 0. : exportSeconds);
                    source = capture->filePath("relay.bag");
                    replay::captureLiveBag(source, {quint16(portNumber), cloudTopic, odomTopic, captureSeconds, lookahead});
                }
                const auto calibration = parser.isSet(extrinsics) ? QFileInfo(parser.value(extrinsics)).absoluteFilePath() : QString();
                auto doc = replay::ProjectDocument::create("bag", source, calibration);
                doc.setValue("cloud_topic", cloudTopic);
                doc.setValue("odom_topic", parser.isSet(odometry) ? odomTopic : QString());
                doc.setValue("pose_source", poseSource == "tf" ? QStringLiteral("TF") : QStringLiteral("Odometry"));
                doc.setValue("max_gap", parser.value(gap));
                doc.setValue("world", parser.value(world));
                doc.setValue("body", parser.value(body));
                source = doc.saveAs(capture->filePath("export.slamfpv")).path();
            } catch (const std::exception &error) {
                QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl;
                return 1;
            }
        }
        auto program = QCoreApplication::applicationDirPath() + QStringLiteral("/uav-studio");
#ifdef Q_OS_WIN
        program += QStringLiteral(".exe");
#endif
        QProcess window;
        window.setProcessChannelMode(QProcess::ForwardedChannels);
        window.start(program, {"--batch", source, destination.absoluteFilePath(),
                               QString::number(begin, 'g', 17), QString::number(exportSeconds, 'g', 17),
                               parser.value(width), parser.value(height), parser.value(fps), parser.value(crf), parser.value(supersample), parser.value(encoder)});
        if (!window.waitForStarted() || !window.waitForFinished(-1) || window.exitStatus() != QProcess::NormalExit) {
            QTextStream(stderr) << "Could not run the native export window: " << window.errorString() << Qt::endl;
            return 1;
        }
        return window.exitCode();
    }
    if (args.size() != 2 || command != QStringLiteral("inspect"))
        parser.showHelp(2);

    try {
        const auto data = QJsonDocument(replay::inspectBag(args.at(1))).toJson(QJsonDocument::Indented);
        QTextStream output(stdout);
        output << QString::fromUtf8(data);
        output.flush();
        return output.status() == QTextStream::Ok ? 0 : 1;
    } catch (const std::exception &error) {
        QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl;
        return 1;
    }
}
