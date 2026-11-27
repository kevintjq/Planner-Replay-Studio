#include "project_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly), "fixture open");
    expect(file.write(data) == data.size(), "fixture write");
}

QByteArray read(const QString &path)
{
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}
}

int main(int argc, char **argv)
{
    try {
        // The optional path lets developers exercise the same round trip on a real workspace.
        QTemporaryDir temporary;
        expect(temporary.isValid(), "temporary directory");
        QDir directory(temporary.path());
        if (argc == 2) {
            const auto original = replay::ProjectDocument::load(QString::fromLocal8Bit(argv[1]));
            const auto copy = replay::ProjectDocument::load(original.exportFolder(directory.filePath("package")));
            expect(copy.media().size() == original.media().size(), "workspace media count mismatch");
            expect(copy.values() == original.values(), "workspace settings mismatch");
            const auto oldClips = original.videos();
            const auto packedClips = copy.videos();
            expect(oldClips.size() == packedClips.size(), "workspace videos lost");
            for (size_t i = 0; i < oldClips.size(); ++i) {
                expect(packedClips[i].timing.anchors == oldClips[i].timing.anchors, "workspace anchors lost");
                expect(packedClips[i].timing.segments == oldClips[i].timing.segments, "workspace segments lost");
            }
            for (qsizetype i = 0; i < copy.media().size(); ++i)
                expect(read(copy.media()[i]) == read(original.media()[i]), "workspace asset bytes mismatch");
            return 0;
        }

        expect(directory.mkdir("素材") && directory.mkdir("other") && directory.mkdir("relocated"), "fixture folders");
        const auto manifest = directory.filePath("旧项目.slamfpv");
        const auto videoPath = directory.filePath("素材/fpv.mp4");
        write(videoPath, "fpv material");
        write(directory.filePath("other/fpv.mp4"), "second camera");
        write(directory.filePath("素材/run.bag"), "recording");
        write(directory.filePath("素材/marker.png"), "picture");
        write(directory.filePath("素材/extrinsics.json"), "{}");
        write(manifest, R"json({
          "version": 1,
          "source": "bag",
          "bag": "素材/run.bag",
          "extrinsics": "素材/extrinsics.json",
          "values": {"cloud_topic": "/cloud_registered", "world": "map"},
          "enabled": {"scan": true},
          "metadata": {"producer": "existing project"},
          "composition": {
            "main_view": {"id": "main", "kind": "rear", "width": 1.0},
            "videos": [
              {"path": "素材/fpv.mp4", "id": "one", "edit": {"anchors": [[0, 2], [3, 5.2]], "crop": [0, 0, 0.9, 1]}},
              {"path": "other/fpv.mp4", "id": "two"},
              {"path": "素材/./fpv.mp4", "id": "again"}
            ],
            "images": [{"path": "素材/marker.png", "id": "logo"}],
            "views": [],
            "texts": [],
            "layouts": [{"time": 3, "mode": "inset", "video_id": "one"}],
            "topic_displays": [{"id": "odometry", "topic": "/odom", "kind": "position"}]
          }
        })json");

        auto project = replay::ProjectDocument::load(manifest);
        expect(project.media().size() == 5, "deduplicated media paths");
        expect(project.bag().endsWith("素材/run.bag"), "bag reference");
        const auto rviz = directory.filePath("import.rviz");
        write(rviz, R"yaml(
Visualization Manager:
  Global Options:
    Fixed Frame: /map
  Displays:
    - Class: rviz/PointCloud2
      Topic: /cloud
      Color: 10; 20; 30
    - Class: rviz_default_plugins/Group
      Enabled: false
      Displays:
        - Class: rviz_default_plugins/Path
          Topic: {Value: /path}
        - Class: rviz/MarkerArray
          Marker Topic: /markers
    - Class: rviz_default_plugins/Image
      Topic: {Value: /camera/image}
    - Class: example/Custom
      Name: Custom data
      Topic: /custom
    - Class: rviz/Grid
      Name: Grid
    - Class: rviz/TF
)yaml");
        const auto layout = replay::readRviz(rviz);
        expect(layout.fixedFrame == "map" && layout.displays.size() == 7, "RViz display count and fixed frame");
        expect(layout.displays[0].topic == "/cloud" && layout.displays[0].color == QColor("#0a141e"),
               "RViz topic and configured color");
        expect(!layout.displays[1].enabled && layout.displays[1].kind == "path" && layout.displays[1].topic == "/path" &&
               !layout.displays[2].enabled && layout.displays[2].topic == "/markers", "Nested and disabled RViz displays");
        expect(layout.displays[3].kind == "image" && layout.displays[4].kind == "message" &&
               layout.displays[5].topic == "/tf" && layout.displays[6].topic == "/tf_static" &&
               layout.skipped.contains("Grid"), "RViz image, custom, TF and unsupported displays");
        auto configured = project;
        configured.setTopicLayout(layout);
        const auto imported = configured.saveAs(directory.filePath("relocated/rviz.slamfpv"));
        const auto importedLayout = replay::ProjectDocument::load(imported.path()).topicLayout();
        expect(importedLayout.fixedFrame == "map" && importedLayout.displays[3].id == layout.displays[3].id &&
               importedLayout.displays[4].name == "Custom data", "RViz settings survive project round trip");

        const auto settings = project.json().value("composition").toObject();
        project.setSceneBackground("#E5EAEE");
        project.setSpeedGauge({{"scheme", "light"}});
        project.setTrajectorySettings({{"path", videoPath}, {"interval", .1},
            {"edits", QJsonObject{{"ranges", QJsonArray{QJsonObject{{"id", "stable"}, {"begin_ns", "100"}, {"end_ns", "200"}}}}}}});
        auto saved = project.saveAs(directory.filePath("relocated/edit.slamfpv"));
        expect(saved.sceneBackground() == "#e5eaee" && saved.speedGauge().value("scheme") == "light", "independent content style persisted");
        expect(saved.trajectorySettings().value("path").toString() == videoPath &&
            saved.trajectorySettings().value("edits") == project.trajectorySettings().value("edits"), "trajectory video and stable edit state rebased");
        expect(replay::ProjectDocument::load(saved.path()).media() == project.media(), "save as must rebase references");
        expect(saved.json().value("metadata") == project.json().value("metadata"), "opaque root settings preserved");
        expect(saved.json().value("composition").toObject().value("layouts") == settings.value("layouts"),
               "opaque timeline preserved");
        expect(saved.json().value("composition").toObject().value("videos").toArray()[0].toObject().value("edit") ==
               settings.value("videos").toArray()[0].toObject().value("edit"), "anchors and crop preserved");

        const auto clips = project.videos();
        expect(clips.size() == 3 && clips[0].path == videoPath, "old media opened");
        expect(std::abs(clips[0].timing.sourceSeconds(1.5) - 3.6) < 1e-9, "two-anchor drift");
        expect(clips[0].timing.crop[2] == .9 && clips[1].timing.includes(0), "old video defaults and crop");
        auto edited = project;
        edited.setVideoTiming(0, {{"trim_in", 2.5}, {"trim_out", 5.}, {"anchors", QJsonArray{QJsonArray{3, 5.2}, QJsonArray{0, 2}}},
                                  {"segments", QJsonArray{QJsonArray{2.5, 3.}, QJsonArray{4., 5.5}}}});
        edited.setVideoTiming(1, {{"segments", QJsonArray{}}});
        edited.addVideo(videoPath);
        auto reloaded = replay::ProjectDocument::load(edited.saveAs(directory.filePath("relocated/edited.slamfpv")).path());
        const auto updated = reloaded.videos();
        expect(updated.size() == 4 && updated[3].path == videoPath && !updated[3].id.isEmpty(), "new video persisted");
        expect(std::abs(updated[0].timing.sourceSeconds(1.5) - 3.6) < 1e-9, "reversed anchor entry order");
        expect(updated[0].timing.includes(2.5) && !updated[0].timing.includes(3.) &&
               updated[0].timing.includes(4.5) && !updated[0].timing.includes(5.), "trim and deleted segments");
        expect(updated[0].timing.crop[2] == .9, "editing timing preserves crop");
        expect(!updated[1].timing.includes(2.), "explicitly empty video remains hidden");
        const auto initialEdit = edited.json();
        bool invalidEdit = false;
        try { edited.setVideoTiming(0, {{"anchors", QJsonArray{QJsonArray{0, 2}, QJsonArray{3, 1}}}}); }
        catch (const std::exception &) { invalidEdit = true; }
        expect(invalidEdit && edited.json() == initialEdit, "invalid timing leaves project unchanged");

        bool refused = false;
        try { project.saveAs(videoPath); } catch (const std::exception &) { refused = true; }
        expect(refused && read(videoPath) == "fpv material", "asset overwrite blocked");

        const QString packedPath = project.exportFolder(directory.filePath("打包项目"));
        const auto packed = replay::ProjectDocument::load(packedPath);
        expect(read(packed.trajectorySettings().value("path").toString()) == "fpv material", "portable trajectory video copied");
        expect(packed.isPortable() && packed.media().size() == 5, "portable project metadata");
        expect(QFileInfo(packedPath).dir().exists("cache") && QFileInfo(packedPath).dir().exists("exports"), "portable folders");
        const auto packedVideos = packed.json().value("composition").toObject().value("videos").toArray();
        expect(packedVideos[0].toObject().value("path") == packedVideos[2].toObject().value("path"), "reuse copied media");
        expect(packedVideos[0].toObject().value("path") != packedVideos[1].toObject().value("path"), "distinct same-name media");
        expect(directory.rename("打包项目", "移动后") && directory.rename("素材", "原素材") && directory.rename("other", "archived"),
               "relocate portable folder and originals");
        auto moved = replay::ProjectDocument::load(directory.filePath("移动后/project.slamfpv"));
        expect(read(moved.bag()) == "recording" && read(moved.media()[3]) == "second camera", "relocated assets");
        refused = false;
        try { moved.exportFolder(directory.filePath("移动后")); } catch (const std::exception &) { refused = true; }
        expect(refused, "refuse overwrite of portable folder");
        refused = false;
        try { moved.exportFolder(directory.filePath("cancelled"), [] { return true; }); }
        catch (const std::exception &) { refused = true; }
        expect(refused && !directory.exists("cancelled") && directory.entryList({".planner-replay-*"}, QDir::Dirs | QDir::Hidden).isEmpty(),
               "cancelled folder is not published and staging is cleaned");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
