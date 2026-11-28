#include "update_manager.h"

#include <QDesktopServices>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QDebug>
#include <cstring>
#include <memory>

namespace {
int failures = 0;
int assertions = 0;
bool check(bool passed, const char *message)
{
    ++assertions;
    if (!passed) {
        ++failures;
        qCritical().noquote() << message;
    }
    return passed;
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 2000)
{
    if (predicate()) return true;
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (predicate()) loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(1);
    timeout.start(timeoutMs);
    loop.exec();
    return predicate();
}

QVariantMap metadata()
{
    return {{"version", "0.1.1"}, {"appName", "UAV Studio"}, {"repositoryFullName", "kevintjq/uav-studio"},
        {"releasesUrl", "https://github.com/kevintjq/uav-studio/releases"}};
}

QJsonObject release(const QString &tag, bool prerelease = false, bool draft = false,
    const QString &notes = QStringLiteral("Real release notes\n实际发行说明"))
{
    return {{"tag_name", tag}, {"draft", draft}, {"prerelease", prerelease}, {"body", notes},
        {"html_url", "https://github.com/kevintjq/uav-studio/releases/tag/" + tag}};
}

struct Response {
    QByteArray body = "[]";
    int code = 200;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    QByteArray links;
    int delayMs = 0;
};

Response jsonResponse(const QJsonArray &releases, int delayMs = 0, const QByteArray &links = {})
{
    Response response;
    response.body = QJsonDocument(releases).toJson(QJsonDocument::Compact);
    response.delayMs = delayMs;
    response.links = links;
    return response;
}

class FixtureReply final : public QNetworkReply {
public:
    FixtureReply(const QNetworkRequest &request, Response response, int *abortCount, QObject *parent)
        : QNetworkReply(parent), bytes_(std::move(response.body)), abortCount_(abortCount)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.code);
        if (!response.links.isEmpty()) setRawHeader("Link", response.links);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(response.delayMs, this, [this, response] {
            if (isFinished()) return;
            available_ = true;
            if (response.error != NoError) setError(response.error, QStringLiteral("fixture network error"));
            emit readyRead();
            if (isFinished()) return;
            setFinished(true);
            if (response.error != NoError) emit errorOccurred(response.error);
            emit finished();
        });
    }
    void abort() override
    {
        if (isFinished()) return;
        ++*abortCount_;
        setError(OperationCanceledError, QStringLiteral("fixture aborted"));
        setFinished(true);
        emit errorOccurred(OperationCanceledError);
        emit finished();
    }
    qint64 bytesAvailable() const override
    {
        return (available_ ? bytes_.size() - offset_ : 0) + QNetworkReply::bytesAvailable();
    }
protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        if (!available_) return 0;
        const auto size = std::min(maximum, qint64(bytes_.size()) - offset_);
        if (!size) return isFinished() ? -1 : 0;
        std::memcpy(data, bytes_.constData() + offset_, size_t(size));
        offset_ += size;
        return size;
    }
private:
    QByteArray bytes_;
    qint64 offset_ = 0;
    int *abortCount_;
    bool available_ = false;
};

class FixtureNetwork final : public QNetworkAccessManager {
public:
    QList<Response> responses;
    QList<QNetworkRequest> requests;
    int abortCount = 0;
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
    {
        check(operation == GetOperation, "Updater must only issue GET requests");
        requests.push_back(request);
        if (responses.isEmpty()) {
            check(false, "Unexpected request: offline fixture exhausted");
            Response error;
            error.code = 0;
            error.error = QNetworkReply::HostNotFoundError;
            return new FixtureReply(request, error, &abortCount, this);
        }
        return new FixtureReply(request, responses.takeFirst(), &abortCount, this);
    }
};

struct Context {
    QTemporaryDir directory;
    QSettings settings{directory.filePath("settings.ini"), QSettings::IniFormat};
    FixtureNetwork network;
    QDateTime time = QDateTime::fromString("2026-11-28T12:00:00Z", Qt::ISODate);
    std::unique_ptr<UpdateManager> manager;
    void create(int timeoutMs = 500, int delayMs = 0, const QVariantMap &identity = metadata())
    {
        manager.reset();
        UpdateManager::Services services;
        services.network = &network;
        services.settings = &settings;
        services.now = [this] { return time; };
        services.timeoutMs = timeoutMs;
        services.automaticDelayMs = delayMs;
        manager = std::make_unique<UpdateManager>(identity, services);
    }
    void run(const Response &response, bool manual = true)
    {
        network.responses.push_back(response);
        manager->checkForUpdates(manual);
        check(waitUntil([this] { return !manager->busy(); }), "Update request did not complete");
    }
};

