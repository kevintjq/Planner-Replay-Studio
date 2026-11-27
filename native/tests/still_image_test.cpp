#include "still_image_writer.h"
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

void expect(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    try {
        QImage image(3840, 2160, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        image.setPixelColor(100, 100, Qt::red);
        const auto png = directory.filePath("frame.png");
        expect(replay::writeStillImage(image, png, 600, 3.125).isEmpty(), "PNG write failed");
        QImageReader reader(png);
        const auto decoded = reader.read();
        expect(decoded.size() == image.size() && decoded.pixelColor(0, 0).alpha() == 0 && decoded.pixelColor(100, 100) == QColor(Qt::red), "PNG pixels or alpha changed");
        expect(std::abs(decoded.dotsPerMeterX() * .0254 - 600) < .1, "DPI not preserved");
        expect(decoded.text("Description").contains("3.125000000"), "Experiment time missing");
        for (const auto &format : {"jpg", "pdf", "svg"}) {
            const auto file = directory.filePath(QString("frame.%1").arg(format));
            expect(replay::writeStillImage(image, file, 300, 3.125).isEmpty(), "Document write failed");
            QFile check(file); expect(check.open(QIODevice::ReadOnly), "Document missing");
            const auto bytes = check.readAll();
            if (QString(format) == "pdf") expect(bytes.startsWith("%PDF-"), "Invalid PDF");
            else if (QString(format) == "svg") expect(bytes.contains("<svg") && bytes.contains("<image") && bytes.contains("embedded raster"), "SVG must identify raster content");
            else expect(QImage(file).size() == image.size(), "JPEG dimensions changed");
        }
        expect(!replay::writeStillImage(image, directory.filePath("bad.bin"), 300, 0).isEmpty(), "Invalid format accepted");
        expect(!QFile::exists(directory.filePath("bad.bin")), "Failed output was published");
        std::cout << "4K pixels, alpha, DPI, timestamp, JPEG, PDF, SVG and atomic rejection passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
