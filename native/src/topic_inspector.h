#pragma once

#include "ros_topic_message.h"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <atomic>
#include <memory>
#include <optional>

class PreviewImages;
class ReplayController;
namespace replay { struct Recording; }

class TopicInspector final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList topics READ topics NOTIFY changed)
    Q_PROPERTY(bool working READ working NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString fields READ fields NOTIFY changed)
    Q_PROPERTY(QUrl preview READ preview NOTIFY changed)
public:
    TopicInspector(ReplayController &controller, PreviewImages &images);
    ~TopicInspector() override;

    QStringList topics() const;
    bool working() const { return working_; }
    QString status() const { return status_; }
    QString fields() const { return fields_; }
    QUrl preview() const;
    Q_INVOKABLE void inspect(const QString &topic);
    Q_INVOKABLE void cancel();

signals:
    void changed();

private:
    struct Result {
        std::optional<replay::TopicMessage> message;
        QString error;
    };
    ReplayController &replay_;
    PreviewImages &images_;
    QFutureWatcher<Result> watcher_;
    std::shared_ptr<const replay::Recording> current_;
    std::atomic_bool cancelled_ = false;
    bool working_ = false, hasImage_ = false;
    quint64 revision_ = 0;
    QString status_, fields_;
};
