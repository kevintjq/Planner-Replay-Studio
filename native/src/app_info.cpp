#include "app_info.h"
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

AppInfo::AppInfo(QObject *parent) : QObject(parent)
{
    QFile identity(QStringLiteral(":/metadata/app-metadata.json"));
    if (!identity.open(QIODevice::ReadOnly)) qFatal("Missing embedded application metadata");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(identity.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) qFatal("Invalid embedded application metadata");
    identity_ = document.object().toVariantMap();
    for (const auto &key : {"app_name", "project_name", "executable_name", "version", "author", "email", "github_url", "repository_full_name"})
        if (identity_.value(key).toString().isEmpty()) qFatal("Incomplete embedded application metadata");
    QFile license(QStringLiteral(":/metadata/LICENSE"));
    if (!license.open(QIODevice::ReadOnly)) qFatal("Missing embedded application license");
    licenseText_ = QString::fromUtf8(license.readAll());
}
QString AppInfo::appName() const { return identity_.value("app_name").toString(); }
QString AppInfo::projectName() const { return identity_.value("project_name").toString(); }
QString AppInfo::executableName() const { return identity_.value("executable_name").toString(); }
QString AppInfo::fullBranding() const { return appName() + QStringLiteral(" — ") + identity_.value("tagline").toString(); }
QString AppInfo::version() const { return identity_.value("version").toString(); }
QString AppInfo::displayVersion() const {
    const auto channel = identity_.value("release_channel").toString();
    return version() + (channel.isEmpty() || channel == "stable" ? QString() : " · " + channel);
}
QString AppInfo::author() const { return identity_.value("author").toString(); }
QString AppInfo::email() const { return identity_.value("email").toString(); }
QString AppInfo::githubUrl() const { return identity_.value("github_url").toString(); }
QString AppInfo::issuesUrl() const { return githubUrl() + "/issues"; }
QString AppInfo::releasesUrl() const { return githubUrl() + "/releases"; }
QString AppInfo::roadmapUrl() const { return githubUrl() + "#roadmap"; }
QString AppInfo::licenseUrl() const { return githubUrl() + "/blob/main/LICENSE"; }
QVariantMap AppInfo::metadata() const {
    return {{"appName", appName()}, {"version", version()}, {"displayVersion", displayVersion()},
        {"author", author()}, {"email", email()}, {"githubUrl", githubUrl()}, {"issuesUrl", issuesUrl()},
        {"releasesUrl", releasesUrl()}, {"roadmapUrl", roadmapUrl()}, {"licenseUrl", licenseUrl()},
        {"repositoryFullName", identity_.value("repository_full_name")}};
}
bool AppInfo::openLink(const QString &key) const {
    const auto urls = QMap<QString, QString>{{"github", githubUrl()}, {"issues", issuesUrl()},
        {"releases", releasesUrl()}, {"roadmap", roadmapUrl()}, {"license", licenseUrl()}};
    return urls.contains(key) && QDesktopServices::openUrl(QUrl(urls.value(key)));
}
bool AppInfo::copyEmail() const {
    auto *clipboard = QGuiApplication::clipboard();
    if (!clipboard) return false;
    clipboard->setText(email());
    return true;
}
