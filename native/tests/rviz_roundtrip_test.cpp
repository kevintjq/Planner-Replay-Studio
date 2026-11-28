#include "project_document.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        const QByteArray source = R"yaml(# Keep comments and all unsupported plugin fields on an unchanged export.
Panels:
  - Class: rviz/Displays
    Name: Displays
Visualization Manager:
  Global Options: {Fixed Frame: /map, Background Color: '48; 48; 48', Frame Rate: 30}
  Displays:
    - Class: rviz/Group
      Name: Sensors
      Enabled: false
      Displays:
        - Class: rviz/PointCloud2
          Name: Cloud
          Enabled: true
          Topic: {Value: /cloud, Reliability Policy: Reliable, Durability Policy: Volatile}
          Color: 10; 20; 30
          Alpha: 0.6
          Size (m): 0.025
          Color Transformer: RGB8
          Custom Properties: {Calibration: [1, 2, 3], Nested: {gain: 2.5}}
        - Class: custom/UnknownPlugin
          Name: Unknown with no topic
          Payload: {a: 3, b: [true, false, 7]}
    - Class: rviz/Grid
      Name: Ground
      Cell Size: 2.0
      Plane: XY
    - Class: rviz/TF
      Name: Transforms
      Enabled: true
      Value: true
      Color: 80; 90; 100
      Frames: {All Enabled: false, body: {Value: true}}
  Tools:
    - {Class: rviz/Measure, Line color: '128; 128; 0'}
  Views:
    Current: {Class: rviz/Orbit, Distance: 15, Yaw: 1.25, Target Frame: body}
