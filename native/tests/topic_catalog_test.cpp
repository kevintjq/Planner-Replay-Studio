#include "replay_data.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUrl>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
QByteArray u32(quint32 value) { char data[4]; qToLittleEndian(value, data); return {data, 4}; }
QByteArray i16(qint16 value) { char data[2]; qToLittleEndian(value, data); return {data, 2}; }
QByteArray f64(double value) { char data[8]; qToLittleEndian(std::bit_cast<quint64>(value), data); return {data, 8}; }
QByteArray blob(const QByteArray &value) { return u32(quint32(value.size())) + value; }
QByteArray field(const QByteArray &name, const QByteArray &value) { return blob(name + '=' + value); }
QByteArray record(quint8 op, const QByteArray &header, const QByteArray &body) {
    return blob(field("op", QByteArray(1, char(op))) + header) + blob(body);
}
QByteArray time(quint32 sec, quint32 ns = 0) { return u32(sec) + u32(ns); }
QByteArray message(quint32 headerSec, double value, quint32 ns = 123) {
    const auto child = [&](double speed) { return f64(speed) + i16(-2) + i16(3); };
    return u32(7) + time(headerSec, ns) + blob("map") + child(value) + u32(2) + child(value + 1) + child(value + 2)
        + QByteArray(8, char(255)) + blob("123");
}
QByteArray connection(quint32 id, const QByteArray &name, const QByteArray &type, const QByteArray &definition = {}) {
    return record(7, field("conn", u32(id)) + field("topic", name), field("type", type) + field("message_definition", definition));
}
QByteArray item(quint32 id, quint32 seconds, const QByteArray &data) {
    return record(2, field("conn", u32(id)) + field("time", time(seconds)), data);
}
void writeBag(const QString &path, const QByteArray &payload, const QByteArray &connections) {
    QFile file(path); expect(file.open(QIODevice::WriteOnly), "Cannot open bag fixture");
    const auto data = QByteArray("#ROSBAG V2.0\n") + record(5, field("compression", "none") + field("size", u32(quint32(payload.size()))), payload) + connections;
    expect(file.write(data) == data.size(), "Cannot write bag fixture");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QByteArray definition = "Header header\nChild child\nChild[] array\nuint64 ticks\nstring tag\nuint8 CONSTANT=7\n"
            "================================================================================\nMSG: fixture/Child\nfloat64 speed\nint16[2] axes\n";
        const auto schema = replay::describeTopicSchema("fixture/Reading", definition);
        expect(schema.definitionAvailable && schema.parseable && schema.fields.size() == 5, "Nested ROS1 schema is incomplete");
        expect(schema.numericFields.contains("array[].axes[]") && schema.numericFields.contains("header.stamp.nsec") &&
            !schema.numericFields.contains("tag"), "Numeric schema paths include strings or lose array descendants");
        const auto missing = replay::describeTopicSchema("fixture/Unknown", {});
        expect(!missing.definitionAvailable && !missing.parseable && missing.status.contains("message_definition"), "Missing definition was not explained");
        const auto incomplete = replay::describeTopicSchema("fixture/Broken", "Missing child\n");
        expect(!incomplete.parseable && incomplete.status.contains("fixture/Missing"), "Missing nested type was not explained");
        const auto decoded = replay::decodeTopicMessage("fixture/Reading", definition, message(1700000000, 1), 10'000'000'000);
        expect(decoded.headerStampNs == "1700000000000000123" && decoded.stamp == 10'000'000'000 && decoded.timeBasis == "bag-storage",
            "Header and storage clocks were conflated or rounded");
        expect(decoded.structuredFields.value("ticks").toString() == "18446744073709551615", "uint64 exact value was lost");
        bool tagNumeric = false, nestedNumeric = false;
        for (const auto &entry : decoded.numericFields) {
            const auto row = entry.toMap();
            tagNumeric |= row.value("path").toString() == "tag";
            nestedNumeric |= row.value("path").toString() == "array[1].axes[0]" && row.value("value").toDouble() == -2.;
        }
        expect(!tagNumeric && nestedNumeric, "Structured numeric selection includes numeric-looking strings or misses nested arrays");
        const auto scalar = replay::decodeTopicNumeric("fixture/Reading", definition, message(1700000000, 1), "array[1].speed");
        expect(scalar && scalar->value == 3. && scalar->headerStampNs == "1700000000000000123", "Concrete nested array selection failed");
        const auto exact = replay::decodeTopicNumeric("fixture/Reading", definition, message(1700000000, 1), "ticks");
        expect(exact && exact->exactValue == "18446744073709551615", "Numeric series lost exact integer text");
        bool invalidIndex = false;
        try { replay::decodeTopicNumeric("fixture/Reading", definition, message(1700000000, 1), "array[2].speed"); }
        catch (const std::runtime_error &) { invalidIndex = true; }
        expect(invalidIndex, "Out-of-range numeric array index was accepted");

        QTemporaryDir directory; expect(directory.isValid(), "No temporary directory");
        const auto bagPath = directory.filePath("all-topics.bag");
        writeBag(bagPath, item(0, 10, message(1700000000, 1)) + item(1, 13, message(1700000005, 5)) +
            item(0, 11, message(1700000002, 3)) + item(2, 12, blob("raw text")) + item(3, 12, QByteArray::fromHex("deadbeef")),
            connection(0, "/custom/nonpreset", "fixture/Reading", definition) + connection(1, "/custom/nonpreset", "fixture/Reading", definition) +
            connection(2, "/custom/nonpreset", "std_msgs/String", "string data\n") + connection(3, "/opaque", "vendor/NoDefinition") +
            connection(4, "/advertised/empty", "fixture/Empty", "uint8 data\n"));
        const auto catalog = replay::inspectBagCatalog(bagPath);
        expect(catalog.size() == 4, "Catalog filtered unsupported/empty connections or failed to merge equal name/type");
        const replay::TopicCatalogEntry *custom = nullptr;
        for (const auto &entry : catalog) if (entry.name == "/custom/nonpreset" && entry.type == "fixture/Reading") custom = &entry;
        expect(custom && custom->count == 3 && custom->connectionCount == 2 && custom->firstStampNs == 10'000'000'000 &&
            custom->lastStampNs == 13'000'000'000 && std::abs(custom->frequencyHz() - 2. / 3.) < 1e-12,
            "Merged connection count, storage range or frequency is wrong");
        expect(custom->source == QUrl::fromLocalFile(bagPath).toString() && custom->toVariant().value("firstStampNs").metaType() == QMetaType::fromType<QString>(),
            "Catalog source is unstable or nanosecond timestamp crossed QML as a number");
        const auto recording = replay::loadBag(bagPath);
        expect(recording->hasMessages && recording->points.empty() && recording->frames.empty() && recording->poses.empty() &&
            recording->topicCatalog.size() == 4 && recording->timelineTimeBasis == "header" && recording->duration() == 5. &&
            recording->stampAt(2) == 1700000002000000123LL && recording->frameAt(recording->stampAt(2)).end == 0,
            "A custom-only Bag cannot drive its real Header timeline without fake geometry");
        bool ambiguous = false;
        try { replay::inspectBagTopic(bagPath, "/custom/nonpreset", 20'000'000'000); }
        catch (const std::runtime_error &) { ambiguous = true; }
        expect(ambiguous, "Same-name different types silently mixed schemas");
        const auto selected = replay::inspectBagTopic(bagPath, "/custom/nonpreset", 11'000'000'000, {}, 0, "fixture/Reading");
        expect(selected && selected->structuredFields.value("child").toMap().value("speed").toDouble() == 3., "Type-filtered inspection chose the wrong message");
        const auto headerSelected = replay::inspectBagTopic(bagPath, "/custom/nonpreset", 1700000003000000123LL, {}, 0, "fixture/Reading", "header");
        expect(headerSelected && headerSelected->stamp == 11'000'000'000 && headerSelected->selectionTimeBasis == "header" &&
            headerSelected->selectionStampNs == "1700000002000000123", "Header selection guessed a storage offset or conflated source clocks");
        const auto series = replay::sampleBagTopic(bagPath, "/custom/nonpreset", "child.speed", "header", {}, {}, 20, "fixture/Reading");
        expect(series.samples.size() == 3 && series.statistics.value("validCount").toLongLong() == 3 &&
            series.statistics.value("minimum").toDouble() == 1. && series.statistics.value("maximum").toDouble() == 5. &&
            series.statistics.value("mean").toDouble() == 3. && series.statistics.value("firstStampNs").toString() == "1700000000000000123" &&
            series.samples[1].toMap().value("seconds").toDouble() == 2., "Header-clock numeric plot or full population statistics are wrong");
        const auto interval = replay::sampleBagTopic(bagPath, "/custom/nonpreset", "child.speed", "storage", "11000000000", "13000000000", 2, "fixture/Reading");
        expect(interval.statistics.value("validCount").toLongLong() == 2 && interval.statistics.value("mean").toDouble() == 4., "Exact storage interval sampling failed");
        const auto opaque = replay::inspectBagTopic(bagPath, "/opaque", 20'000'000'000);
        expect(opaque && opaque->decodeStatus.contains("message_definition") && opaque->rawHex == "de ad be ef", "Opaque payload cannot be inspected with an explicit missing definition reason");
        bool cancelled = false;
        try { replay::inspectBagCatalog(bagPath, [] { return true; }); } catch (const std::runtime_error &) { cancelled = true; }
        expect(cancelled, "Catalog cancellation was ignored");

        const auto storageBag = directory.filePath("storage-only.bag");
        writeBag(storageBag, item(0, 4, blob("a")) + item(0, 7, blob("b")), connection(0, "/text", "std_msgs/String", "string data\n"));
        const auto storageRecord = replay::loadBag(storageBag);
        expect(storageRecord->timelineTimeBasis == "bag-storage" && storageRecord->duration() == 3. && storageRecord->stampAt(1) == 5'000'000'000,
            "Unstamped messages did not retain an explicit real storage timeline");
        std::cout << "Complete topic catalog, custom structured fields, exact clocks, numeric sampling and no-cloud playback passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
