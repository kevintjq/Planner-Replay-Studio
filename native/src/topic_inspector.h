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
    Q_PROPERTY(int arrayPage READ arrayPage NOTIFY changed)
    Q_PROPERTY(int arrayPageCount READ arrayPageCount NOTIFY changed)
public:
    TopicInspector(ReplayController &controller, PreviewImages &images);
    ~TopicInspector() override;

    QStringList topics() const;
    bool working() const { return working_; }
    QString status() const { return status_; }
    QString fields() const { return fields_; }
    QUrl preview() const;
    int arrayPage() const { return page_; }
    int arrayPageCount() const { return pageCount_; }
    Q_INVOKABLE void inspect(const QString &topic);
    Q_INVOKABLE void setArrayPage(int page);
    Q_INVOKABLE void cancel();

signals:
    void changed();

private:
    struct Result {
        std::optional<replay::TopicMessage> message;
        QString error;
    };
    void read(int page);

    ReplayController &replay_;
    PreviewImages &images_;
    QFutureWatcher<Result> watcher_;
    std::shared_ptr<const replay::Recording> current_;
    std::atomic_bool cancelled_ = false;
    bool working_ = false, hasImage_ = false;
    quint64 revision_ = 0;
    QString status_, fields_, topic_;
    qint64 stamp_ = 0;
    int page_ = 0, pendingPage_ = 0, pageCount_ = 1;
};
