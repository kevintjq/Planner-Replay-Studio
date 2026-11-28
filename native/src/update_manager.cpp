#include "update_manager.h"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>
#include <algorithm>

namespace {
constexpr qint64 maximumPageBytes = 8 * 1024 * 1024;
constexpr qint64 maximumCheckBytes = 32 * 1024 * 1024;
constexpr int maximumPages = 100;
constexpr auto automaticKey = "Updates/automaticCheck";
constexpr auto skippedKey = "Updates/skippedVersion";
constexpr auto remindVersionKey = "Updates/remindVersion";
constexpr auto remindUntilKey = "Updates/remindUntil";
constexpr auto lastCheckedKey = "Updates/lastChecked";
}

UpdateManager::UpdateManager(const QVariantMap &metadata, QObject *parent)
    : UpdateManager(metadata, Services{}, parent)
{
}

UpdateManager::UpdateManager(const QVariantMap &metadata, Services services, QObject *parent)
    : QObject(parent),
      network_(services.network ? services.network : new QNetworkAccessManager(this)),
      settings_(services.settings ? services.settings : new QSettings(this)),
      clock_(services.now ? std::move(services.now) : [] { return QDateTime::currentDateTimeUtc(); }),
      repository_(metadata.value("repositoryFullName").toString()),
      installedVersionText_(metadata.value("version").toString()),
      installedVersion_(parseVersion("v" + installedVersionText_)),
      timeoutMs_(std::clamp(services.timeoutMs, 1, 120000)),
      automaticDelayMs_(std::clamp(services.automaticDelayMs, 0, 60000)),
      status_(QStringLiteral("Not checked · 尚未检查"))
{
    static const QRegularExpression repositoryPattern(
        QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9._-]{1,100}\\z"));
    const auto name = repository_.section('/', 1);
    validRepository_ = repositoryPattern.match(repository_).hasMatch() && name != "." && name != "..";
    automaticCheck_ = settings_->value(automaticKey, true).toBool();
    lastChecked_ = settings_->value(lastCheckedKey).toDateTime().toUTC();
    deadline_.setSingleShot(true);
    automaticTimer_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("Update check timed out. Try again later. · 更新检查超时，请稍后重试。"));
    });
    connect(&automaticTimer_, &QTimer::timeout, this, [this] {
        if (automaticCheck_) checkForUpdates(false);
    });
}

UpdateManager::~UpdateManager()
{
    disposeReply();
}

std::optional<UpdateManager::Version> UpdateManager::parseVersion(const QString &tag)
{
    // Compare decimal components as strings, avoiding integer overflow and
    // preserving SemVer's prohibition of leading zeros.
    static const QRegularExpression pattern(QStringLiteral("\\Av(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z"));
    const auto match = pattern.match(tag);
    if (!match.hasMatch()) return std::nullopt;
    return Version{tag, {match.captured(1), match.captured(2), match.captured(3)}};
}

int UpdateManager::compareVersions(const Version &left, const Version &right)
{
    for (int index = 0; index < 3; ++index) {
        const auto &a = left.components.at(index);
        const auto &b = right.components.at(index);
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        const auto order = QString::compare(a, b, Qt::CaseSensitive);
        if (order) return order < 0 ? -1 : 1;
    }
    return 0;
}

QDateTime UpdateManager::now() const { return clock_().toUTC(); }
QString UpdateManager::repositoryWebUrl() const { return "https://github.com/" + repository_; }

QUrl UpdateManager::pageUrl(int page) const
{
    QUrl url("https://api.github.com");
    url.setPath("/repos/" + repository_ + "/releases");
    QUrlQuery query;
    query.addQueryItem("per_page", "100");
    query.addQueryItem("page", QString::number(page));
    url.setQuery(query);
    return url;
}

bool UpdateManager::validPageUrl(const QUrl &url, int page) const
{
    const auto expected = pageUrl(page);
    const QUrlQuery query(url);
    return url.isValid() && url.scheme() == "https" && url.host() == "api.github.com"
        && url.port() == -1 && url.userInfo().isEmpty() && url.fragment().isEmpty()
        && url.path() == expected.path() && query.queryItems().size() == 2
        && query.queryItemValue("per_page") == "100" && query.queryItemValue("page") == QString::number(page);
}

