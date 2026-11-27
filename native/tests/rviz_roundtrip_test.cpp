#include "project_document.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <yaml-cpp/yaml.h>
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
        expect(layout.displays[0].sourcePath == QList<int>({0, 0}) && layout.displays[0].pluginClass == "rviz/PointCloud2", "Nested property identity lost");
        expect(replay::serializeRviz(layout) == source, "Unedited export must retain every byte");
        QFile bom(directory.filePath("bom.rviz")); expect(bom.open(QIODevice::WriteOnly), "Cannot create BOM fixture");
        const auto bomSource = QByteArray("\xef\xbb\xbf") + source; bom.write(bomSource); bom.close();
        expect(replay::serializeRviz(replay::readRviz(bom.fileName())) == bomSource, "UTF-8 BOM not preserved");
        auto project = replay::ProjectDocument::create("demo"); project.setTopicLayout(layout);
        project = project.saveAs(directory.filePath("project.slamfpv"));
        auto restored = replay::ProjectDocument::load(project.path()).topicLayout();
        expect(restored.sourceYaml == layout.sourceYaml && restored.skipped == layout.skipped && replay::serializeRviz(restored) == source,
            "Project round trip lost raw properties or compatibility diagnostics");
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
        invalid = layout; invalid.displays[0].enabled = true;
        expectRejected(invalid, "Enabled child below disabled group silently exported");
        std::cout << "Byte-identical unedited RViz, project persistence, nested unknown plugins/QoS/views/tools, targeted edits and TF conflict passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
