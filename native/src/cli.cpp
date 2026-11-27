#include "replay_data.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QFileInfo>
#include <QProcess>
#include <cmath>
#include <QTextStream>
#include <exception>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PlannerReplayCli"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Inspect ROS1 bags and export Planner Replay scenes."));
    const QCommandLineOption output("output", "Save a PNG or MP4 to this path.", "file");
    const QCommandLineOption start("start", "Start at this playback time in seconds.", "seconds", "0");
    const QCommandLineOption duration("seconds", "Export this many seconds, or 0 for the entire remaining interval.", "seconds", "0");
    parser.addOptions({output, start, duration});
    parser.addHelpOption();
    parser.addPositionalArgument("command", "inspect, export, bag, or demo.");
    parser.addPositionalArgument("source", "ROS1 bag or .slamfpv project.", "[source]");
    parser.process(app);

    const auto args = parser.positionalArguments();
    const auto command = args.value(0);
    if (command == "export" || command == "bag" || command == "demo") {
        const bool demo = command == "demo";
        if (args.size() != (demo ? 1 : 2) || !parser.isSet(output))
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
        const auto source = demo ? QStringLiteral(":demo:") : QFileInfo(args[1]).absoluteFilePath();
        auto program = QCoreApplication::applicationDirPath() + QStringLiteral("/PlannerReplayNative");
#ifdef Q_OS_WIN
        program += QStringLiteral(".exe");
#endif
        QProcess window;
        window.setProcessChannelMode(QProcess::ForwardedChannels);
        window.start(program, {"--batch", source, destination.absoluteFilePath(),
                               QString::number(begin, 'g', 17), QString::number(seconds, 'g', 17)});
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