void UpdateManager::setAutomaticCheck(bool enabled)
{
    if (automaticCheck_ == enabled) return;
    automaticCheck_ = enabled;
    if (settings_) settings_->setValue(automaticKey, enabled);
    if (!enabled) automaticTimer_.stop();
    emit automaticCheckChanged();
}

void UpdateManager::startAutomaticCheck()
{
    if (automaticStarted_) return;
    automaticStarted_ = true;
    if (automaticCheck_) automaticTimer_.start(automaticDelayMs_);
}

void UpdateManager::checkForUpdates(bool manual)
{
    if (!manual && !automaticCheck_) return;
    if (manual) automaticTimer_.stop();
    if (busy_) {
        // A manual request during an automatic request shares that response,
        // but explicitly restores notification even for a skipped version.
        manual_ = manual_ || manual;
        return;
    }
    manual_ = manual;
    errorString_.clear();
    notificationVisible_ = false;
    candidate_.reset();
    response_.clear();
    receivedBytes_ = 0;
    busy_ = true;
    status_ = QStringLiteral("Checking for updates… · 正在检查更新…");
    emit stateChanged();
    if (!validRepository_ || !installedVersion_) {
        fail(QStringLiteral("Application update metadata is invalid. · 应用更新元信息无效。"));
        return;
    }
    if (!network_ || !settings_) {
        fail(QStringLiteral("Update service is unavailable. · 更新服务不可用。"));
        return;
    }
    deadline_.start(timeoutMs_);
    requestPage(1);
}

