#include "ros_topic_message.h"

#include <QBuffer>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace replay {
namespace {
constexpr quint32 ArrayPage = 128;
constexpr qsizetype MaxFieldCharacters = 4000;

class Reader {
public:
    explicit Reader(QByteArrayView data) : data_(data) {}

    QByteArrayView take(qsizetype len)
    {
        if (len < 0 || len > data_.size())
            throw std::runtime_error("Truncated ROS message.");
        const auto result = data_.first(len);
        data_ = data_.sliced(len);
        return result;
    }

    template<class T>
    T number() { return qFromLittleEndian<T>(take(sizeof(T)).data()); }

    QByteArrayView blob() { return take(number<quint32>()); }
private:
    QByteArrayView data_;
};

struct Field {
    QString type;
    QString name;
    qint64 count = -2; // Scalar. -1 means a variable-length array.
};
using Definitions = QHash<QString, std::vector<Field>>;

Definitions parseDefinition(const QString &root, const QByteArray &definition)
{
    Definitions schema;
    schema.insert("std_msgs/Header", {{"uint32","seq"},{"time","stamp"},{"string","frame_id"}});
    if (!definition.trimmed().isEmpty())
        schema.insert(root, {});
    auto current = root;
    for (auto line : QString::fromUtf8(definition).split('\n')) {
        line = line.section('#', 0, 0).trimmed();
        if (line.startsWith("MSG:")) {
            current = line.mid(4).trimmed();
            continue;
        }
        if (line.isEmpty() || line.startsWith('=') || line.contains('='))
            continue;
        const auto parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() != 2)
            throw std::runtime_error("Invalid ROS field definition in recorded message_definition.");
        Field field{parts[0], parts[1]};
        const auto bracket = field.type.indexOf('[');
        if (bracket >= 0) {
            if (!field.type.endsWith(']'))
                throw std::runtime_error("Invalid ROS array type.");
            const auto length = field.type.mid(bracket + 1, field.type.size() - bracket - 2);
            bool ok = false;
            field.count = length.isEmpty() ? -1 : length.toLongLong(&ok);
            if (!length.isEmpty() && (!ok || field.count < 0 || field.count > std::numeric_limits<quint32>::max()))
                throw std::runtime_error("Invalid ROS array size.");
            field.type.truncate(bracket);
        }
        if (field.type == "Header")
            field.type = "std_msgs/Header";
        auto &fields = schema[current];
        if (std::none_of(fields.begin(), fields.end(), [&](const auto &old) { return old.name == field.name; }))
            fields.push_back(std::move(field));
    }
    return schema;
}

const Definitions &cachedDefinition(const QString &root, const QByteArray &definition)
{
    // A worker may visit interleaved connections repeatedly. Parse their immutable
    // recorded definitions once; keep the per-thread cache bounded.
    thread_local std::map<std::pair<QString, QByteArray>, Definitions> cache;
    const auto key = std::make_pair(root, definition);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    if (cache.size() >= 16) cache.erase(cache.begin());
    return cache.emplace(key, parseDefinition(root, definition)).first->second;
}

QJsonValue finite(double value)
{
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QString::number(value));
}

qsizetype fixedSize(const QString &type)
{
    if (type == "bool" || type == "byte" || type == "char" || type == "uint8" || type == "int8") return 1;
    if (type == "uint16" || type == "int16") return 2;
    if (type == "uint32" || type == "int32" || type == "float32") return 4;
    if (type == "uint64" || type == "int64" || type == "float64" || type == "time" || type == "duration") return 8;
    return 0;
}

QString qualifiedType(const QString &parent, const QString &type)
{
    if (type == "Header") return QStringLiteral("std_msgs/Header");
    return fixedSize(type) || type == "string" || type.contains('/')
        ? type : parent.section('/', 0, -2) + '/' + type;
}

bool numericType(const QString &type)
{
    return fixedSize(type) && type != "time" && type != "duration";
}

