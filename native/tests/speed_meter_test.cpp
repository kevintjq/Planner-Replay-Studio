#include "speed_meter_data.h"
#include "speed_meter_model.h"
#include "transform_tree.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
constexpr qint64 Second = 1'000'000'000;
constexpr qint64 Origin = 1'700'000'000'000'000'123;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
bool close(double first, double second, double tolerance = 1.e-9) { return std::abs(first - second) <= tolerance; }
template<class F> void rejected(F operation, const char *message) {
    bool failed = false; try { operation(); } catch (const std::exception &) { failed = true; } require(failed, message);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 30000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    require(done(), "Asynchronous speed operation did not finish");
}
QByteArray u32(quint32 value) { char bytes[4]; qToLittleEndian(value, bytes); return {bytes, 4}; }
QByteArray f64(double value) { char bytes[8]; qToLittleEndian(std::bit_cast<quint64>(value), bytes); return {bytes, 8}; }
QByteArray blob(const QByteArray &value) { return u32(quint32(value.size())) + value; }
QByteArray field(const QByteArray &name, const QByteArray &value) { return blob(name + '=' + value); }
QByteArray record(quint8 op, const QByteArray &header, const QByteArray &body) { return blob(field("op", QByteArray(1, char(op))) + header) + blob(body); }
QByteArray stamp(qint64 ns) { return u32(quint32(ns / Second)) + u32(quint32(ns % Second)); }
QByteArray header(qint64 ns, const QByteArray &frame = "map") { return u32(1) + stamp(ns) + blob(frame); }
QByteArray vector(double x, double y, double z) { return f64(x) + f64(y) + f64(z); }
QByteArray item(quint32 connection, qint64 stored, const QByteArray &message) { return record(2, field("conn", u32(connection)) + field("time", stamp(stored)), message); }
QByteArray connection(quint32 id, const QByteArray &topic, const QByteArray &type, const QByteArray &definition) { return record(7, field("conn", u32(id)) + field("topic", topic), field("type", type) + field("message_definition", definition)); }
void bag(const QString &path, const QByteArray &payload, const QByteArray &connections) {
    QFile file(path); require(file.open(QIODevice::WriteOnly), "Cannot create speed Bag fixture");
    const auto bytes = QByteArray("#ROSBAG V2.0\n") + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload) + connections;
    require(file.write(bytes) == bytes.size(), "Cannot write speed Bag fixture");
}
const QByteArray definitions =
    "\n================================================================================\nMSG: std_msgs/Header\nuint32 seq\ntime stamp\nstring frame_id\n"
    "================================================================================\nMSG: geometry_msgs/Vector3\nfloat64 x\nfloat64 y\nfloat64 z\n"
    "================================================================================\nMSG: geometry_msgs/Twist\ngeometry_msgs/Vector3 linear\ngeometry_msgs/Vector3 angular\n"
    "================================================================================\nMSG: geometry_msgs/TwistWithCovariance\ngeometry_msgs/Twist twist\nfloat64[36] covariance\n"
    "================================================================================\nMSG: geometry_msgs/Point\nfloat64 x\nfloat64 y\nfloat64 z\n"
    "================================================================================\nMSG: geometry_msgs/Quaternion\nfloat64 x\nfloat64 y\nfloat64 z\nfloat64 w\n"
    "================================================================================\nMSG: geometry_msgs/Pose\ngeometry_msgs/Point position\ngeometry_msgs/Quaternion orientation\n"
    "================================================================================\nMSG: geometry_msgs/PoseWithCovariance\ngeometry_msgs/Pose pose\nfloat64[36] covariance\n";
