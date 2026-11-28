#pragma once
#include <QObject>
#include <QVariantMap>

// Public product identity, shared by About, update discovery and packaging.
// Personal settings retain the existing application/organization names.
class AppInfo final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString projectName READ projectName CONSTANT)
    Q_PROPERTY(QString executableName READ executableName CONSTANT)
    Q_PROPERTY(QString fullBranding READ fullBranding CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString displayVersion READ displayVersion CONSTANT)
    Q_PROPERTY(QString author READ author CONSTANT)
    Q_PROPERTY(QString email READ email CONSTANT)
    Q_PROPERTY(QString githubUrl READ githubUrl CONSTANT)
    Q_PROPERTY(QString issuesUrl READ issuesUrl CONSTANT)
    Q_PROPERTY(QString releasesUrl READ releasesUrl CONSTANT)
    Q_PROPERTY(QString roadmapUrl READ roadmapUrl CONSTANT)
    Q_PROPERTY(QString licenseUrl READ licenseUrl CONSTANT)
    Q_PROPERTY(QString licenseText READ licenseText CONSTANT)
public:
    explicit AppInfo(QObject *parent = nullptr);
    QString appName() const;
    QString projectName() const;
    QString executableName() const;
    QString fullBranding() const;
    QString version() const;
    QString displayVersion() const;
    QString author() const;
    QString email() const;
    QString githubUrl() const;
    QString issuesUrl() const;
    QString releasesUrl() const;
    QString roadmapUrl() const;
    QString licenseUrl() const;
    QString licenseText() const { return licenseText_; }
    QVariantMap metadata() const;
    Q_INVOKABLE bool openLink(const QString &key) const;
    Q_INVOKABLE bool copyEmail() const;
private:
    QVariantMap identity_;
    QString licenseText_;
};