Window Geometry: {Width: 1440, Height: 900, QMainWindow State: preserved-state}
)yaml";
        const auto path = directory.filePath("source.rviz"); QFile file(path);
        expect(file.open(QIODevice::WriteOnly) && file.write(source) == source.size(), "Cannot create RViz fixture"); file.close();
        const auto layout = replay::readRviz(path);
        expect(layout.sourceYaml.toUtf8() == source && layout.displays.size() == 3, "Raw YAML or display mapping lost");
        expect(layout.backgroundColor == QColor(48, 48, 48) && replay::rvizBackgroundColor(layout.sourceYaml) == layout.backgroundColor,
            "Global Options background was not decoded");
        expect(layout.displays[0].sourcePath == QList<int>({0, 0}) && layout.displays[0].pluginClass == "rviz/PointCloud2", "Nested property identity lost");
        expect(replay::serializeRviz(layout) == source, "Unedited export must retain every byte");
        QFile bom(directory.filePath("bom.rviz")); expect(bom.open(QIODevice::WriteOnly), "Cannot create BOM fixture");
        const auto bomSource = QByteArray("\xef\xbb\xbf") + source; bom.write(bomSource); bom.close();
        expect(replay::serializeRviz(replay::readRviz(bom.fileName())) == bomSource, "UTF-8 BOM not preserved");
        auto project = replay::ProjectDocument::create("demo"); project.setTopicLayout(layout);
        project = project.saveAs(directory.filePath("project.slamfpv"));
        auto restored = replay::ProjectDocument::load(project.path()).topicLayout();
        expect(restored.sourceYaml == layout.sourceYaml && restored.skipped == layout.skipped && restored.backgroundColor == layout.backgroundColor &&
            project.sceneBackground() == "#303030" && replay::serializeRviz(restored) == source,
            "Project round trip lost raw properties or compatibility diagnostics");
        auto legacyRoot = project.json(); auto legacyComposition = legacyRoot.value("composition").toObject();
        legacyComposition.remove("display_background_color"); legacyComposition.remove("scene_background");
        legacyRoot["composition"] = legacyComposition;
        QFile legacyProject(directory.filePath("legacy-background.uavstudio")); expect(legacyProject.open(QIODevice::WriteOnly), "Cannot write old background project");
        legacyProject.write(QJsonDocument(legacyRoot).toJson()); legacyProject.close();
        const auto migrated = replay::ProjectDocument::load(legacyProject.fileName());
        expect(migrated.sceneBackground() == "#303030" && migrated.topicLayout().backgroundColor == layout.backgroundColor &&
            replay::serializeRviz(migrated.topicLayout()) == source, "Old project did not restore its imported Global background without altering source YAML");
        auto recolored = layout; recolored.backgroundColor = QColor("#e5eaee");
        expect(YAML::Load(replay::serializeRviz(recolored).toStdString())["Visualization Manager"]["Global Options"]["Background Color"].as<std::string>() == "229; 234; 238",
            "Legacy Global Options background edit was not exported");
        restored.fixedFrame = "odom"; restored.displays[0].name = "Edited Cloud"; restored.displays[0].topic = "/new_cloud"; restored.displays[0].color = QColor("#abcdef");
        const auto result = replay::serializeRviz(restored); const auto yaml = YAML::Load(result.toStdString());
        const auto manager = yaml["Visualization Manager"], cloud = manager["Displays"][0]["Displays"][0];
        expect(manager["Global Options"]["Fixed Frame"].as<std::string>() == "odom" && cloud["Topic"]["Value"].as<std::string>() == "/new_cloud", "Known fields not updated");
        expect(cloud["Topic"]["Reliability Policy"].as<std::string>() == "Reliable" && cloud["Alpha"].as<double>() == .6 && cloud["Size (m)"].as<double>() == .025,
            "Topic QoS or plugin rendering properties lost");
        expect(cloud["Color"].as<std::string>() == "171; 205; 239" && cloud["Name"].as<std::string>() == "Edited Cloud", "Style update wrong");
        const auto original = YAML::Load(source.toStdString());
        expect(YAML::Dump(yaml["Panels"]) == YAML::Dump(original["Panels"]) && YAML::Dump(yaml["Window Geometry"]) == YAML::Dump(original["Window Geometry"]), "Window or panel properties changed");
        expect(YAML::Dump(manager["Tools"]) == YAML::Dump(original["Visualization Manager"]["Tools"]) && YAML::Dump(manager["Views"]) == YAML::Dump(original["Visualization Manager"]["Views"]), "Camera or tools lost");
        expect(YAML::Dump(manager["Displays"][0]["Displays"][1]) == YAML::Dump(original["Visualization Manager"]["Displays"][0]["Displays"][1]) &&
            YAML::Dump(manager["Displays"][1]) == YAML::Dump(original["Visualization Manager"]["Displays"][1]), "Unknown plugin or grid lost");
        restored.displays[1].enabled = false;
        bool rejected = false; try { replay::serializeRviz(restored); } catch (const std::exception &) { rejected = true; }
        expect(rejected, "Conflicting split TF settings silently exported");
        const auto expectRejected = [](const replay::RvizLayout &value, const char *message) {
            bool rejected = false;
            try { replay::serializeRviz(value); } catch (const std::exception &) { rejected = true; }
            expect(rejected, message);
        };
        auto invalid = layout; invalid.displays.pop_back();
        expectRejected(invalid, "Deleted TF half silently retained");
        invalid = layout; invalid.displays[1].topic = "/other_tf";
        expectRejected(invalid, "Unrepresentable TF topic edit silently retained");
        invalid = layout; invalid.displays[0].pluginClass = "rviz/Image";
        expectRejected(invalid, "Plugin class edit silently ignored");
        invalid = layout; invalid.displays.push_back(invalid.displays[0]);
        expectRejected(invalid, "Duplicate source display silently collapsed");
        invalid.displays.back().topic = "/another_topic";
        expectRejected(invalid, "Duplicate source with different topics silently overwrote a display");
        invalid = layout; invalid.displays[0].enabled = true;
        expectRejected(invalid, "Enabled child below disabled group silently exported");
        auto disabledTf = layout;
        disabledTf.displays[1].enabled = false; disabledTf.displays[2].enabled = false;
        const auto disabledYaml = YAML::Load(replay::serializeRviz(disabledTf).toStdString());
        const auto tf = disabledYaml["Visualization Manager"]["Displays"][2];
        expect(!tf["Enabled"].as<bool>() && !tf["Value"].as<bool>(), "RViz boolean Value retained a stale checkbox state");
        auto instances = replay::rvizDisplayInstances(layout);
        expect(instances.size() == 5 && instances[0].kind == "group" && instances[1].kind == "cloud" &&
            instances[2].kind == "unsupported" && instances[3].kind == "grid" && instances[4].kind == "tf",
            "Display model omitted a Group, static display, unknown plugin, or duplicated TF");
        expect(instances[1].parentId == instances[0].id && instances[2].parentId == instances[0].id &&
            !instances[0].enabled && instances[1].enabled, "Parent hierarchy lost local child checkbox state");
        auto instanceLayout = replay::withRvizDisplayInstances(layout, instances);
        expect(instanceLayout.useInstanceModel && instanceLayout.displays.size() == 3 && !instanceLayout.displays[0].enabled,
            "Effective subscriptions do not reflect a disabled Group");
        expect(replay::serializeRviz(instanceLayout) == source, "Unedited instance export must preserve all source bytes");
        auto recoloredInstances = instanceLayout; recoloredInstances.backgroundColor = QColor("#1c2634");
        expect(YAML::Load(replay::serializeRviz(recoloredInstances).toStdString())["Visualization Manager"]["Global Options"]["Background Color"].as<std::string>() == "28; 38; 52",
            "Instance Global Options background edit was not exported");
        const auto malformedSource = QByteArray("Visualization Manager:\n  Global Options: {Background Color: '999; 10; 20'}\n  Displays: []\n");
        const auto malformedPath = directory.filePath("invalid-color.rviz"); QFile malformed(malformedPath);
        expect(malformed.open(QIODevice::WriteOnly) && malformed.write(malformedSource) == malformedSource.size(), "Cannot write invalid color fixture"); malformed.close();
        const auto malformedLayout = replay::readRviz(malformedPath);
        expect(!malformedLayout.backgroundColor.isValid(), "Invalid background was replaced with an invented value");
        const auto malformedBytes = replay::serializeRviz(replay::withRvizDisplayInstances(malformedLayout, {}));
        if (malformedBytes != malformedSource) std::cerr << "Unedited invalid-color fixture changed:\n" << malformedBytes.toStdString();
        expect(malformedBytes == malformedSource, "Unedited empty Display collection or invalid raw background changed on export");
        const auto rawProperties = replay::rvizDisplayPropertyTree(instances[1]);
        bool preservedGain = false;
        for (const auto &property : rawProperties) {
            const auto row = property.toMap();
            expect(row.value("readOnly").toBool(), "Preserved configuration pretends to be an editable native Property");
            preservedGain = preservedGain || (row.value("name").toString() == "Custom Properties / Nested / gain" && row.value("value").toString() == "2.5");
        }
        expect(preservedGain && replay::independentDisplayKind("custom/PointCloud2") == "unsupported",
            "Nested raw configuration or explicit standalone Class boundary lost");
        auto copiedCloud = instances[1]; copiedCloud.id = "independent-copy"; copiedCloud.parentId.clear();
        copiedCloud.name = "Cloud copy"; copiedCloud.topic = "/other_cloud"; copiedCloud.alpha = .35;
        copiedCloud.pointSize = 7.; copiedCloud.lineWidth = 4.5; copiedCloud.historySeconds = 1.25;
        copiedCloud.useMessageColor = false;
        instances.erase(instances.begin() + 3); // A real removal must remove the original Grid node.
        instances[0].enabled = true; instances.push_back(copiedCloud);
        instanceLayout = replay::withRvizDisplayInstances(layout, instances);
        const auto instanceBytes = replay::serializeRviz(instanceLayout);
        const auto instanceYaml = YAML::Load(instanceBytes.toStdString());
        const auto instanceManager = instanceYaml["Visualization Manager"];
        expect(instanceManager["Displays"].size() == 3 && instanceManager["Displays"][1]["Class"].as<std::string>() == "rviz/TF" &&
            instanceManager["Displays"][2]["Name"].as<std::string>() == "Cloud copy", "Deletion, copy order, or single TF identity lost");
        const auto clone = instanceManager["Displays"][2];
        expect(clone["Topic"]["Value"].as<std::string>() == "/other_cloud" &&
            clone["Topic"]["Reliability Policy"].as<std::string>() == "Reliable" &&
            YAML::Dump(clone["Custom Properties"]) == YAML::Dump(original["Visualization Manager"]["Displays"][0]["Displays"][0]["Custom Properties"]),
            "Copied Display lost unknown nested fields or Topic QoS");
        expect(clone["Alpha"].as<double>() == .35 && clone["Size (Pixels)"].as<double>() == 7. &&
            clone["Size (m)"].as<double>() == .025 && clone["UAV Studio"]["Line Width (Pixels)"].as<double>() == 4.5 &&
            !clone["UAV Studio"]["Use Message Color"].as<bool>(), "Independent style or native units were overwritten");
        expect(YAML::Dump(instanceManager["Displays"][0]["Displays"][1]) == YAML::Dump(original["Visualization Manager"]["Displays"][0]["Displays"][1]) &&
            YAML::Dump(instanceManager["Tools"]) == YAML::Dump(original["Visualization Manager"]["Tools"]) &&
            YAML::Dump(instanceManager["Views"]) == YAML::Dump(original["Visualization Manager"]["Views"]) &&
            YAML::Dump(instanceYaml["Panels"]) == YAML::Dump(original["Panels"]), "Instance edits changed unsupported plugins, Tools, Views, or Panels");
        auto instanceProject = replay::ProjectDocument::create("demo"); instanceProject.setTopicLayout(instanceLayout);
        instanceProject = instanceProject.saveAs(directory.filePath("instances.uavstudio"));
        const auto instanceRestored = replay::ProjectDocument::load(instanceProject.path()).topicLayout();
        expect(instanceRestored.useInstanceModel && instanceRestored.backgroundColor == layout.backgroundColor && instanceRestored.instances.size() == instances.size() &&
            instanceRestored.instances.back().id == "independent-copy" && instanceRestored.instances.back().lineWidth == 4.5 &&
            replay::serializeRviz(instanceRestored) == instanceBytes, "Project round trip lost independent instance IDs, styles, or preserved YAML");
        auto invalidInstances = instanceLayout; invalidInstances.instances.back().id = invalidInstances.instances.front().id;
        expectRejected(invalidInstances, "Duplicate independent identities silently exported");
        invalidInstances = instanceLayout; invalidInstances.instances.front().parentId = invalidInstances.instances.front().id;
        expectRejected(invalidInstances, "Cyclic Group hierarchy silently exported");
        const auto empty = replay::withRvizDisplayInstances(layout, {});
        expect(YAML::Load(replay::serializeRviz(empty).toStdString())["Visualization Manager"]["Displays"].size() == 0,
            "Delete-all silently restored source Displays");
        std::cout << "Byte-identical RViz, legacy TF safety, full instance CRUD, Groups, unknown properties/QoS/views/tools and project persistence passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
