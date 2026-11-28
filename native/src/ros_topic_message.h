#pragma once

#include <QByteArrayView>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <optional>
#include <utility>

namespace replay {

struct TopicMessage {
    QString type;
    QString fields;
    QImage image;
    qint64 stamp = 0;
    int arrayPageCount = 1;
    QVariantMap structuredFields;
    QVariantList numericFields;
    QString decodeStatus;
    QString headerStampNs;
    QString timeBasis = QStringLiteral("bag-storage");
    QString rawHex;
    qint64 serializedBytes = 0;
    QString selectionTimeBasis = QStringLiteral("bag-storage");
    QString selectionStampNs;
};

struct TopicSchema {
    bool definitionAvailable = false;
    bool parseable = false;
    QVariantList fields;
    QStringList numericFields;
    QString status;
};

struct TopicNumericValue {
    double value = 0.;
    QString exactValue;
    QString type;
    QString headerStampNs;
};

TopicSchema describeTopicSchema(const QByteArray &type, const QByteArray &definition);
std::optional<std::pair<qint64, qint64>> topicMessageHeaderRange(const QByteArray &type,
    const QByteArray &definition, QByteArrayView bytes);
QString topicMessageHeaderStamp(const QByteArray &type, const QByteArray &definition, QByteArrayView bytes);
QString decodeTopicString(const QByteArray &type, const QByteArray &definition,
                          QByteArrayView bytes, const QString &fieldPath);
std::optional<TopicNumericValue> decodeTopicNumeric(const QByteArray &type, const QByteArray &definition,
                                                    QByteArrayView bytes, const QString &fieldPath);

TopicMessage decodeTopicMessage(const QByteArray &type, const QByteArray &definition,
                                QByteArrayView bytes, qint64 stamp, int arrayPage = 0);

}
