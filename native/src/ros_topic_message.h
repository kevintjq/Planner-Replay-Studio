#pragma once

#include <QByteArrayView>
#include <QImage>
#include <QString>

namespace replay {

struct TopicMessage {
    QString type;
    QString fields;
    QImage image;
    qint64 stamp = 0;
};

TopicMessage decodeTopicMessage(const QByteArray &type, const QByteArray &definition,
                                QByteArrayView bytes, qint64 stamp);

}