QVariantList schemaFields(const Definitions &definitions, const QString &type, const QString &prefix,
                         QStringList &numeric, QStringList &errors, QStringList ancestors = {})
{
    if (ancestors.size() >= 24 || ancestors.contains(type)) {
        errors.append(QStringLiteral("Recursive or excessively deep definition: %1").arg(type));
        return {};
    }
    ancestors.append(type);
    const auto fields = definitions.constFind(type);
    if (fields == definitions.cend()) {
        errors.append(QStringLiteral("Recorded definition is missing nested type %1.").arg(type));
        return {};
    }
    QVariantList result;
    for (const auto &field : *fields) {
        const auto resolved = qualifiedType(type, field.type);
        const bool array = field.count != -2;
        const auto path = prefix + field.name + (array ? QStringLiteral("[]") : QString());
        QVariantMap row{{"name", field.name}, {"path", path}, {"type", resolved},
                        {"array", array}, {"variableLength", field.count == -1},
                        {"arrayLength", field.count >= 0 ? field.count : qint64(-1)},
                        {"numeric", numericType(resolved)}};
        if (numericType(resolved)) {
            numeric.append(path);
        } else if (resolved == "time" || resolved == "duration") {
            QVariantList children;
            for (const auto &part : {QStringLiteral("sec"), QStringLiteral("nsec")}) {
                const auto childPath = path + '.' + part;
                numeric.append(childPath);
                children.append(QVariantMap{{"name", part}, {"path", childPath},
                    {"type", resolved == "time" ? "uint32" : "int32"}, {"array", false},
                    {"numeric", true}, {"children", QVariantList()}});
            }
            row.insert("children", children);
        } else if (resolved != "string") {
            row.insert("children", schemaFields(definitions, resolved, path + '.', numeric, errors, ancestors));
        }
        if (!row.contains("children")) row.insert("children", QVariantList());
        result.append(row);
    }
    return result;
}

void skipValue(Reader &input, const Definitions &schema, const QString &type, int depth)
{
    if (depth > 24) throw std::runtime_error("ROS schema nesting is too deep.");
    if (const auto size = fixedSize(type)) { input.take(size); return; }
    if (type == "string") { input.blob(); return; }
    const auto fields = schema.constFind(type);
    if (fields == schema.cend()) throw std::runtime_error("ROS type is absent from the recorded schema.");
    for (const auto &field : *fields) {
        const auto resolved = qualifiedType(type, field.type);
        const auto count = field.count == -2 ? quint32(1) : field.count < 0 ? input.number<quint32>() : quint32(field.count);
        if (const auto width = fixedSize(resolved)) {
            input.take(qsizetype(count) * width);
        } else {
            if (count > 1'000'000) throw std::runtime_error("ROS message contains too many nested values.");
            for (quint32 i = 0; i < count; ++i) skipValue(input, schema, resolved, depth + 1);
        }
    }
}

