#pragma once

#include "video_timing.h"

#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace replay {

// Keep the full v1 tree, including fields whose editors have not been migrated.
class ProjectDocument {
public:
    static ProjectDocument load(const QString &manifest);
    static ProjectDocument create(const QString &source, const QString &bag = {});

    QString source() const;
    QString bag() const;
    QStringList media() const;
    bool isPortable() const;
    const QString &path() const { return path_; }
    const QJsonObject &json() const { return root_; }
    QJsonObject values() const;
    void setValue(const QString &key, const QString &value);
    std::vector<VideoClip> videos() const;
    void addVideo(const QString &file);
    void setVideoTiming(qsizetype index, const QJsonObject &changes);

    ProjectDocument saveAs(const QString &manifest) const;
    QString exportFolder(const QString &folder, const std::function<bool()> &cancel = {}) const;

private:
    ProjectDocument(QString path, QJsonObject root);
    QJsonObject remapMedia(const std::function<QString(const QString &)> &map) const;
    QString absoluteMedia(const QString &reference) const;

    QString path_;
    QJsonObject root_;
};
}
