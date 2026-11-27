#pragma once

#include "rviz_layout.h"
#include "video_timing.h"

#include <QJsonObject>
#include <QStringList>
#include <QVariantList>
#include <functional>

namespace replay {

// Keep the full v1 tree, including fields whose editors have not been migrated.
class ProjectDocument {
public:
    static ProjectDocument load(const QString &manifest);
    static ProjectDocument create(const QString &source, const QString &bag = {}, const QString &extrinsics = {});

    QString source() const;
    QString bag() const;
    QStringList media() const;
    bool isPortable() const;
    const QString &path() const { return path_; }
    QString extrinsics() const;
    const QJsonObject &json() const { return root_; }
    QJsonObject values() const;
    void setValue(const QString &key, const QString &value);
    bool isLayerEnabled(const QString &layer) const;
    void setLayerEnabled(const QString &layer, bool isEnabled);
    QVariantMap speedGauge() const;
    void setSpeedGauge(const QVariantMap &values);
    QVariantMap moviePreferences() const;
    void setMoviePreferences(double rate, const QString &audioId);
    bool comparePlans() const;
    void setComparePlans(bool compare);
    std::vector<VideoClip> videos() const;
    QString cameraKind() const;
    void setCameraKind(const QString &kind);
    bool transparentCanvas() const;
    void setTransparentCanvas(bool transparent);
    QJsonArray layoutCues() const;
    void setLayoutCue(double time, const QString &mode, const QString &videoId, double transition);
    void removeLayoutCue(double time);
    QJsonArray timelineMarkers() const;
    void addTimelineMarker(double time);
    void replaceTimelineMarker(double time, const std::optional<QString> &label);
    QVariantMap mainView() const;
    void setMainViewWidth(double width);
    QJsonArray layerMotions() const;
    void addLayerMotion(const QString &kind, const QString &id, double begin, double time, double duration,
                        double width, double x, double y);
    void removeLayerMotion(qsizetype index);
    QVariantList cloudViews() const;
    void addCloudView(const QString &kind);
    void setCloudView(const QString &id, const QVariantMap &settings);
    void removeCloudView(const QString &id);
    QVariantList sceneLayers() const;
    void addSceneLayer(const QString &kind, const QString &content, double start, double end);
    void editSceneLayer(const QString &id, const QJsonObject &changes);
    void removeSceneLayer(const QString &id);
    RvizLayout topicLayout() const;
    void setTopicLayout(const RvizLayout &layout);
    void addVideo(const QString &file);
    QJsonObject videoTiming(qsizetype index) const;
    void setVideoTiming(qsizetype index, const QJsonObject &changes);
    void replaceVideoTiming(qsizetype index, const QJsonObject &edit);

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
