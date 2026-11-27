#include "frame_export_plan.h"
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
void require(bool valid, const char *message) { if (!valid) throw std::runtime_error(message); }
QString safeView(QString text) {
    text.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
    return text.left(80);
}
}
QVector<replay::FrameExportJob> replay::planFrameExport(double duration, double currentTime,
    const QVariantList &markers, const QVariantList &views, const QVariantMap &settings)
{
    require(std::isfinite(duration) && duration > 0., "Invalid experiment duration.");
    const QString mode = settings.value("mode", "current").toString();
    QVector<double> times;
    if (mode == "current") times.append(currentTime);
    else if (mode == "manual" || mode == "samples") {
        const auto entries = settings.value("times").toString().split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
        const double fps = settings.value("sampleFps", 30.).toDouble();
        require(mode != "samples" || (std::isfinite(fps) && fps > 0. && fps <= 10000.), "Invalid sampling FPS.");
        require(entries.size() <= 1000, "At most 1000 export times are allowed.");
        for (const auto &entry : entries) {
            bool ok = false; double value = entry.toDouble(&ok);
            require(ok && std::isfinite(value), "Invalid export time or sample index.");
            if (mode == "samples") {
                require(value >= 0 && value == std::floor(value), "Sample indices must be nonnegative integers.");
                value /= fps;
            }
            times.append(value);
        }
    } else if (mode == "markers") {
        for (const auto &marker : markers) times.append(marker.toMap().value("time", -1.).toDouble());
    } else if (mode == "interval") {
        const double begin = settings.value("begin", 0.).toDouble(), end = settings.value("end", duration).toDouble();
        const double step = settings.value("interval", 1.).toDouble();
        require(std::isfinite(begin) && std::isfinite(end) && std::isfinite(step) && begin >= 0 && end <= duration && end >= begin && step > 0,
            "Invalid interval or experiment range.");
        const double count = std::floor((end - begin) / step + 1.e-9) + 1;
        require(count >= 1 && count <= 1000, "At most 1000 export times are allowed.");
        for (int i = 0; i < int(count); ++i) times.append(std::min(end, begin + i * step));
    } else require(false, "Unknown export time source.");
    require(!times.isEmpty() && times.size() <= 1000, "No export times, or too many markers.");
    for (double time : times) require(std::isfinite(time) && time >= 0 && time <= duration, "Export time is outside the experiment.");
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::llround(a * 1.e9) == std::llround(b * 1.e9); }), times.end());

    QStringList viewIds;
    if (settings.value("allViews").toBool()) {
        viewIds.append("main");
        for (const auto &view : views) {
            const auto id = view.toMap().value("id").toString();
            if (!id.isEmpty() && !viewIds.contains(id)) viewIds.append(id);
        }
    } else viewIds.append(settings.value("scope").toString() == "view" ? settings.value("viewId", "main").toString() : QString("canvas"));
    require(qint64(viewIds.size()) * times.size() <= 1000, "At most 1000 frame/view combinations are allowed.");
    const auto extension = settings.value("format", "png").toString().toLower();
    require(QStringList{"png", "jpg", "tiff"}.contains(extension), "Batch frames require PNG, JPG or TIFF.");
    const auto pattern = settings.value("pattern", "frame_{index}_{time}_{view}").toString();
    require(!pattern.isEmpty() && pattern.size() <= 180 && !pattern.endsWith('.') && !pattern.endsWith(' ') &&
        !pattern.contains(QRegularExpression("[\\x00-\\x1f<>:\"/\\\\|?*]")) && pattern != "." && pattern != "..", "Invalid filename pattern.");
    QVector<FrameExportJob> jobs; QSet<QString> names;
    for (double time : times) for (const auto &view : viewIds) {
        auto name = pattern;
        name.replace("{index}", QString::number(jobs.size() + 1).rightJustified(4, '0'));
        name.replace("{time}", QString::number(time, 'f', 9));
        name.replace("{view}", safeView(view));
        require(!name.contains('{') && !name.contains('}') && name.size() <= 220, "Unknown filename token or filename too long.");
        require(!QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption).match(name).hasMatch(), "Reserved filename.");
        name += '.' + extension;
        require(!names.contains(name.toCaseFolded()), "Filename pattern produces duplicate files. Include {index}, {time} or {view}.");
        names.insert(name.toCaseFolded()); jobs.append({time, view, name});
    }
    return jobs;
}
