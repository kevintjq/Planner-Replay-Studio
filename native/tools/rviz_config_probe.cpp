// 可选 ROS Noetic 验收工具；不进入 UAV Studio 应用运行依赖。
// source /opt/ros/noetic/setup.bash 后使用 tools/rviz-probe 的独立 CMake 工程。
#include <ros/ros.h>
#include <rviz/visualization_frame.h>
#include <rviz/visualization_manager.h>
#include <rviz/display_group.h>
#include <rviz/yaml_config_reader.h>
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

QJsonArray displays(rviz::DisplayGroup *group) {
    QJsonArray result;
    for (int i = 0; i < group->numDisplays(); ++i) {
        auto *display = group->getDisplayAt(i);
        QJsonObject entry{{"name", display->getName()}, {"class", display->getClassId()}, {"enabled", display->isEnabled()}};
        if (auto *child = dynamic_cast<rviz::DisplayGroup *>(display)) entry.insert("children", displays(child));
        result.append(entry);
    }
    return result;
}
bool matches(const rviz::Config &expected, const QJsonArray &actual) {
    if (expected.listLength() != actual.size()) return false;
    for (int i = 0; i < expected.listLength(); ++i) {
        const auto config = expected.listChildAt(i); const auto display = actual[i].toObject();
        QString name, plugin; config.mapGetString("Name", &name); config.mapGetString("Class", &plugin);
        if (display["class"].toString() != plugin || (!name.isEmpty() && display["name"].toString() != name)) return false;
        const auto children = config.mapGetChild("Displays");
        if (children.isValid() && !matches(children, display["children"].toArray())) return false;
    }
    return true;
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const QString source = QString::fromLocal8Bit(argv[1]), output = QString::fromLocal8Bit(argv[2]);
    ros::init(argc, argv, "uav_studio_rviz_probe", ros::init_options::AnonymousName);
    QApplication app(argc, argv);
    rviz::Config expected; rviz::YamlConfigReader reader; reader.readFile(expected, source);
    if (reader.error()) return 3;
    rviz::VisualizationFrame frame;
    frame.setSplashPath(""); frame.setShowChooseNewMaster(false); frame.initialize(source);
    frame.resize(1440, 900); frame.show();
    QTimer::singleShot(5000, &app, [&] {
        const auto error = frame.getErrorMessage();
        const auto tree = displays(frame.getManager()->getRootDisplayGroup());
        const bool treeMatches = matches(expected.mapGetChild("Visualization Manager").mapGetChild("Displays"), tree);
        const bool screenshot = frame.grab().save(output + ".png");
        const bool nativeSave = frame.saveDisplayConfig(output + ".rviz");
        const bool passed = error.isEmpty() && !tree.isEmpty() && treeMatches && screenshot && nativeSave;
        const QJsonObject report{{"configuration_loaded", passed}, {"error", error}, {"fixed_frame", frame.getManager()->getFixedFrame()},
            {"displays", tree}, {"display_tree_matches", treeMatches}, {"screenshot", screenshot}, {"native_config_saved", nativeSave}, {"scene_equivalence_verified", false}};
        QFile file(output + ".json");
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0) { app.exit(1); return; }
        app.exit(passed ? 0 : 1);
    });
    return app.exec();
}
