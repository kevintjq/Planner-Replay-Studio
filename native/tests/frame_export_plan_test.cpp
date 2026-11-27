#include "frame_export_plan.h"
#include "still_image_writer.h"
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        const QVariantList markers{QVariantMap{{"time", 3.125}}, QVariantMap{{"time", 1.25}}};
        const QVariantList views{QVariantMap{{"id", "inset-a"}}};
        auto jobs = replay::planFrameExport(10., 0, markers, views, {{"mode", "manual"}, {"times", "3.125, 1.25; 3.125 0.000000001"}, {"allViews", true}});
        expect(jobs.size() == 6 && jobs[0].time == 1.e-9 && jobs[1].viewId == "inset-a" && jobs[4].time == 3.125, "Times not sorted/deduplicated or views missing");
        expect(jobs[0].fileName.contains("0.000000001") && jobs[0].fileName != jobs[1].fileName, "Nanosecond naming or unique views lost");
        jobs = replay::planFrameExport(10., 0, markers, views, {{"mode", "interval"}, {"begin", 1.}, {"end", 1.3}, {"interval", .1}});
        expect(jobs.size() == 4 && std::abs(jobs.last().time - 1.3) < 1.e-9, "Interval endpoints wrong");
        jobs = replay::planFrameExport(10., 0, markers, views, {{"mode", "markers"}});
        expect(jobs.size() == 2 && jobs[0].time == 1.25, "Event timestamps changed");
        jobs = replay::planFrameExport(10., 0, markers, views, {{"mode", "samples"}, {"times", "0,3,15"}, {"sampleFps", 30.}});
        expect(jobs.size() == 3 && jobs[1].time == .1 && jobs[2].time == .5, "Sampling grid wrong");
        const QList<QVariantMap> invalid{
            {{"mode", "manual"}, {"times", "nan"}}, {{"mode", "manual"}, {"times", "10.01"}},
            {{"mode", "manual"}, {"times", "1,2"}, {"pattern", "same"}},
            {{"mode", "current"}, {"pattern", "../outside"}}, {{"mode", "current"}, {"pattern", "CON"}},
            {{"mode", "current"}, {"pattern", "{unknown}"}}, {{"mode", "interval"}, {"interval", .00001}},
            {{"mode", "samples"}, {"times", "1.5"}}, {{"mode", "samples"}, {"times", "1"}, {"sampleFps", 0.}}
        };
        for (const auto &options : invalid) {
            bool rejected = false;
            try { replay::planFrameExport(10., 1., markers, views, options); } catch (const std::exception &) { rejected = true; }
            expect(rejected, "Invalid plan accepted");
        }
        expect(!replay::validatePanelFigure(1000, QSize(7680, 4320), 2).isEmpty(), "Figure memory limit missing");
        QTemporaryDir dir; QStringList paths;
        for (int i = 0; i < 4; ++i) {
            QImage image(320, 180, QImage::Format_ARGB32_Premultiplied); image.fill(i % 2 ? Qt::blue : Qt::red);
            const auto path = dir.filePath(QString("panel-%1.png").arg(i)); expect(image.save(path), "Cannot create panel"); paths.append(path);
        }
        const QList<double> times{0, 1.25, 3.125, 7.21};
        for (const auto &format : {"png", "pdf", "svg"}) {
            const auto path = dir.filePath(QString("figure.%1").arg(format));
            expect(replay::writePanelFigure(paths, times, path, QSize(320, 180), 2, 600, Qt::white).isEmpty(), "Figure write failed");
            if (QString(format) == "png") {
                const QImage figure(path);
                expect(figure.size() == QSize(640, 416) && figure.pixelColor(5, 5) == QColor(Qt::red) && figure.pixelColor(325, 5) == QColor(Qt::blue), "Figure geometry or panel pixels changed");
            } else {
                QFile file(path); expect(file.open(QIODevice::ReadOnly), "Figure missing"); const auto bytes = file.readAll();
                if (QString(format) == "svg") expect(bytes.contains("<text") && bytes.contains("7.210000000") && bytes.contains("(d)"), "SVG captions must be real text");
                else expect(bytes.startsWith("%PDF-"), "PDF invalid");
            }
        }
        paths[0] = dir.filePath("missing.png");
        expect(!replay::writePanelFigure(paths, times, dir.filePath("failed.pdf"), QSize(320, 180), 2, 300, Qt::white).isEmpty() && !QFile::exists(dir.filePath("failed.pdf")), "Broken figure published");
        std::cout << "Time sources, view products, safe names, memory limits and mixed vector/raster figure outputs passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
