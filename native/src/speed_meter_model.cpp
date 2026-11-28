#include "speed_meter_model.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
using replay::SpeedRange;
constexpr double Second = 1'000'000'000.;
QString normalizedFile(const QUrl &url)
{
    if (!url.isLocalFile()) throw std::runtime_error("Select a local Bag file.");
    const QFileInfo info(url.toLocalFile());
    if (!info.isFile()) throw std::runtime_error("The real Bag source does not exist.");
    return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}
qint64 timestamp(const QString &text)
{
    bool ok = false; const auto value = text.toLongLong(&ok);
    if (!ok || value < 0) throw std::runtime_error("Use an exact nonnegative experiment nanosecond timestamp string.");
    return value;
}
std::vector<SpeedRange> includedRanges(const replay::SpeedIndex &index, const QVariantMap &settings, std::optional<qint64> playhead = {})
{
    const auto mode = settings.value("statisticsMode", "entire").toString();
    if (mode == "entire") return {{index.firstStamp(), index.lastStamp()}};
    if (mode == "elapsed") {
        if (!playhead) throw std::runtime_error("Elapsed statistics require an exact experiment playhead timestamp.");
        return {{index.firstStamp(), std::max(index.firstStamp(), *playhead)}};
    }
    if (mode != "custom" && mode != "multiple" && mode != "current_clip") throw std::runtime_error("Unknown speed statistics mode.");
    const auto ranges = replay::speedRangesFromVariants(settings.value("includeRanges").toList());
    if (ranges.empty()) throw std::runtime_error("The selected statistics mode requires explicit experiment-time intervals.");
    return ranges;
}
double niceCeiling(double value)
{
    if (value <= 0.) return 5.;
    const auto decade = std::pow(10., std::floor(std::log10(value)));
    for (const double coefficient : {1., 2., 5., 10.}) if (value <= coefficient * decade * (1. + 1.e-12)) return coefficient * decade;
    return value;
}
QString csvCell(QString text) { text.replace('"', "\"\""); return '"' + text + '"'; }
QString number(double value) { return QString::number(value, 'g', 17); }
}

