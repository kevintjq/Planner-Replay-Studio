#pragma once

#include <QByteArray>
#include <QtEndian>
#include <array>
#include <stdexcept>

namespace replay::bridge {
inline const QByteArray Magic("PRS1\n");
constexpr int MaxMessage = 32 * 1024 * 1024;

inline QByteArray u32(quint32 value)
{
    std::array<char, 4> bytes;
    qToLittleEndian(value, bytes.data());
    return QByteArray(bytes.data(), int(bytes.size()));
}

inline QByteArray field(const QByteArray &bytes)
{
    return u32(quint32(bytes.size())) + bytes;
}

struct Message {
    quint64 time;
    QByteArray topic, type, definition, payload;
};

inline QByteArray encode(const Message &message)
{
    const qint64 length = 24LL + message.topic.size() + message.type.size() + message.definition.size() + message.payload.size();
    if (length > MaxMessage)
        throw std::runtime_error("Bridge message exceeds 32 MiB.");
    std::array<char, 8> time;
    qToLittleEndian(message.time, time.data());
    QByteArray frame;
    frame.reserve(int(length) + 4);
    frame += u32(quint32(length));
    frame.append(time.data(), int(time.size()));
    frame += field(message.topic);
    frame += field(message.type);
    frame += field(message.definition);
    frame += field(message.payload);
    return frame;
}

inline Message decode(const QByteArray &frame)
{
    if (frame.size() < 24 || frame.size() > MaxMessage)
        throw std::runtime_error("Invalid bridge frame size.");
    auto position = 8;
    const auto read = [&] {
        if (frame.size() - position < 4)
            throw std::runtime_error("Truncated bridge message.");
        const auto length = qFromLittleEndian<quint32>(frame.constData() + position);
        position += 4;
        if (length > quint32(frame.size() - position))
            throw std::runtime_error("Truncated bridge field.");
        auto bytes = frame.mid(position, int(length));
        position += int(length);
        return bytes;
    };
    Message message{qFromLittleEndian<quint64>(frame.constData()), read(), read(), read(), read()};
    if (position != frame.size() || !message.topic.startsWith('/') || message.topic.size() > 1024 ||
        message.type.isEmpty() || message.type.size() > 1024 || message.definition.size() > 512 * 1024)
        throw std::runtime_error("Invalid ROS bridge connection metadata.");
    return message;
}
}
