#include "project_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
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
        const auto settings = project.json().value("composition").toObject();
        auto saved = project.saveAs(directory.filePath("relocated/edit.slamfpv"));
        expect(replay::ProjectDocument::load(saved.path()).media() == project.media(), "save as must rebase references");
        expect(saved.json().value("metadata") == project.json().value("metadata"), "opaque root settings preserved");
        expect(saved.json().value("composition").toObject().value("layouts") == settings.value("layouts"),
               "opaque timeline preserved");
        expect(saved.json().value("composition").toObject().value("videos").toArray()[0].toObject().value("edit") ==
               settings.value("videos").toArray()[0].toObject().value("edit"), "anchors and crop preserved");

        bool refused = false;
        try { project.saveAs(videoPath); } catch (const std::exception &) { refused = true; }
        expect(refused && read(videoPath) == "fpv material", "asset overwrite blocked");

        const QString packedPath = project.exportFolder(directory.filePath("打包项目"));
        const auto packed = replay::ProjectDocument::load(packedPath);
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
