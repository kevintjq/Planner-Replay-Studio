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
            continue;
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

QJsonValue decodeValue(Reader &input, const Definitions &schema, const QString &type, int depth,
                       qsizetype &remaining, bool collect)
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
        const bool output = collect && remaining > 0;
        if (field.count == -2) {
            if (output)
                --remaining;
            auto value = decodeValue(input, schema, fieldType, depth+1, remaining, output);
            if (output)
                object.insert(field.name, value);
            continue;
        }
        const quint32 count = field.count < 0 ? input.number<quint32>() : quint32(field.count);
        const auto width = fixedSize(fieldType);
        if (!width && count > 1'000'000)
            throw std::runtime_error("ROS message contains too many nested values.");
        QJsonArray items;
        quint32 index = 0;
        for (; output && index < count && index < ArrayPage && remaining > 0; ++index) {
            --remaining;
            items.append(decodeValue(input, schema, fieldType, depth+1, remaining, true));
        }
        if (width) {
            input.take(qsizetype(count - index) * width);
        } else {
            for (auto i = index; i < count; ++i)
                decodeValue(input, schema, fieldType, depth+1, remaining, false);
        }
        if (output) {
            if (index < count)
                object.insert(field.name, QJsonObject{{"total", qint64(count)}, {"firstItems", items}});
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

TopicMessage decodeTopicMessage(const QByteArray &type, const QByteArray &definition, QByteArrayView bytes, qint64 stamp)
{
    TopicMessage result{QString::fromUtf8(type), {}, {}, stamp};
    try {
        result.image = decodeImage(result.type, bytes);
    } catch (const std::runtime_error &) {
        // Keep the serialized fields available if the image preview cannot be decoded.
    }
    try {
        const auto schema = parseDefinition(result.type, definition);
        Reader reader(bytes);
        qsizetype remaining = 12'000;
        const auto parsed = decodeValue(reader, schema, result.type, 0, remaining, true);
        result.fields = QString::fromUtf8(QJsonDocument(parsed.toObject()).toJson(QJsonDocument::Indented));
    } catch (const std::runtime_error &) {
        const auto prefix = bytes.first(std::min(bytes.size(), qsizetype(512))).toByteArray().toHex(' ');
        result.fields = QStringLiteral("原始消息 · %1 字节\n%2").arg(bytes.size()).arg(QString::fromLatin1(prefix));
    }
    return result;
}
}
