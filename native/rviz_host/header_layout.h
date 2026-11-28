#pragma once

#include <QRegularExpression>
#include <QString>

namespace native_replay {

// ROS1 constants appear in the definition but consume no serialized bytes.
// Only the first actual field can establish a leading Header; nested message
// definitions and a Header following another field must never be accepted.
inline bool hasLeadingHeader(const QString &dataType, const QString &definition)
{
    if (dataType == "std_msgs/Header") return true;
    static const QRegularExpression constant(
        "^(?:bool|int8|uint8|byte|char|int16|uint16|int32|uint32|int64|uint64|float32|float64|string)"
        "\\s+[A-Za-z][A-Za-z0-9_]*\\s*=.*$");
    static const QRegularExpression header("^(?:std_msgs/)?Header\\s+[A-Za-z][A-Za-z0-9_]*$");
    for (auto line : definition.split('\n')) {
        line = line.section('#', 0, 0).trimmed();
        if (line.isEmpty() || constant.match(line).hasMatch()) continue;
        return header.match(line).hasMatch();
    }
    return false;
}

} // namespace native_replay