void collectHeaderRange(Reader &input, const Definitions &schema, const QString &type, int depth,
                        std::optional<std::pair<qint64, qint64>> &range)
{
    if (depth > 24) throw std::runtime_error("ROS schema nesting is too deep.");
    if (type == "std_msgs/Header") {
        input.number<quint32>();
        const auto sec = input.number<quint32>(), ns = input.number<quint32>();
        input.blob();
        if (ns >= 1'000'000'000 || (!sec && !ns)) return;
        const auto stamp = qint64(sec) * 1'000'000'000 + ns;
        if (!range) range = std::make_pair(stamp, stamp);
        else { range->first = std::min(range->first, stamp); range->second = std::max(range->second, stamp); }
        return;
    }
    if (fixedSize(type) || type == "string") { skipValue(input, schema, type, depth); return; }
    const auto fields = schema.constFind(type);
    if (fields == schema.cend()) throw std::runtime_error("ROS type is absent from the recorded schema.");
    for (const auto &field : *fields) {
        const auto resolved = qualifiedType(type, field.type);
        const auto count = field.count == -2 ? quint32(1) : field.count < 0 ? input.number<quint32>() : quint32(field.count);
        if (const auto width = fixedSize(resolved)) input.take(qsizetype(count) * width);
        else {
            if (count > 1'000'000) throw std::runtime_error("ROS message contains too many nested values.");
            for (quint32 i = 0; i < count; ++i) collectHeaderRange(input, schema, resolved, depth + 1, range);
        }
    }
}

struct SelectionPart { QString name; qint64 index = -1; };
using Selection = std::vector<SelectionPart>;

Selection parseSelection(const QString &path)
{
    static const QRegularExpression syntax(QStringLiteral("^([A-Za-z_][A-Za-z0-9_]*)(?:\\[([0-9]+)\\])?$"));
    Selection result;
    for (const auto &part : path.split('.')) {
        const auto match = syntax.match(part);
        if (!match.hasMatch()) throw std::runtime_error("Select a concrete numeric field; arrays require an explicit [index].");
        bool ok = true;
        const auto index = match.captured(2).isEmpty() ? qint64(-1) : match.captured(2).toLongLong(&ok);
        if (!ok || index > std::numeric_limits<quint32>::max()) throw std::runtime_error("ROS array index is invalid.");
        result.push_back({match.captured(1), index});
    }
    return result;
}

struct Scalar { QJsonValue value; QString type; };
Scalar selectScalar(Reader &input, const Definitions &schema, const QString &type,
                    const Selection &selection, size_t at = 0, int depth = 0, bool allowString = false)
{
    if (depth > 24 || at >= selection.size()) throw std::runtime_error("Selected field is not numeric.");
    if (type == "time" || type == "duration") {
        const auto sec = type == "time" ? qint64(input.number<quint32>()) : qint64(input.number<qint32>());
        const auto nsec = type == "time" ? qint64(input.number<quint32>()) : qint64(input.number<qint32>());
        const auto &part = selection[at];
        if (at + 1 != selection.size() || part.index >= 0 || (part.name != "sec" && part.name != "nsec"))
            throw std::runtime_error("ROS time fields must select sec or nsec.");
        return {part.name == "sec" ? sec : nsec, type == "time" ? "uint32" : "int32"};
    }
    const auto fields = schema.constFind(type);
    if (fields == schema.cend()) throw std::runtime_error("ROS type is absent from the recorded schema.");
    for (const auto &field : *fields) {
        const auto resolved = qualifiedType(type, field.type);
        const bool array = field.count != -2;
        const auto count = !array ? quint32(1) : field.count < 0 ? input.number<quint32>() : quint32(field.count);
        if (field.name != selection[at].name) {
            if (const auto width = fixedSize(resolved)) input.take(qsizetype(count) * width);
            else {
                if (count > 1'000'000) throw std::runtime_error("ROS message contains too many nested values.");
                for (quint32 i = 0; i < count; ++i) skipValue(input, schema, resolved, depth + 1);
            }
            continue;
        }
        const auto index = selection[at].index;
        if ((array && (index < 0 || quint64(index) >= count)) || (!array && index >= 0))
            throw std::runtime_error("Selected array index is missing or outside this message.");
        if (array) {
            if (const auto width = fixedSize(resolved)) input.take(qsizetype(index) * width);
            else for (qint64 i = 0; i < index; ++i) skipValue(input, schema, resolved, depth + 1);
        }
        if (at + 1 < selection.size()) return selectScalar(input, schema, resolved, selection, at + 1, depth + 1, allowString);
        if (resolved == "string" && allowString) {
            const auto text = input.blob();
            if (text.size() > 4096) throw std::runtime_error("Selected ROS string exceeds 4096 bytes.");
            return {QString::fromUtf8(text), resolved};
        }
        if (!numericType(resolved)) throw std::runtime_error("Selected field is not a numeric scalar.");
        if (resolved == "bool") return {input.number<quint8>() != 0, resolved};
        if (resolved == "char" || resolved == "uint8") return {int(input.number<quint8>()), resolved};
        if (resolved == "byte" || resolved == "int8") return {int(input.number<qint8>()), resolved};
        if (resolved == "uint16") return {int(input.number<quint16>()), resolved};
        if (resolved == "int16") return {int(input.number<qint16>()), resolved};
        if (resolved == "uint32") return {qint64(input.number<quint32>()), resolved};
        if (resolved == "int32") return {input.number<qint32>(), resolved};
        if (resolved == "uint64") return {QString::number(input.number<quint64>()), resolved};
        if (resolved == "int64") return {QString::number(input.number<qint64>()), resolved};
        if (resolved == "float32") return {finite(std::bit_cast<float>(input.number<quint32>())), resolved};
        return {finite(std::bit_cast<double>(input.number<quint64>())), resolved};
    }
    throw std::runtime_error("Selected field is absent from the recorded definition.");
}

QString headerTimestamp(const Definitions &schema, const QString &type, QByteArrayView bytes)
{
    const auto fields = schema.constFind(type);
    if (fields == schema.cend()) return {};
    for (const auto &field : *fields) {
        if (field.count != -2 || qualifiedType(type, field.type) != "std_msgs/Header") continue;
        try {
            Reader first(bytes), second(bytes);
            const auto sec = selectScalar(first, schema, type, parseSelection(field.name + ".stamp.sec")).value.toInteger();
            const auto ns = selectScalar(second, schema, type, parseSelection(field.name + ".stamp.nsec")).value.toInteger();
            if (sec >= 0 && ns >= 0 && ns < 1'000'000'000 && (sec || ns))
                return QString::number(sec * 1'000'000'000 + ns);
        } catch (const std::runtime_error &) { return {}; }
        return {};
    }
    return {};
}

void collectNumeric(const QJsonValue &value, const QString &path, QVariantList &result)
{
    if (value.isDouble() || value.isBool()) {
        result.append(QVariantMap{{"path", path}, {"value", value.isBool() ? double(value.toBool()) : value.toDouble()},
                                  {"exactValue", value.isBool() ? (value.toBool() ? "1" : "0") : QString::number(value.toDouble(), 'g', 17)}});
    } else if (value.isString()) {
        bool ok = false;
        const auto number = value.toString().toDouble(&ok);
        if (ok && std::isfinite(number)) result.append(QVariantMap{{"path", path}, {"value", number}, {"exactValue", value.toString()}});
    } else if (value.isArray()) {
        const auto array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) collectNumeric(array[i], path + '[' + QString::number(i) + ']', result);
    } else if (value.isObject()) {
        const auto object = value.toObject();
        if (object.contains("total") && object.contains("offset") && object.value("items").isArray()) {
            const auto array = object.value("items").toArray();
            const auto offset = object.value("offset").toInteger();
            for (qsizetype i = 0; i < array.size(); ++i) collectNumeric(array[i], path + '[' + QString::number(offset + i) + ']', result);
        } else for (auto i = object.begin(); i != object.end(); ++i)
            collectNumeric(i.value(), path.isEmpty() ? i.key() : path + '.' + i.key(), result);
    }
}

struct DecodePage {
    quint32 offset = 0;
    quint32 largestArray = 0;
    qsizetype remaining = 12'000;
};

QJsonValue decodeValue(Reader &input, const Definitions &schema, const QString &type, int depth,
                       DecodePage &page, bool collect)
{
    if (depth > 24)
        throw std::runtime_error("ROS schema nesting is too deep.");
    if (type == "bool") return input.number<quint8>() != 0;
    if (type == "char" || type == "uint8") return int(input.number<quint8>());
    if (type == "byte" || type == "int8") return int(input.number<qint8>());
    if (type == "uint16") return int(input.number<quint16>());
    if (type == "int16") return int(input.number<qint16>());
    if (type == "uint32") return qint64(input.number<quint32>());
    if (type == "int32") return input.number<qint32>();
    if (type == "uint64") return QString::number(input.number<quint64>());
    if (type == "int64") return QString::number(input.number<qint64>());
    if (type == "float32") return finite(std::bit_cast<float>(input.number<quint32>()));
    if (type == "float64") return finite(std::bit_cast<double>(input.number<quint64>()));
    if (type == "time") return QJsonObject{{"sec", qint64(input.number<quint32>())}, {"nsec", qint64(input.number<quint32>())}};
    if (type == "duration") return QJsonObject{{"sec", input.number<qint32>()}, {"nsec", input.number<qint32>()}};
    if (type == "string") {
        const auto bytes = input.blob();
        return collect ? QJsonValue(QString::fromUtf8(bytes.first(std::min(bytes.size(), MaxFieldCharacters)))) : QJsonValue();
    }

    const auto fields = schema.constFind(type);
    if (fields == schema.end())
        throw std::runtime_error("ROS type is absent from the recorded schema.");
    QJsonObject object;
    for (const auto &field : *fields) {
        const auto fieldType = fixedSize(field.type) || field.type == "string" || field.type.contains('/')
            ? field.type : type.section('/', 0, -2) + '/' + field.type;
        const bool output = collect && page.remaining > 0;
        if (field.count == -2) {
            if (output)
                --page.remaining;
            auto value = decodeValue(input, schema, fieldType, depth + 1, page, output);
            if (output)
                object.insert(field.name, value);
            continue;
        }
        const quint32 count = field.count < 0 ? input.number<quint32>() : quint32(field.count);
        const auto width = fixedSize(fieldType);
        if (!width && count > 1'000'000)
            throw std::runtime_error("ROS message contains too many nested values.");
        const auto start = count > ArrayPage ? std::min(count, page.offset) : 0u;
        const auto stop = start + std::min(count - start, ArrayPage);
        QJsonArray items;
        quint32 index = 0;
        if (output) {
            page.largestArray = std::max(page.largestArray, count);
            if (width)
                input.take(qsizetype(start) * width);
            else
                for (; index < start; ++index)
                    decodeValue(input, schema, fieldType, depth + 1, page, false);
            index = start;
            for (; index < stop && page.remaining > 0; ++index) {
                --page.remaining;
                items.append(decodeValue(input, schema, fieldType, depth + 1, page, true));
            }
        }
        if (width)
            input.take(qsizetype(count - index) * width);
        else
            for (; index < count; ++index)
                decodeValue(input, schema, fieldType, depth + 1, page, false);
        if (output) {
            if (start > 0 || items.size() < count)
                object.insert(field.name, QJsonObject{{"total", qint64(count)}, {"offset", qint64(start)}, {"items", items}});
            else
                object.insert(field.name, items);
        }
    }
    return object;
}

QImage decodeImage(const QString &type, QByteArrayView bytes)
{
    if (type != "sensor_msgs/Image" && type != "sensor_msgs/CompressedImage")
        return {};
    Reader reader(bytes);
    reader.take(12); // Header sequence and time.
    reader.blob(); // Header frame.
    if (type == "sensor_msgs/CompressedImage") {
        reader.blob(); // format
        QBuffer buffer;
        buffer.setData(reader.blob().toByteArray());
        if (!buffer.open(QIODevice::ReadOnly))
            return {};
        QImageReader decoder(&buffer);
        const auto size = decoder.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 32'000'000)
            return {};
        decoder.setScaledSize(size.scaled(960, 540, Qt::KeepAspectRatio));
        return decoder.read();
    }

    const auto height = reader.number<quint32>(), width = reader.number<quint32>();
    const QString encoding = QString::fromUtf8(reader.blob()).toLower();
    reader.take(1); // Byte order is irrelevant for the eight-bit formats.
    const auto stride = reader.number<quint32>();
    const auto data = reader.blob();
    if (!width || !height || quint64(width) * height > 32'000'000 || stride > std::numeric_limits<int>::max() ||
        quint64(height) * stride > quint64(data.size()))
        return {};
    QImage::Format format = QImage::Format_Invalid;
    int channels = 0;
    if (encoding == "mono8" || encoding == "8uc1") { format = QImage::Format_Grayscale8; channels = 1; }
    else if (encoding == "rgb8") { format = QImage::Format_RGB888; channels = 3; }
    else if (encoding == "bgr8") { format = QImage::Format_BGR888; channels = 3; }
    else if (encoding == "rgba8" || encoding == "bgra8") { format = QImage::Format_RGBA8888; channels = 4; }
    if (!channels || stride < width * channels)
        return {};
    QImage image(reinterpret_cast<const uchar *>(data.data()), int(width), int(height), qsizetype(stride), format);
    if (encoding == "bgra8")
        image = image.rgbSwapped();
    if (width > 960 || height > 540)
        return image.scaled(960, 540, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image.copy();
}
}

TopicSchema describeTopicSchema(const QByteArray &type, const QByteArray &definition)
{
    TopicSchema result;
    result.definitionAvailable = !definition.trimmed().isEmpty();
    const auto root = QString::fromUtf8(type);
    if (!result.definitionAvailable && root != "std_msgs/Header") {
        result.status = QStringLiteral("Bag connection has no message_definition; raw bytes remain available.");
        return result;
    }
    try {
        const auto schema = parseDefinition(root, definition);
        QStringList errors;
        result.fields = schemaFields(schema, root, {}, result.numericFields, errors);
        errors.removeDuplicates();
        result.parseable = errors.isEmpty();
        result.status = errors.isEmpty() ? QStringLiteral("Recorded ROS1 definition is available.") : errors.join(' ');
    } catch (const std::runtime_error &error) {
        result.status = QString::fromUtf8(error.what());
    }
    return result;
}

std::optional<std::pair<qint64, qint64>> topicMessageHeaderRange(const QByteArray &type,
    const QByteArray &definition, QByteArrayView bytes)
{
    try {
        const auto root = QString::fromUtf8(type);
        const auto &schema = cachedDefinition(root, definition);
        Reader reader(bytes);
        std::optional<std::pair<qint64, qint64>> range;
        collectHeaderRange(reader, schema, root, 0, range);
        return range;
    } catch (const std::runtime_error &) { return {}; }
}

QString topicMessageHeaderStamp(const QByteArray &type, const QByteArray &definition, QByteArrayView bytes)
{
    const auto root = QString::fromUtf8(type);
    try {
        const auto &schema = cachedDefinition(root, definition);
        const auto direct = headerTimestamp(schema, root, bytes);
        if (!direct.isEmpty()) return direct;
        if (const auto range = topicMessageHeaderRange(type, definition, bytes); range && range->first == range->second)
            return QString::number(range->first);
    } catch (const std::runtime_error &) {}
    return {};
}

QString decodeTopicString(const QByteArray &type, const QByteArray &definition,
                          QByteArrayView bytes, const QString &fieldPath)
{
    const auto root = QString::fromUtf8(type);
    const auto &schema = cachedDefinition(root, definition);
    Reader reader(bytes);
    const auto scalar = selectScalar(reader, schema, root, parseSelection(fieldPath), 0, 0, true);
    if (scalar.type != "string") throw std::runtime_error("Selected ROS field is not a string.");
    return scalar.value.toString();
}

std::optional<TopicNumericValue> decodeTopicNumeric(const QByteArray &type, const QByteArray &definition,
                                                    QByteArrayView bytes, const QString &fieldPath)
{
    const auto root = QString::fromUtf8(type);
    const auto &schema = cachedDefinition(root, definition);
    Reader reader(bytes);
    const auto scalar = selectScalar(reader, schema, root, parseSelection(fieldPath));
    bool ok = true;
    const auto value = scalar.value.isString() ? scalar.value.toString().toDouble(&ok) :
        scalar.value.isBool() ? double(scalar.value.toBool()) : scalar.value.toDouble();
    if (!ok || !std::isfinite(value)) return {};
    const auto exact = scalar.value.isString() ? scalar.value.toString() :
        scalar.value.isBool() ? (scalar.value.toBool() ? QStringLiteral("1") : QStringLiteral("0")) :
        QString::number(value, 'g', 17);
    return TopicNumericValue{value, exact, scalar.type, topicMessageHeaderStamp(type, definition, bytes)};
}

TopicMessage decodeTopicMessage(const QByteArray &type, const QByteArray &definition, QByteArrayView bytes, qint64 stamp,
                                int arrayPage)
{
    TopicMessage result{QString::fromUtf8(type), {}, {}, stamp};
    result.serializedBytes = bytes.size();
    result.rawHex = QString::fromLatin1(bytes.first(std::min(bytes.size(), qsizetype(512))).toByteArray().toHex(' '));
    try {
        result.image = decodeImage(result.type, bytes);
    } catch (const std::runtime_error &) {
        // Keep the serialized fields available if the image preview cannot be decoded.
    }
    try {
        const auto schema = parseDefinition(result.type, definition);
        Reader reader(bytes);
        DecodePage page{quint32(std::clamp<qint64>(qint64(arrayPage) * ArrayPage, 0,
                                                  std::numeric_limits<quint32>::max()))};
        const auto parsed = decodeValue(reader, schema, result.type, 0, page, true);
        result.arrayPageCount = std::max(1, int((quint64(page.largestArray) + ArrayPage - 1) / ArrayPage));
        result.fields = QString::fromUtf8(QJsonDocument(parsed.toObject()).toJson(QJsonDocument::Indented));
        result.structuredFields = parsed.toObject().toVariantMap();
        result.headerStampNs = topicMessageHeaderStamp(type, definition, bytes);
        collectNumeric(parsed, {}, result.numericFields);
        const auto description = describeTopicSchema(type, definition);
        static const QRegularExpression indices(QStringLiteral("\\[[0-9]+\\]"));
        result.numericFields.removeIf([&](const QVariant &value) {
            auto path = value.toMap().value("path").toString();
            path.replace(indices, "[]");
            return !description.numericFields.contains(path);
        });
        result.decodeStatus = QStringLiteral("Decoded recorded ROS1 schema. Display arrays are paged, strings are limited to 4000 characters, raw hex previews the first 512 bytes; stampNs uses bag-storage time.");
    } catch (const std::runtime_error &error) {
        result.decodeStatus = QString::fromUtf8(error.what());
        if (definition.trimmed().isEmpty())
            result.decodeStatus = QStringLiteral("Bag connection has no message_definition; fields cannot be decoded.");
        result.fields = QStringLiteral("原始消息 · %1 字节\n%2\n%3").arg(bytes.size()).arg(result.decodeStatus, result.rawHex);
    }
    return result;
}
}