SpeedMeterModel::SpeedMeterModel(QObject *parent) : QObject(parent)
{
    connect(&exportWatcher_, &QFutureWatcher<QString>::finished, this, [this] {
        const auto error = exportWatcher_.result(); emit changed(); emit exportFinished(error.isEmpty(), error.isEmpty() ? tr("速度完整样本及统计 CSV 已导出。 / Full speed samples and statistics exported.") : error);
    });
}
SpeedMeterModel::~SpeedMeterModel()
{
    for (const auto &job : jobs_) *job->cancelled = true;
    for (const auto &job : jobs_) job->watcher->waitForFinished();
    exportWatcher_.waitForFinished();
}
std::shared_ptr<const SpeedMeterModel::Bundle> SpeedMeterModel::makeBundle(replay::SpeedSeries series, const replay::Cancel &cancel,
                                                                       std::shared_ptr<const replay::Recording> owner)
{
    auto bundle = std::make_shared<Bundle>(); bundle->series = std::move(series); bundle->recordingOwner = std::move(owner);
    for (size_t i = 0; i < bundle->indices.size(); ++i) {
        if (cancel && cancel()) throw std::runtime_error("Speed cache construction cancelled.");
        const auto mode = replay::VelocityMode(i);
        if ((bundle->series.vector && mode == replay::VelocityMode::Custom)
            || (!bundle->series.vector && mode != replay::VelocityMode::Custom
                && !(mode == replay::VelocityMode::Total && bundle->series.scalarIsSpeed))) continue;
        bundle->indices[i] = std::make_shared<replay::SpeedIndex>(bundle->series, mode);
    }
    return bundle;
}
void SpeedMeterModel::setTransforms(std::shared_ptr<const replay::TransformTree> transforms)
{
    if (transforms_ == transforms) return;
    clear(); transforms_ = std::move(transforms);
}
std::shared_ptr<const SpeedMeterModel::Bundle> SpeedMeterModel::data(const QString &id) const
{
    return cache_.value(meters_.value(id));
}
QVariantList SpeedMeterModel::sources() const
{
    QVariantList result;
    auto ids = meters_.keys(); ids.sort();
    for (const auto &id : ids) { auto state = sourceState(id); state.insert("id", id); result.append(state); }
    return result;
}
QVariantMap SpeedMeterModel::sourceState(const QString &id) const
{
    const auto bundle = data(id); const auto key = meters_.value(id);
    QVariantMap result{{"ready", bool(bundle)}, {"working", !key.isEmpty() && jobs_.contains(key)}, {"error", errors_.value(id)}};
    if (bundle) { result.insert("provenance", bundle->series.provenance); result.insert("samples", qlonglong(bundle->series.samples.size())); result.insert("vector", bundle->series.vector); }
    return result;
}
void SpeedMeterModel::loadSource(const QString &id, const QUrl &bag, const QVariantMap &source)
{
    if (id.isEmpty()) return;
    try {
        const auto path = normalizedFile(bag);
        const auto json = QJsonDocument(QJsonObject::fromVariantMap(source)).toJson(QJsonDocument::Compact);
        const auto key = path + ':' + QString::fromLatin1(QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex())
            + ':' + QString::number(generation_);
        const auto previous = meters_.value(id);
        errors_.remove(id); meters_.insert(id, key);
        if (previous != key && !meters_.values().contains(previous)) cache_.remove(previous);
        if (cache_.contains(key) || jobs_.contains(key)) { emit changed(); return; }
        auto job = std::make_shared<Job>(); job->generation = generation_; job->watcher = new QFutureWatcher<Result>(this); jobs_.insert(key, job);
        connect(job->watcher, &QFutureWatcher<Result>::finished, this, [this, key, job] {
            const auto result = job->watcher->result(); jobs_.remove(key);
            if (!*job->cancelled && job->generation == generation_) {
                if (result.bundle && meters_.values().contains(key)) cache_.insert(key, result.bundle);
                for (auto it = meters_.cbegin(); it != meters_.cend(); ++it)
                    if (it.value() == key) { if (result.error.isEmpty()) errors_.remove(it.key()); else errors_.insert(it.key(), result.error); }
            }
            job->watcher->deleteLater(); emit changed();
        });
        const auto tf = transforms_;
        job->watcher->setFuture(QtConcurrent::run([path, source, tf, stop = job->cancelled]() -> Result {
            try {
                const replay::Cancel cancel = [stop] { return stop->load(); };
                return {makeBundle(replay::loadSpeedSeries(path, source, tf, cancel), cancel), {}};
            } catch (const std::exception &error) { return {{}, QString::fromUtf8(error.what())}; }
        }));
        emit changed();
    } catch (const std::exception &error) { meters_.remove(id); errors_.insert(id, QString::fromUtf8(error.what())); emit changed(); }
}
void SpeedMeterModel::useRecording(const QString &id, std::shared_ptr<const replay::Recording> recording)
{
    if (id.isEmpty()) return;
    try {
        if (!recording || recording->speeds.empty()) throw std::runtime_error("The Recording has no direct speed cache. Select a real velocity Topic; no zero or unlabelled estimate was substituted.");
        const auto key = QStringLiteral("recording:") + QString::number(quintptr(recording.get()), 16) + ':' + QString::number(generation_);
        if (meters_.value(id) == key && (cache_.contains(key) || jobs_.contains(key))) return;
        const auto previous = meters_.value(id);
        meters_.insert(id, key); errors_.remove(id);
        if (previous != key && !meters_.values().contains(previous)) cache_.remove(previous);
        if (!cache_.contains(key) && !jobs_.contains(key)) {
            auto job = std::make_shared<Job>(); job->generation = generation_; job->watcher = new QFutureWatcher<Result>(this); jobs_.insert(key, job);
            connect(job->watcher, &QFutureWatcher<Result>::finished, this, [this, key, job] {
                const auto result = job->watcher->result(); jobs_.remove(key);
                if (!*job->cancelled && job->generation == generation_) {
                    if (result.bundle && meters_.values().contains(key)) cache_.insert(key, result.bundle);
                    for (auto it = meters_.cbegin(); it != meters_.cend(); ++it)
                        if (it.value() == key) { if (result.error.isEmpty()) errors_.remove(it.key()); else errors_.insert(it.key(), result.error); }
                }
                job->watcher->deleteLater(); emit changed();
            });
            job->watcher->setFuture(QtConcurrent::run([recording, stop = job->cancelled]() -> Result {
                try { return {makeBundle(replay::speedSeriesFromRecording(*recording), [stop] { return stop->load(); }, recording), {}}; }
                catch (const std::exception &error) { return {{}, QString::fromUtf8(error.what())}; }
            }));
        }
        emit changed();
    } catch (const std::exception &error) { removeMeter(id); errors_.insert(id, QString::fromUtf8(error.what())); emit changed(); }
}
void SpeedMeterModel::removeMeter(const QString &id)
{
    const auto key = meters_.take(id); errors_.remove(id);
    if (!key.isEmpty() && !meters_.values().contains(key)) {
        // A running shared read may still serve another meter added before it
        // completes. Its result is discarded if no meter needs it at completion.
        cache_.remove(key);
    }
    emit changed();
}
void SpeedMeterModel::clear()
{
    ++generation_; for (const auto &job : jobs_) *job->cancelled = true;
    meters_.clear(); errors_.clear(); cache_.clear(); emit changed();
}
QVariantMap SpeedMeterModel::frame(const QString &id, const QString &exactStampNs, const QVariantMap &settings) const
{
    auto result = sourceState(id); result.insert("currentValid", false);
    try {
        const auto bundle = data(id);
        if (!bundle) return result;
        const auto stamp = timestamp(exactStampNs); const auto mode = replay::velocityMode(settings.value("mode").toString());
        const auto index = bundle->indices[size_t(mode)];
        if (!index) throw std::runtime_error("The selected actual source does not contain the requested velocity components.");
        const auto unit = settings.value("unit", "m/s").toString(); const double scale = replay::speedDisplayScale(unit);
        const auto includes = includedRanges(*index, settings, stamp);
        const auto excludes = replay::speedRangesFromVariants(settings.value("excludeRanges").toList());
        auto stats = index->statistics(includes, excludes);
        result.insert("statistics", stats); result.insert("unit", unit); result.insert("displayScale", scale);
        result.insert("stampNs", exactStampNs); result.insert("mode", replay::velocityModeName(mode));
        const auto current = index->valueAt(stamp);
        if (current) { result.insert("currentSI", *current); result.insert("current", *current * scale); result.insert("currentValid", true); }
        else result.insert("currentError", "N/A: no valid speed at this exact experiment time; no extrapolation across missing data.");
        for (const auto &pair : {std::pair{"maximumSI", "maximum"}, {"minimumSI", "minimum"}, {"averageSI", "average"}})
            if (stats.contains(pair.first)) { result.insert(pair.first, stats.value(pair.first)); result.insert(pair.second, stats.value(pair.first).toDouble() * scale); }
        if (stats.contains("distanceMetres")) result.insert("distanceMetres", stats.value("distanceMetres"));
        result.insert("flightTimeSeconds", stats.value("validDurationSeconds"));
        const auto rangeMode = settings.value("rangeMode", "auto").toString(); const bool locked = settings.value("lockRange", false).toBool();
        if (rangeMode != "auto" && rangeMode != "manual") throw std::runtime_error("Gauge rangeMode must be auto or manual.");
        double low = settings.value("rangeMinimumSI", 0.).toDouble(), high = settings.value("rangeMaximumSI", 5.).toDouble();
        if (rangeMode == "auto" && !locked) {
            const auto reference = stats.contains("maximumSI") ? stats : index->statistics();
            const double minimum = reference.value("minimumSI", 0.).toDouble(), maximum = reference.value("maximumSI", 5.).toDouble();
            low = minimum < 0. ? -niceCeiling(-minimum) : 0.; high = niceCeiling(std::max(0., maximum));
            if (current) { if (*current < low) low = -niceCeiling(-*current); if (*current > high) high = niceCeiling(*current); }
        }
        if (!std::isfinite(low) || !std::isfinite(high) || high <= low) throw std::runtime_error("Gauge physical range must be finite and increasing.");
        result.insert("rangeMinimumSI", low); result.insert("rangeMaximumSI", high);
        result.insert("rangeMinimum", low * scale); result.insert("rangeMaximum", high * scale);
        const bool overflow = current && (*current < low || *current > high); result.insert("overflow", overflow);
        if (current) result.insert("indicatorFraction", std::clamp((*current - low) / (high - low), 0., 1.));
        if (overflow) result.insert("rangeWarning", "Actual velocity is outside the locked/manual range; the numeric value remains accurate.");
        result.insert("error", QString());
    } catch (const std::exception &error) { result.insert("error", QString::fromUtf8(error.what())); result.insert("currentValid", false); }
    return result;
}
QVariantList SpeedMeterModel::plot(const QString &id, const QVariantMap &settings, int maximumPoints) const
{
    try {
        const auto bundle = data(id); if (!bundle) return {};
        const auto index = bundle->indices[size_t(replay::velocityMode(settings.value("mode").toString()))]; if (!index) return {};
        const auto includes = includedRanges(*index, settings, settings.contains("playheadNs") ? std::optional(timestamp(settings.value("playheadNs").toString())) : std::nullopt);
        auto result = index->plot(includes, maximumPoints); const auto scale = replay::speedDisplayScale(settings.value("unit", "m/s").toString());
        for (auto &value : result) { auto row = value.toMap(); if (row.value("valid").toBool()) row.insert("value", row.value("valueSI").toDouble() * scale); value = row; }
        return result;
    } catch (const std::exception &) { return {}; }
}
void SpeedMeterModel::exportCsv(const QString &id, const QUrl &destination, const QVariantMap &settings)
{
    if (exportWatcher_.isRunning()) return;
    try {
        const auto bundle = data(id); if (!bundle) throw std::runtime_error("Load real speed data before exporting.");
        const auto index = bundle->indices[size_t(replay::velocityMode(settings.value("mode").toString()))];
        if (!index) throw std::runtime_error("The requested velocity mode is unavailable.");
        if (!destination.isLocalFile() || QFileInfo(destination.toLocalFile()).suffix().compare("csv", Qt::CaseInsensitive)) throw std::runtime_error("Select a local CSV destination.");
        const auto path = destination.toLocalFile();
        const auto sourceUrl = QUrl(bundle->series.provenance.value("source").toString());
        if (sourceUrl.isLocalFile() && QFileInfo(sourceUrl.toLocalFile()).absoluteFilePath().compare(QFileInfo(path).absoluteFilePath(), Qt::CaseInsensitive) == 0)
            throw std::runtime_error("CSV output cannot overwrite its real source.");
        const auto unit = settings.value("unit", "m/s").toString(); const auto scale = replay::speedDisplayScale(unit);
        const auto includes = includedRanges(*index, settings, settings.contains("playheadNs") ? std::optional(timestamp(settings.value("playheadNs").toString())) : std::nullopt);
        const auto excludes = replay::speedRangesFromVariants(settings.value("excludeRanges").toList());
        exportWatcher_.setFuture(QtConcurrent::run([bundle, index, path, unit, scale, includes, excludes]() -> QString {
            try {
                QSaveFile output(path); if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create the speed CSV destination.");
                const auto write = [&](const QString &line) { const auto bytes = line.toUtf8(); if (output.write(bytes) != bytes.size()) throw std::runtime_error("Cannot write the speed CSV."); };
                write("kind,stamp_ns,time_s,value_si_m_per_s,value,unit,valid,name,provenance_json\n");
                const auto metadata = csvCell(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(bundle->series.provenance)).toJson(QJsonDocument::Compact)));
                const auto stats = index->statistics(includes, excludes); const auto effective = replay::subtractSpeedRanges(includes, excludes);
                const auto &stamps = index->stamps(); const auto &values = index->values(); size_t range = 0;
                const auto inRange = [](const auto &ranges, qint64 stamp) { return std::any_of(ranges.begin(), ranges.end(), [stamp](const auto &r) { return stamp >= r.begin && stamp <= r.end; }); };
                for (size_t i = 0; i < stamps.size(); ++i) {
                    while (range < effective.size() && effective[range].end < stamps[i]) ++range;
                    if (range >= effective.size() || stamps[i] < effective[range].begin || inRange(excludes, stamps[i])) continue;
                    const bool valid = std::isfinite(values[i]);
                    write("sample," + QString::number(stamps[i]) + ',' + QString::number(double(stamps[i] - index->firstStamp()) / Second, 'f', 9)
                        + ',' + (valid ? number(values[i]) : QString()) + ',' + (valid ? number(values[i] * scale) : QString())
                        + ',' + csvCell(unit) + ',' + (valid ? "1" : "0") + ",," + metadata + '\n');
                }
                const auto statsMetadata = csvCell(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(stats)).toJson(QJsonDocument::Compact)));
                for (const auto &name : {QStringLiteral("maximumSI"), QStringLiteral("minimumSI"), QStringLiteral("averageSI")})
                    if (stats.contains(name)) write("statistic,,," + number(stats.value(name).toDouble()) + ',' + number(stats.value(name).toDouble() * scale) + ',' + csvCell(unit) + ",1," + csvCell(name) + ',' + statsMetadata + '\n');
                for (const auto &name : {QStringLiteral("distanceMetres"), QStringLiteral("validDurationSeconds")})
                    if (stats.contains(name)) write("statistic,,,," + number(stats.value(name).toDouble()) + ',' + csvCell(name == "distanceMetres" ? "m" : "s") + ",1," + csvCell(name) + ',' + statsMetadata + '\n');
                if (!output.commit()) throw std::runtime_error("Cannot atomically publish the speed CSV.");
                return {};
            } catch (const std::exception &error) { return QString::fromUtf8(error.what()); }
        })); emit changed();
    } catch (const std::exception &error) { emit exportFinished(false, QString::fromUtf8(error.what())); }
}
