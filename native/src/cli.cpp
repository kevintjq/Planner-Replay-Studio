#include "replay_data.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <exception>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PlannerReplayCli"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Inspect a recorded ROS1 bag."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("inspect"), QStringLiteral("List topics, message counts, and storage time."));
    parser.addPositionalArgument(QStringLiteral("bag"), QStringLiteral("ROS1 bag file."));
    parser.process(app);

    const auto args = parser.positionalArguments();
    if (args.size() != 2 || args.first() != QStringLiteral("inspect"))
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