replay::SpeedSeries simple() {
    replay::SpeedSeries series; series.maximumGapNs = 10 * Second;
    for (const auto &[time, value] : {std::pair{0., 0.}, {1., 10.}, {4., 10.}})
        series.samples.push_back({qint64(time * Second), {value, 0., 0.}, 0., true, "map"});
    return series;
}
void math() {
    const auto source = simple(); const replay::SpeedIndex index(source, replay::VelocityMode::Total);
    const auto stats = index.statistics();
    require(close(stats.value("averageSI").toDouble(), 8.75) && close(stats.value("distanceMetres").toDouble(), 35.)
        && stats.value("validSampleCount").toInt() == 3, "Irregular samples used an arithmetic mean or lost real samples");
    require(index.valueAt(500'000'000) && close(*index.valueAt(500'000'000), 5.) && !index.valueAt(5 * Second), "Exact-time interpolation/extrapolation rule failed");
    const auto multiple = index.statistics({{0, 750'000'000}, {500'000'000, 1'500'000'000}, {3 * Second, 4 * Second}}, {{500'000'000, Second}});
    require(close(multiple.value("validDurationSeconds").toDouble(), 2.) && close(multiple.value("averageSI").toDouble(), 8.125)
        && close(multiple.value("distanceMetres").toDouble(), 16.25), "Union/exclusion statistics double-counted overlap or used wrong boundaries");
    const auto subSample = index.statistics({{1'100'000'000, 1'900'000'000}});
    require(close(subSample.value("averageSI").toDouble(), 10.) && !subSample.contains("maximumSI"), "Boundary interpolation was presented as a measured sample maximum");
    auto gap = source; gap.maximumGapNs = 500'000'000;
    const replay::SpeedIndex sparse(gap, replay::VelocityMode::Total);
    require(!sparse.valueAt(500'000'000) && !sparse.statistics().contains("averageSI"), "Long source gaps were silently filled");
    auto invalid = source; invalid.samples.insert(invalid.samples.begin() + 2, {2 * Second, {}, 0., false, "map"});
    const replay::SpeedIndex broken(invalid, replay::VelocityMode::Total);
    require(!broken.valueAt(3 * Second) && close(broken.statistics().value("validDurationSeconds").toDouble(), 1.), "Invalid samples did not break integration");
    auto changed = source; changed.samples[1].frame = "body";
    require(!replay::SpeedIndex(changed, replay::VelocityMode::Total).statistics().contains("averageSI"), "Different source frames were interpolated without a transform");
    const auto filtered = replay::filteredSpeedSeries(source, 1.);
    require(close(source.samples[1].velocity[0], 10.) && close(filtered.samples[1].velocity[0], 10. * (1. - std::exp(-1.)))
        && filtered.provenance.value("filtered").toBool(), "Filtering modified raw data or hid its definition");
    require(replay::speedDisplayScale("km/h") == 3.6 && replay::speedDisplayScale("m/s") == 1., "Display unit conversion is not exactly 3.6");
    rejected([] { replay::speedDisplayScale("mph"); }, "Unsupported display unit was silently accepted");
    rejected([] { replay::speedRangesFromVariants({QVariantMap{{"beginNs", "9007199254740993"}, {"endNs", "9007199254740992"}}}); }, "Invalid exact large timestamps were accepted");
}
QString customBag(const QString &path, bool highRate = false) {
    QByteArray payload;
    const int count = highRate ? 200001 : 3;
    for (int i = 0; i < count; ++i) {
        const auto time = Origin + (highRate ? qint64(i) * 5'000'000 : qint64(i) * Second);
        const double value = highRate ? (i == 123457 ? 50. : 1.) : 36.;
        payload += item(0, 10 * Second + qint64(i) * 5'000'000, header(time) + vector(3., 4., -12.) + f64(value));
    }
    bag(path, payload, connection(0, "/arbitrary/velocity", "fixture/Velocity", "Header header\ngeometry_msgs/Vector3 velocity\nfloat64 scalar\n" + definitions));
    return path;
}
QVariantMap vectorSource() { return {{"topic", "/arbitrary/velocity"}, {"type", "fixture/Velocity"}, {"fields", QStringList{"velocity.x", "velocity.y", "velocity.z"}}, {"inputUnit", "m/s"}, {"timeBasis", "header"}, {"sourceFrame", "map"}, {"maximumGapSeconds", 2.}}; }
void realSchema(const QString &directory) {
    const auto path = customBag(directory + "/custom.bag");
    const auto source = replay::loadSpeedSeries(path, vectorSource());
    require(source.samples.size() == 3 && source.samples[0].stampNs == Origin, "Real custom fields lost exact Header timestamps");
    require(close(*replay::SpeedIndex(source, replay::VelocityMode::Total).valueAt(Origin), 13.)
        && close(*replay::SpeedIndex(source, replay::VelocityMode::Horizontal).valueAt(Origin), 5.)
        && close(*replay::SpeedIndex(source, replay::VelocityMode::Vertical).valueAt(Origin), -12.), "Actual custom vector mode definitions are incorrect");
    auto scalarSource = vectorSource(); scalarSource.remove("fields"); scalarSource.insert("fieldPath", "scalar"); scalarSource.insert("inputUnit", "km/h"); scalarSource.insert("scalarIsSpeed", true);
    const auto scalar = replay::loadSpeedSeries(path, scalarSource);
    require(close(*replay::SpeedIndex(scalar, replay::VelocityMode::Total).valueAt(Origin), 10.), "Source km/h was not converted to canonical SI");
    scalarSource.remove("inputUnit"); rejected([&] { replay::loadSpeedSeries(path, scalarSource); }, "Unknown custom source units were assumed to be SI");
    rejected([&] { replay::loadSpeedSeries(path, vectorSource(), {}, [] { return true; }); }, "Bag read cancellation was ignored");
    qint64 visited = 0; replay::visitBagTopic(path, "/arbitrary/velocity", [&](const auto &, const auto &, auto, qint64) { ++visited; });
    require(visited == 3, "One-pass serialized visitor lost selected messages");
    const auto odometry = directory + "/odometry.bag";
    const auto message = [&](qint64 time, double x) { return header(time) + blob("body") + vector(x, 0., 0.) + vector(0., 0., 0.) + f64(1.) + QByteArray(36*8, '\0')
        + vector(3., 4., 0.) + vector(0., 0., 0.) + QByteArray(36*8, '\0'); };
    bag(odometry, item(0, 10*Second, message(Origin, 0.)) + item(0, 11*Second, message(Origin + Second, 2.))
        + item(0, 12*Second, message(Origin + 2*Second, 4.)), connection(0, "/vehicle/odom", "nav_msgs/Odometry", "Header header\nstring child_frame_id\ngeometry_msgs/PoseWithCovariance pose\ngeometry_msgs/TwistWithCovariance twist\n" + definitions));
    auto tf = std::make_shared<replay::TransformTree>();
    tf->add("map", "body", 0, {{0., 0., 0.}, QQuaternion::fromAxisAndAngle(0., 0., 1., 90.)}, true); tf->prepare();
    const QVariantMap standard{{"topic", "/vehicle/odom"}, {"timeBasis", "header"}, {"targetFrame", "map"}, {"maximumGapSeconds", 2.}};
    const auto transformed = replay::loadSpeedSeries(odometry, standard, tf);
    require(transformed.samples[0].frame == "map" && close(transformed.samples[0].velocity[0], -4., 1.e-5)
        && close(transformed.samples[0].velocity[1], 3., 1.e-5), "Odometry Twist used header.frame_id instead of child_frame_id or skipped actual TF");
    const auto absentTf = replay::loadSpeedSeries(odometry, standard);
    require(!absentTf.samples[0].valid && absentTf.provenance.value("missingTfCount").toInt() == 3
        && !replay::SpeedIndex(absentTf, replay::VelocityMode::Total).valueAt(Origin), "Missing velocity TF was replaced by zero or raw wrong-frame values");
    auto estimate = standard; estimate.insert("method", "position-difference");
    const auto estimated = replay::loadSpeedSeries(odometry, estimate, tf);
    require(estimated.provenance.value("estimated").toBool() && close(*replay::SpeedIndex(estimated, replay::VelocityMode::Total).valueAt(Origin + Second), 2.), "Real position differences were incorrect or unlabelled");
    for (const auto &type : {QByteArray("geometry_msgs/Twist"), QByteArray("geometry_msgs/TwistStamped"), QByteArray("geometry_msgs/TwistWithCovarianceStamped")}) {
        const bool stamped = type.endsWith("Stamped"), covariance = type.contains("Covariance");
        const auto bytes = (stamped ? header(Origin, "body") : QByteArray()) + vector(3., 4., 12.) + vector(0., 0., 0.) + (covariance ? QByteArray(36*8, '\0') : QByteArray());
        const auto definition = (stamped ? QByteArray("Header header\n") : QByteArray())
            + (covariance ? QByteArray("geometry_msgs/TwistWithCovariance twist\n") : stamped ? QByteArray("geometry_msgs/Twist twist\n") : QByteArray("geometry_msgs/Vector3 linear\ngeometry_msgs/Vector3 angular\n")) + definitions;
        const auto file = directory + '/' + QString::fromUtf8(type).replace('/', '-') + ".bag";
        bag(file, item(0, 10*Second, bytes), connection(0, "/unusual/topic/name", type, definition));
        const auto data = replay::loadSpeedSeries(file, {{"topic", "/unusual/topic/name"}, {"timeBasis", stamped ? "header" : "storage"}});
        require(data.samples.size() == 1 && close(*replay::SpeedIndex(data, replay::VelocityMode::Total).valueAt(stamped ? Origin : 10*Second), 13.), "An actual standard velocity schema was not parsed");
    }
}
void model(const QString &directory) {
    const auto path = directory + "/custom.bag"; const auto source = vectorSource();
    SpeedMeterModel model; model.loadSource("A", QUrl::fromLocalFile(path), source); model.loadSource("B", QUrl::fromLocalFile(path), source);
    require(model.working() && !model.sourceState("A").value("ready").toBool(), "Source loading blocked the GUI thread");
    wait([&] { return !model.working(); });
    const auto stamp = QString::number(Origin + Second);
    const auto a = model.frame("A", stamp, {{"unit", "m/s"}}), b = model.frame("B", stamp, {{"unit", "km/h"}});
    require(a.value("currentValid").toBool() && close(a.value("current").toDouble(), 13.) && close(b.value("current").toDouble(), 46.8)
        && close(b.value("average").toDouble(), 46.8) && close(b.value("rangeMaximum").toDouble(), a.value("rangeMaximum").toDouble()*3.6)
        && !model.working(), "Independent unit/range/statistics conversion lost the source or changed physical values");
    const auto overflow = model.frame("A", stamp, {{"rangeMode", "manual"}, {"rangeMinimumSI", 0.}, {"rangeMaximumSI", 5.}});
    require(overflow.value("overflow").toBool() && close(overflow.value("current").toDouble(), 13.) && close(overflow.value("indicatorFraction").toDouble(), 1.), "Out-of-range velocity gave a false numeric value or indicator");
    for (const auto offset : {2*Second, qint64(0), Second, 500'000'000LL})
        require(close(model.frame("A", QString::number(Origin + offset), {{"playbackRate", 2.}}).value("currentSI").toDouble(), 13.), "Random seeking/playback rate changed physical velocity");
    const auto missing = model.frame("A", QString::number(Origin - 1));
    require(!missing.value("currentValid").toBool() && !missing.contains("current"), "Missing current data were presented as zero");
    bool csvDone = false, csvSuccess = false;
    QObject::connect(&model, &SpeedMeterModel::exportFinished, [&](bool success, const QString &) { csvDone = true; csvSuccess = success; });
    const auto csv = directory + "/speed.csv"; model.exportCsv("A", QUrl::fromLocalFile(csv), {{"unit", "km/h"}});
    wait([&] { return csvDone; }); QFile file(csv); require(csvSuccess && file.open(QIODevice::ReadOnly), "Full-resolution CSV did not publish");
    const auto bytes = file.readAll(); require(bytes.contains(QByteArray::number(Origin)) && bytes.contains("sample,") && bytes.contains("averageSI") && bytes.contains("46.8"), "CSV lost exact time, full samples, units or statistic definitions");
    model.removeMeter("A"); require(model.sourceState("B").value("ready").toBool() && model.frame("B", stamp).value("currentValid").toBool(), "Removing one meter discarded another independent source");
    model.clear(); model.loadSource("after-clear", QUrl::fromLocalFile(path), source); wait([&] { return !model.working(); });
    require(model.sources().size() == 1 && model.sourceState("after-clear").value("ready").toBool(), "Cleared jobs contaminated a new source generation");
    const auto recording = replay::loadBag(directory + "/odometry.bag");
    model.useRecording("legacy-cache", recording);
    require(model.working(), "Recording speed index construction blocked the GUI thread");
    wait([&] { return !model.working(); });
    require(close(model.frame("legacy-cache", QString::number(Origin)).value("currentSI").toDouble(), 5.), "Existing direct Recording speed cache was not retained");
    model.useRecording("legacy-cache", recording);
    require(!model.working() && model.sourceState("legacy-cache").value("ready").toBool(), "Repeated Recording settings unnecessarily rebuilt the speed cache");
    model.useRecording("legacy-cache", {});
    require(!model.sourceState("legacy-cache").value("ready").toBool(), "An invalid new Recording preserved stale speed values");
}
void highRate(const QString &directory) {
    const auto path = customBag(directory + "/200hz.bag", true);
    const QVariantMap source{{"topic", "/arbitrary/velocity"}, {"fieldPath", "scalar"}, {"inputUnit", "m/s"}, {"timeBasis", "header"}, {"scalarIsSpeed", true}, {"maximumGapSeconds", .01}};
    QElapsedTimer timer; timer.start(); const auto data = replay::loadSpeedSeries(path, source);
    const replay::SpeedIndex index(data, replay::VelocityMode::Total); const auto stats = index.statistics();
    require(data.samples.size() == 200001 && stats.value("validSampleCount").toInt() == 200001 && close(stats.value("maximumSI").toDouble(), 50.)
        && close(stats.value("validDurationSeconds").toDouble(), 1000., 1.e-6) && close(stats.value("averageSI").toDouble(), 1.000245, 1.e-8), "200 Hz full statistics decimated samples, lost an isolated peak or integrated wrong time");
    const auto plot = index.plot({}, 1000); bool foundPeak = false; qint64 previous = -1;
    for (const auto &entry : plot) { const auto row = entry.toMap(); const auto time = row.value("stampNs").toString().toLongLong(); require(time > previous, "Display plot timestamps are unordered or duplicated"); previous = time; foundPeak |= close(row.value("valueSI").toDouble(), 50.); }
    require(plot.size() <= 1000 && foundPeak && stats.value("validSampleCount").toInt() > plot.size(), "Display decimation lost a peak or changed full-resolution statistics");
    SpeedMeterModel async; async.loadSource("cancelled", QUrl::fromLocalFile(path), source); async.clear(); async.loadSource("new", QUrl::fromLocalFile(directory + "/custom.bag"), vectorSource());
    wait([&] { return !async.working(); }); require(async.sources().size() == 1 && async.sourceState("new").value("ready").toBool(), "Cancelled full-speed read overwrote the next independent source");
    std::cout << "200 Hz fixture: " << data.samples.size() << " raw samples, " << plot.size() << " display points, full load/index/check " << timer.elapsed() << " ms.\n";
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory; require(directory.isValid(), "Cannot create speed test directory");
        math(); realSchema(directory.path()); model(directory.path()); highRate(directory.path());
        std::cout << "Exact speed clocks, real standard/custom Bag schemas, TF, SI units, weighted union/exclusion statistics, independent async meters, CSV and full 200 Hz cache verified. GUI/visual acceptance is separate.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
