from planner_replay.composition import Composition
from planner_replay.project_file import ProjectFile, load_project, save_project
from planner_replay.rviz_config import read_rviz
from planner_replay.topic_displays import supported_displays


def test_rviz_import_preserves_groups_topics_and_display_options(tmp_path):
    path = tmp_path / "layout.rviz"
    path.write_text("""
Visualization Manager:
  Global Options:
    Fixed Frame: /map
  Displays:
    - Class: rviz/PointCloud2
      Topic: /cloud
      Color: 10; 20; 30
      Enabled: true
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
""", encoding="utf-8")
    layout = read_rviz(path)
    assert layout.fixed_frame == "map"
    assert [(d.topic, d.kind, d.enabled) for d in layout.displays] == [
        ("/cloud", "cloud", True), ("/path", "path", False), ("/markers", "marker", False),
        ("/camera/image", "image", True), ("/custom", "message", True),
    ]
    assert layout.displays[0].color == "#0a141e"
    assert layout.skipped == ("Custom data（导入为消息字段）", "Grid")
    assert supported_displays("example/msg/Custom") == ("message",)
    project = ProjectFile(Composition(topic_displays=layout.displays, show_main_cloud=False,
                                      display_fixed_frame=layout.fixed_frame), {}, {}, "none")
    manifest = tmp_path / "project.slamfpv"
    save_project(manifest, project)
    assert load_project(manifest) == project


def test_camera_image_and_large_message_keep_pixel_and_field_access():
    from types import SimpleNamespace

    import numpy as np

    from planner_replay.topic_inspection import TopicMessage, image_preview, message_children

    image = SimpleNamespace(width=1, height=2, step=4, encoding="bgr8",
                            data=np.array([7, 8, 9, 0, 40, 50, 60, 0], dtype=np.uint8))
    preview = image_preview(TopicMessage("sensor_msgs/msg/Image", 0., image))
    assert np.asarray(preview).reshape(-1, 3).tolist() == [[9, 8, 7], [60, 50, 40]]
    data = np.arange(20000)
    last_page = list(message_children(data))[-1][1]
    while not isinstance(last_page, np.integer):
        last_page = list(message_children(last_page))[-1][1]
    assert last_page == 19999