void stableVersionsAndPagination()
{
    Context context;
    context.create();
    QJsonArray first{
        release("studio-v999.0.0"), release("native-v999.0.0"), release("v999.0.0", true),
        release("v888.0.0", false, true), release("v01.0.0"), release("v2.0.0-rc.1"),
        release("v2.0.0+build"), release("2.0.0"), release("v2.0"), release("v2.0.0\n"), release("v1.9.0")};
    auto absentFlags = release("v777.0.0");
    absentFlags.remove("draft");
    first.append(absentFlags);
    auto wrongFlagType = release("v555.0.0");
    wrongFlagType["prerelease"] = "false";
    first.append(wrongFlagType);
    auto foreign = release("v444.0.0");
    foreign["html_url"] = "https://github.com/elsewhere/product/releases/tag/v444.0.0";
    first.append(foreign);
    const auto notes = QStringLiteral("Notes stay plain: <script>no execution</script>\n中英说明");
    context.network.responses = {
        jsonResponse(first, 0, "<https://api.github.com/repos/kevintjq/uav-studio/releases?page=2&per_page=100>; rel=\"next\""),
        jsonResponse({release("v1.10.0", false, false, notes), release("v1.8.0")})};
    context.manager->checkForUpdates();
    check(context.manager->busy() && context.network.requests.size() == 1, "Check must return while the network request is pending");
    check(waitUntil([&] { return !context.manager->busy(); }), "Paginated check did not finish");
    check(context.network.requests.size() == 2, "All release pages must be checked");
    check(context.manager->updateAvailable() && context.manager->latestVersion() == "v1.10.0", "Maximum canonical stable semantic version was not selected");
    check(context.manager->notificationVisible() && context.manager->errorString().isEmpty(), "Valid new stable release should notify");
    check(context.manager->releaseNotes() == notes, "Release notes were modified or truncated");
    check(context.manager->releaseUrl() == "https://github.com/kevintjq/uav-studio/releases/tag/v1.10.0", "Release URL must stay inside configured repository");
    check(context.manager->lastChecked() == context.time, "Completed check timestamp was not recorded");
    for (const auto &request : context.network.requests) {
        check(request.url().scheme() == "https" && request.url().host() == "api.github.com", "Update request escaped HTTPS GitHub API");
        check(request.rawHeader("Accept") == "application/vnd.github+json"
            && request.rawHeader("X-GitHub-Api-Version") == "2022-11-28", "GitHub API headers are missing");
        check(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() == QNetworkRequest::ManualRedirectPolicy,
            "Updater must refuse cross-endpoint redirects");
    }

    context.run(jsonResponse({release("v123456789012345678901234567890.0.0"), release("v99999999999999999999999999999.0.0")}));
    check(context.manager->latestVersion() == "v123456789012345678901234567890.0.0", "Large semantic components must not overflow numeric comparisons");
}

void reminderPersistence()
{
    Context context;
    context.create();
    const auto response = jsonResponse({release("v1.0.0")});
    context.run(response);
    context.manager->skipVersion();
    check(!context.manager->notificationVisible() && context.manager->updateAvailable(), "Skipping must hide notification but preserve About release details");
    context.create();
    check(context.manager->lastChecked() == context.time, "Check timestamp did not survive manager restart");
    context.run(response, false);
    check(context.manager->updateAvailable() && !context.manager->notificationVisible(), "Skipped stable version was not suppressed after restart");
    context.run(response, true);
    check(context.manager->notificationVisible(), "Manual check must show a previously skipped version");
    context.run(jsonResponse({release("v1.0.1")}), false);
    check(context.manager->notificationVisible(), "Skipping one version must not suppress a newer release");
    context.manager->remindLater();
    check(context.manager->updateAvailable() && !context.manager->notificationVisible(), "Remind later must preserve available release");
    context.time = context.time.addSecs(23 * 3600);
    context.create();
    context.run(jsonResponse({release("v1.0.1")}), false);
    check(!context.manager->notificationVisible(), "Remind-later preference must survive restart for 24 hours");
    context.run(jsonResponse({release("v1.0.1")}), true);
    check(context.manager->notificationVisible(), "Manual check must override remind-later suppression");
    context.time = context.time.addSecs(2 * 3600);
    context.run(jsonResponse({release("v1.0.1")}), false);
    check(context.manager->notificationVisible(), "Expired remind-later deadline must restore notification");
    context.manager->setAutomaticCheck(false);
    context.settings.sync();
    QSettings persisted(context.directory.filePath("settings.ini"), QSettings::IniFormat);
    check(!persisted.value("Updates/automaticCheck", true).toBool()
        && persisted.value("Updates/skippedVersion").toString() == "v1.0.0"
        && persisted.value("Updates/remindVersion").toString() == "v1.0.1"
        && persisted.value("Updates/remindUntil").toDateTime().isValid(), "Reminder and automatic preferences must be present in persistent settings file");
    context.create();
    check(!context.manager->automaticCheck(), "Automatic check preference did not persist");
    const auto before = context.network.requests.size();
    context.manager->startAutomaticCheck();
    context.manager->checkForUpdates(false);
    QCoreApplication::processEvents();
    check(context.network.requests.size() == before, "Disabled automatic checks must not issue network requests");
    context.run(response, true);
    check(context.manager->notificationVisible(), "Disabled auto-check must not disable explicit manual discovery");
}

