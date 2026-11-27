#include "trajectory_controller.h"
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
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
        std::cout << "Trajectory workspace arbitrary source time, 4K PNG/TIFF/PDF/SVG, alpha/DPI and preserved editing state passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
