#pragma once

#include "ros_topic_message.h"
#include "replay_data.h"

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
    Q_PROPERTY(QString selectedTopic READ selectedTopic NOTIFY changed)
    Q_PROPERTY(QString selectedType READ selectedType NOTIFY changed)
    Q_PROPERTY(bool working READ working NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString fields READ fields NOTIFY changed)
    Q_PROPERTY(QUrl preview READ preview NOTIFY changed)
    Q_PROPERTY(int arrayPage READ arrayPage NOTIFY changed)
    Q_PROPERTY(int arrayPageCount READ arrayPageCount NOTIFY changed)
    Q_PROPERTY(QVariantList catalog READ catalog NOTIFY catalogChanged)
    Q_PROPERTY(QStringList types READ types NOTIFY catalogChanged)
    Q_PROPERTY(bool catalogWorking READ catalogWorking NOTIFY catalogChanged)
    Q_PROPERTY(QString catalogStatus READ catalogStatus NOTIFY catalogChanged)
    Q_PROPERTY(QVariantMap structuredFields READ structuredFields NOTIFY changed)
    Q_PROPERTY(QVariantList numericFields READ numericFields NOTIFY changed)
    Q_PROPERTY(QString stampNs READ stampNs NOTIFY changed)
    Q_PROPERTY(QString headerStampNs READ headerStampNs NOTIFY changed)
    Q_PROPERTY(QString timeBasis READ timeBasis NOTIFY changed)
    Q_PROPERTY(QString decodeStatus READ decodeStatus NOTIFY changed)
    Q_PROPERTY(QString selectionTimeBasis READ selectionTimeBasis NOTIFY changed)
    Q_PROPERTY(QString selectionStampNs READ selectionStampNs NOTIFY changed)
    Q_PROPERTY(QString rawHex READ rawHex NOTIFY changed)
    Q_PROPERTY(QVariantList numericSamples READ numericSamples NOTIFY samplingChanged)
    Q_PROPERTY(QVariantMap statistics READ statistics NOTIFY samplingChanged)
    Q_PROPERTY(bool samplingWorking READ samplingWorking NOTIFY samplingChanged)
    Q_PROPERTY(QString samplingStatus READ samplingStatus NOTIFY samplingChanged)
public:
    TopicInspector(ReplayController &controller, PreviewImages &images);
    ~TopicInspector() override;

    QStringList topics() const;
    QString selectedTopic() const { return topic_; }
    QString selectedType() const { return type_; }
    bool working() const { return working_; }
    QString status() const { return status_; }
    QString fields() const { return fields_; }
    QUrl preview() const;
    int arrayPage() const { return page_; }
    int arrayPageCount() const { return pageCount_; }
    QVariantList catalog() const { return catalog_; }
    QStringList types() const { return types_; }
    bool catalogWorking() const { return catalogWorking_; }
    QString catalogStatus() const { return catalogStatus_; }
    QVariantMap structuredFields() const { return message_.structuredFields; }
    QVariantList numericFields() const { return message_.numericFields; }
    QString stampNs() const { return hasMessage_ ? QString::number(message_.stamp) : QString(); }
    QString headerStampNs() const { return message_.headerStampNs; }
    QString timeBasis() const { return message_.timeBasis; }
    QString decodeStatus() const { return message_.decodeStatus; }
    QString selectionTimeBasis() const { return message_.selectionTimeBasis; }
    QString selectionStampNs() const { return message_.selectionStampNs; }
    QString rawHex() const { return message_.rawHex; }
    QVariantList numericSamples() const { return series_.samples; }
    QVariantMap statistics() const { return series_.statistics; }
    bool samplingWorking() const { return samplingWorking_; }
    QString samplingStatus() const { return samplingStatus_; }
    Q_INVOKABLE void inspect(const QString &topic, const QString &type = {});
    Q_INVOKABLE void inspectAt(const QString &topic, const QString &storageStampNs, const QString &type = {});
    Q_INVOKABLE void openBag(const QUrl &url);
    Q_INVOKABLE void loadCatalog(const QUrl &url);
    Q_INVOKABLE void sampleNumeric(const QString &topic, const QString &fieldPath,
        const QString &timeBasis = QStringLiteral("storage"), const QString &firstStampNs = {},
        const QString &lastStampNs = {}, int maxSamples = 20000, const QString &type = {});
    Q_INVOKABLE void setArrayPage(int page);
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void catalogChanged();
    void samplingChanged();

private:
    struct Result {
        std::optional<replay::TopicMessage> message;
        QString error;
        quint64 generation = 0;
    };
    struct CatalogResult {
        std::vector<replay::TopicCatalogEntry> entries;
        QString error, path;
        quint64 generation = 0;
    };
    struct SamplingResult {
        replay::NumericTopicSeries series;
        QString error;
        quint64 generation = 0;
    };
    void read(int page);
    void refreshRecording();
    void setCatalog(const std::vector<replay::TopicCatalogEntry> &entries, const QString &path);
    void startCatalog();
    void clearMessage();
    QString sourcePath() const;

    ReplayController &replay_;
    PreviewImages &images_;
    QFutureWatcher<Result> watcher_;
    QFutureWatcher<CatalogResult> catalogWatcher_;
    QFutureWatcher<SamplingResult> samplingWatcher_;
    std::shared_ptr<const replay::Recording> current_;
    std::atomic_bool cancelled_ = false;
    std::atomic_bool catalogCancelled_ = false, samplingCancelled_ = false;
    bool working_ = false, hasImage_ = false;
    quint64 revision_ = 0;
    QString status_, fields_, topic_;
    qint64 stamp_ = 0;
    int page_ = 0, pendingPage_ = 0, pageCount_ = 1;
    bool catalogWorking_ = false, samplingWorking_ = false, hasMessage_ = false;
    quint64 generation_ = 0, catalogGeneration_ = 0, samplingGeneration_ = 0;
    QString catalogStatus_, samplingStatus_, catalogPath_, pendingCatalogPath_, pendingOpenPath_, type_, selectionBasis_ = QStringLiteral("storage");
    QVariantList catalog_;
    QStringList types_;
    replay::TopicMessage message_;
    replay::NumericTopicSeries series_;
};
