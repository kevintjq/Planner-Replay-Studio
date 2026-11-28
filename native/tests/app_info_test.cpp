#include "app_info.h"
#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QUrl>
#include <QDebug>

class LinkReceiver final : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void receive(const QUrl &url) { urls.push_back(url); }
};

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    AppInfo info;
    const auto check = [](bool passed, const char *message) {
        if (!passed) qCritical() << message;
        return passed;
    };
    bool passed = true;
    passed &= check(info.version() == QStringLiteral(UAV_STUDIO_BUILD_VERSION), "CMake version and embedded metadata diverged");
    passed &= check(info.appName() == "UAV Studio" && info.author() == "kevin" && info.email() == "jq.tong0610@gmail.com", "Public identity is incorrect");
    passed &= check(info.licenseText().startsWith("MIT License") && info.licenseText().contains("Permission is hereby granted"), "Complete actual license is missing");
    passed &= check(info.metadata().value("version").toString() == info.version()
        && info.metadata().value("repositoryFullName").toString() == "kevintjq/uav-studio", "Updater identity diverged");
    LinkReceiver receiver;
    QDesktopServices::setUrlHandler("https", &receiver, "receive");
    for (const auto &key : {"github", "issues", "releases", "roadmap", "license"}) {
        const auto before = receiver.urls.size();
        passed &= check(info.openLink(key) && receiver.urls.size() == before + 1, "Product link was not routed");
    }
    const auto before = receiver.urls.size();
    passed &= check(!info.openLink("https://untrusted.invalid") && receiver.urls.size() == before, "Unknown link key must not launch a URL");
    for (const auto &url : receiver.urls)
        passed &= check(url.scheme() == "https" && url.host() == "github.com" && url.path().startsWith("/kevintjq/uav-studio"), "Product link escaped the configured repository");
    QDesktopServices::unsetUrlHandler("https");
    passed &= check(info.copyEmail() && QGuiApplication::clipboard()->text() == info.email(), "Email clipboard action did not copy the actual address");
    return passed ? 0 : 1;
}
#include "app_info_test.moc"