void UpdateManager::requestPage(int page)
{
    page_ = page;
    response_.clear();
    QNetworkRequest request(pageUrl(page));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "UAV-Studio/" + installedVersionText_.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(timeoutMs_);
    auto *reply = network_->get(request);
    reply_ = reply;
    reply->setReadBufferSize(128 * 1024);
    connect(reply, &QIODevice::readyRead, this, [this, reply] { consumeReply(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishPage(reply); });
    // Dependency-injected replies may already have completed.
    if (reply->isFinished()) QTimer::singleShot(0, this, [this, reply] {
        if (reply_ == reply) finishPage(reply);
    });
}

void UpdateManager::consumeReply(QNetworkReply *reply)
{
    if (reply_ != reply) return;
    while (reply->bytesAvailable() > 0) {
        const auto bytes = reply->read(std::min<qint64>(64 * 1024, reply->bytesAvailable()));
        if (bytes.isEmpty()) break;
        response_.append(bytes);
        receivedBytes_ += bytes.size();
        if (response_.size() > maximumPageBytes || receivedBytes_ > maximumCheckBytes) {
            fail(QStringLiteral("Update response is too large. · 更新响应超出大小限制。"));
            return;
        }
    }
}

void UpdateManager::finishPage(QNetworkReply *reply)
{
    if (reply_ != reply) return;
    consumeReply(reply);
    if (reply_ != reply) return;
    const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 403 || statusCode == 429) {
        fail(QStringLiteral("GitHub rejected or rate-limited this check (HTTP %1). Try again later. · GitHub 拒绝或限制本次请求（HTTP %1），请稍后重试。").arg(statusCode));
        return;
    }
    if (statusCode >= 300 && statusCode < 400) {
        fail(QStringLiteral("Update endpoint redirected; check the configured repository. · 更新地址已重定向，请检查仓库配置。"));
        return;
    }
    if (reply->error() != QNetworkReply::NoError || statusCode != 200) {
        fail(QStringLiteral("Could not contact GitHub (HTTP %1, network %2). Check your connection and retry. · 无法连接 GitHub（HTTP %1，网络错误 %2），请检查网络后重试。")
            .arg(statusCode).arg(int(reply->error())));
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(response_, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        fail(QStringLiteral("GitHub returned an invalid releases response. · GitHub 返回了无效的发行列表。"));
        return;
    }
    const auto releases = document.array();
    if (releases.size() > 100) {
        fail(QStringLiteral("GitHub returned an unexpected release page. · GitHub 返回的发行分页无效。"));
        return;
    }
    for (const auto &value : releases) {
        if (!value.isObject()) continue;
        const auto object = value.toObject();
        if (!object.value("draft").isBool() || object.value("draft").toBool()
            || !object.value("prerelease").isBool() || object.value("prerelease").toBool()) continue;
        const auto version = parseVersion(object.value("tag_name").toString());
        if (!version) continue;
        const auto expectedUrl = repositoryWebUrl() + "/releases/tag/" + version->tag;
        // Never launch a response-supplied cross-repository URL or redirect.
        if (object.value("html_url").toString() != expectedUrl) continue;
        if (!candidate_ || compareVersions(*version, candidate_->version) > 0)
            candidate_ = Release{*version, object.value("body").toString(), expectedUrl};
    }

    const auto links = QString::fromLatin1(reply->rawHeader("Link"));
    static const QRegularExpression nextLink(QStringLiteral("<([^>]+)>\\s*;\\s*rel=\"next\""));
    const auto next = nextLink.match(links);
    if (next.hasMatch()) {
        if (page_ >= maximumPages || !validPageUrl(QUrl(next.captured(1)), page_ + 1)) {
            fail(QStringLiteral("Unsafe or excessive release pagination was refused. · 已拒绝不安全或过多的发行分页。"));
            return;
        }
        disposeReply();
        requestPage(page_ + 1);
        return;
    }
    disposeReply();
    finishSuccess();
}

void UpdateManager::disposeReply()
{
    auto *reply = reply_.data();
    reply_.clear();
    if (!reply) return;
    disconnect(reply, nullptr, this, nullptr);
    if (!reply->isFinished()) reply->abort();
    reply->deleteLater();
}

void UpdateManager::recordCompletedCheck()
{
    lastChecked_ = now();
    if (settings_) settings_->setValue(lastCheckedKey, lastChecked_);
}

bool UpdateManager::shouldNotify() const
{
    if (!updateAvailable_) return false;
    if (manual_) return true;
    if (!automaticCheck_ || !settings_) return false;
    if (settings_->value(skippedKey).toString() == latestVersion_) return false;
    if (settings_->value(remindVersionKey).toString() == latestVersion_
        && settings_->value(remindUntilKey).toDateTime().toUTC() > now()) return false;
    return true;
}

void UpdateManager::finishSuccess()
{
    deadline_.stop();
    busy_ = false;
    latestVersion_ = candidate_ ? candidate_->version.tag : QString();
    releaseNotes_ = candidate_ ? candidate_->notes : QString();
    releaseUrl_ = candidate_ ? candidate_->url : QString();
    updateAvailable_ = candidate_ && compareVersions(candidate_->version, *installedVersion_) > 0;
    notificationVisible_ = shouldNotify();
    status_ = updateAvailable_ ? QStringLiteral("Update available: %1 · 可更新至 %1").arg(latestVersion_)
        : candidate_ ? QStringLiteral("You are up to date · 当前已是最新版本")
        : QStringLiteral("No stable UAV Studio release is published yet · 尚无已发布的 UAV Studio 稳定版");
    recordCompletedCheck();
    candidate_.reset();
    response_.clear();
    emit stateChanged();
}

void UpdateManager::fail(const QString &reason)
{
    deadline_.stop();
    disposeReply();
    candidate_.reset();
    response_.clear();
    busy_ = false;
    notificationVisible_ = false;
    errorString_ = reason;
    status_ = QStringLiteral("Update check failed · 更新检查失败");
    recordCompletedCheck();
    emit stateChanged();
}

void UpdateManager::skipVersion()
{
    if (updateAvailable_ && settings_) settings_->setValue(skippedKey, latestVersion_);
    notificationVisible_ = false;
    emit stateChanged();
}

void UpdateManager::remindLater()
{
    if (updateAvailable_ && settings_) {
        settings_->setValue(remindVersionKey, latestVersion_);
        settings_->setValue(remindUntilKey, now().addSecs(24 * 60 * 60));
    }
    notificationVisible_ = false;
    emit stateChanged();
}

bool UpdateManager::openReleasePage()
{
    if (!validRepository_) return false;
    const auto url = releaseUrl_.isEmpty() ? repositoryWebUrl() + "/releases" : releaseUrl_;
    return QDesktopServices::openUrl(QUrl(url));
}
