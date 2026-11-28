#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QSettings;

// Discovers canonical stable releases. It never downloads or installs software.
class UpdateManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool automaticCheck READ automaticCheck WRITE setAutomaticCheck NOTIFY automaticCheckChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY stateChanged)
    Q_PROPERTY(bool notificationVisible READ notificationVisible NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    Q_PROPERTY(QDateTime lastChecked READ lastChecked NOTIFY stateChanged)
public:
    explicit UpdateManager(const QVariantMap &metadata, QObject *parent = nullptr);
    ~UpdateManager() override;

    // Borrowed dependencies allow deterministic offline integration tests. They
    // must outlive the manager; the regular constructor owns its dependencies.
    struct Services {
        QNetworkAccessManager *network = nullptr;
        QSettings *settings = nullptr;
        std::function<QDateTime()> now;
        int timeoutMs = 15000;
        int automaticDelayMs = 2000;
    };
    UpdateManager(const QVariantMap &metadata, Services services, QObject *parent = nullptr);

    QString status() const { return status_; }
    bool busy() const { return busy_; }
    bool automaticCheck() const { return automaticCheck_; }
    void setAutomaticCheck(bool enabled);
    bool updateAvailable() const { return updateAvailable_; }
    QString latestVersion() const { return latestVersion_; }
    QString releaseNotes() const { return releaseNotes_; }
    QString releaseUrl() const { return releaseUrl_; }
    bool notificationVisible() const { return notificationVisible_; }
    QString errorString() const { return errorString_; }
    QDateTime lastChecked() const { return lastChecked_; }

    Q_INVOKABLE void checkForUpdates(bool manual = true);
    Q_INVOKABLE void startAutomaticCheck();
    Q_INVOKABLE void skipVersion();
    Q_INVOKABLE void remindLater();
    Q_INVOKABLE bool openReleasePage();

signals:
    void stateChanged();
    void automaticCheckChanged();

private:
    struct Version {
        QString tag;
        QStringList components;
    };
    struct Release {
        Version version;
        QString notes;
        QString url;
    };
    static std::optional<Version> parseVersion(const QString &tag);
    static int compareVersions(const Version &left, const Version &right);
    QDateTime now() const;
    QString repositoryWebUrl() const;
    QUrl pageUrl(int page) const;
    bool validPageUrl(const QUrl &url, int page) const;
    void requestPage(int page);
    void consumeReply(QNetworkReply *reply);
    void finishPage(QNetworkReply *reply);
    void finishSuccess();
    void fail(const QString &reason);
    void disposeReply();
    void recordCompletedCheck();
    bool shouldNotify() const;

    QPointer<QNetworkAccessManager> network_;
    QPointer<QSettings> settings_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    QTimer automaticTimer_;
    std::function<QDateTime()> clock_;
    QString repository_;
    QString installedVersionText_;
    std::optional<Version> installedVersion_;
    std::optional<Release> candidate_;
    QByteArray response_;
    qint64 receivedBytes_ = 0;
    int page_ = 0;
    int timeoutMs_ = 15000;
    int automaticDelayMs_ = 2000;
    QString status_;
    QString errorString_;
    QString latestVersion_;
    QString releaseNotes_;
    QString releaseUrl_;
    QDateTime lastChecked_;
    bool validRepository_ = false;
    bool busy_ = false;
    bool manual_ = false;
    bool automaticCheck_ = true;
    bool automaticStarted_ = false;
    bool updateAvailable_ = false;
    bool notificationVisible_ = false;
};