void startupAndResponsiveness()
{
    Context context;
    context.create(1000);
    context.network.responses.push_back(jsonResponse({release("v1.0.0")}, 120));
    context.settings.setValue("Updates/skippedVersion", "v1.0.0");
    int heartbeat = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; });
    timer.start(5);
    context.manager->startAutomaticCheck();
    context.manager->startAutomaticCheck();
    check(context.network.requests.isEmpty(), "Startup must schedule asynchronous discovery");
    check(waitUntil([&] { return context.manager->busy(); }), "Automatic check did not start");
    context.manager->checkForUpdates(true);
    check(waitUntil([&] { return !context.manager->busy(); }), "Delayed update did not finish");
    check(heartbeat >= 5 && context.network.requests.size() == 1, "Network wait blocked event loop or concurrent checks duplicated requests");
    check(context.manager->notificationVisible(), "Manual request sharing an automatic request must override skip suppression");

    Context disabledDuringDelay;
    disabledDuringDelay.create(500, 25);
    disabledDuringDelay.manager->startAutomaticCheck();
    disabledDuringDelay.manager->setAutomaticCheck(false);
    QEventLoop loop;
    QTimer::singleShot(40, &loop, &QEventLoop::quit);
    loop.exec();
    check(disabledDuringDelay.network.requests.isEmpty(), "Turning off automatic discovery must cancel scheduled startup request");

    Context manualDuringDelay;
    manualDuringDelay.create(500, 25);
    manualDuringDelay.manager->startAutomaticCheck();
    manualDuringDelay.run(jsonResponse({release("v1.0.0")}));
    QTimer::singleShot(40, &loop, &QEventLoop::quit);
    loop.exec();
    check(manualDuringDelay.network.requests.size() == 1, "Manual startup discovery must cancel a redundant scheduled automatic request");
}

void errorAndTimeout()
{
    Context context;
    context.create();
    context.run(jsonResponse({release("v1.0.0")}));
    const auto knownUrl = context.manager->releaseUrl();
    Response error;
    error.code = 403;
    context.run(error);
    check(context.manager->errorString().contains("403") && !context.manager->notificationVisible(), "Rate-limit failure must be reported without forcing a notification");
    check(context.manager->updateAvailable() && context.manager->releaseUrl() == knownUrl, "Network error must preserve previously discovered valid release");
    error.code = 0;
    error.error = QNetworkReply::HostNotFoundError;
    context.run(error);
    check(!context.manager->errorString().isEmpty() && !context.manager->busy(), "Offline failure must finish and expose status");
    error = Response{};
    error.body = "{broken-json";
    context.run(error);
    check(!context.manager->errorString().isEmpty(), "Malformed JSON must not be treated as an up-to-date response");
    error.body = "{}";
    context.run(error);
    check(!context.manager->errorString().isEmpty(), "Non-array API body must be rejected");
    error = Response{};
    error.code = 302;
    context.run(error);
    check(!context.manager->errorString().isEmpty(), "Redirected repository must require metadata correction");
    const auto before = context.network.requests.size();
    context.run(jsonResponse({release("v1.0.0")}, 0, "<https://untrusted.invalid/releases?page=2&per_page=100>; rel=\"next\""));
    check(!context.manager->errorString().isEmpty() && context.network.requests.size() == before + 1,
        "Unsafe pagination must not issue a request outside configured repository");
    context.run(jsonResponse({release("v1.0.0")}, 0, "<https://api.github.com/repos/kevintjq/uav-studio/releases?page=3&per_page=100>; rel=\"next\""));
    check(!context.manager->errorString().isEmpty(), "Pagination must not skip a release page");
    error = Response{};
    error.body = QByteArray(8 * 1024 * 1024 + 1, ' ');
    context.run(error);
    check(context.manager->errorString().contains("large"), "Oversized network response must be bounded before JSON parsing");

    Context timeout;
    timeout.create(35);
    auto neverReady = jsonResponse({release("v1.0.0")}, 1000);
    int heartbeat = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; });
    timer.start(2);
    timeout.run(neverReady);
    check(timeout.manager->errorString().contains("timed out") && timeout.network.abortCount == 1,
        "Overall deadline must abort the network request and report timeout");
    check(heartbeat >= 2, "Event loop must remain responsive during timeout");
    timeout.run(jsonResponse({release("v1.0.0")}));
    check(timeout.manager->errorString().isEmpty() && timeout.manager->updateAvailable(), "Manual retry after timeout must recover");

    Context paginationTimeout;
    paginationTimeout.create(60);
    paginationTimeout.network.responses = {
        jsonResponse({release("v1.0.0")}, 40,
            "<https://api.github.com/repos/kevintjq/uav-studio/releases?per_page=100&page=2>; rel=\"next\""),
        jsonResponse({release("v2.0.0")}, 40)};
    paginationTimeout.manager->checkForUpdates();
    check(waitUntil([&] { return !paginationTimeout.manager->busy(); }), "Paginated overall deadline did not finish");
    check(paginationTimeout.manager->errorString().contains("timed out")
        && paginationTimeout.network.requests.size() == 2 && paginationTimeout.network.abortCount == 1
        && !paginationTimeout.manager->updateAvailable(), "Overall deadline must span all pages and never publish a partially checked candidate");

    Context destruction;
    destruction.create();
    destruction.network.responses.push_back(neverReady);
    destruction.manager->checkForUpdates();
    destruction.manager.reset();
    check(destruction.network.abortCount == 1, "Destroying updater must abort a pending request");
}

void noUpdateAndMetadata()
{
    Context context;
    context.create();
    context.run(jsonResponse({release("native-v9.0.0"), release("studio-v9.0.0"), release("v9.0.0", true)}));
    check(!context.manager->updateAvailable() && context.manager->latestVersion().isEmpty()
        && context.manager->errorString().isEmpty(), "Historical product and prerelease tags must not produce a current stable update");
    context.run(jsonResponse({release("v0.1.1"), release("v0.1.0")}));
    check(!context.manager->updateAvailable() && !context.manager->notificationVisible(), "Current version must not be offered as an update");
    context.run(jsonResponse({release("v0.1.0")}));
    check(!context.manager->updateAvailable(), "Older release must never trigger downgrade prompt");
    auto invalid = metadata();
    invalid["repositoryFullName"] = "kevintjq/uav-studio/../../untrusted";
    context.create(500, 0, invalid);
    const auto before = context.network.requests.size();
    context.manager->checkForUpdates();
    check(!context.manager->busy() && !context.manager->errorString().isEmpty() && context.network.requests.size() == before,
        "Invalid repository metadata must be rejected without network traffic");
    invalid = metadata();
    invalid["version"] = "0.1.1\n";
    context.create(500, 0, invalid);
    context.manager->checkForUpdates();
    check(!context.manager->errorString().isEmpty() && context.network.requests.size() == before,
        "Current metadata version must be canonical and exclude trailing newline");
}
}

class LinkReceiver final : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void receive(const QUrl &url) { urls.push_back(url); }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    stableVersionsAndPagination();
    reminderPersistence();
    startupAndResponsiveness();
    errorAndTimeout();
    noUpdateAndMetadata();

    LinkReceiver receiver;
    QDesktopServices::setUrlHandler("https", &receiver, "receive");
    Context links;
    links.create();
    check(links.manager->openReleasePage() && receiver.urls.size() == 1
        && receiver.urls.last().toString() == "https://github.com/kevintjq/uav-studio/releases", "Manual download entry must open configured repository's releases page");
    links.run(jsonResponse({release("v1.0.0")}));
    check(links.manager->openReleasePage() && receiver.urls.size() == 2
        && receiver.urls.last().toString() == "https://github.com/kevintjq/uav-studio/releases/tag/v1.0.0", "Available release entry must open validated canonical release URL");
    QDesktopServices::unsetUrlHandler("https");
    qInfo() << "Offline UpdateManager assertions:" << assertions << "failures:" << failures;
    return failures ? 1 : 0;
}
#include "update_manager_test.moc"
