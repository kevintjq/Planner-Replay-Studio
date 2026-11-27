from pathlib import Path
from types import SimpleNamespace

import pytest

from planner_replay.composition import Composition
from planner_replay.layer_controls import LayerControls


@pytest.mark.parametrize("has_bag", [False, True])
def test_speed_settings_select_and_apply_topic(has_bag):
    calls = []
    source = SimpleNamespace(get=lambda: "自动识别", set=lambda value: calls.append(value))
    app = SimpleNamespace(
        project=Composition(),
        values={"speed_topic": source},
        bag_path=Path("flight.bag") if has_bag else None,
        bag_metadata={"topics": [
            {"topic": "/odom", "type": "nav_msgs/msg/Odometry"},
            {"topic": "/velocity", "type": "geometry_msgs/msg/TwistStamped"},
            {"topic": "/points", "type": "sensor_msgs/msg/PointCloud2"},
        ]},
        frame=object(),
        refresh_layers=lambda: None,
        root=SimpleNamespace(after_idle=lambda action: action()),
        start_render=lambda preview: calls.append(preview),
    )

    def dialog(title, fields, save, **_options):
        assert title == "飞行速度表"
        topic = next(field for field in fields if field[0] == "speed_topic")
        assert topic[3] == (("自动识别", "/odom", "/velocity") if has_bag else ("自动识别",))
        values = {field[0]: field[2] for field in fields}
        values["speed_topic"] = "/velocity" if has_bag else "自动识别"
        save(values)

    app.layer_dialog = dialog
    LayerControls.edit_speed(app)
    assert app.project.show_speed and app.selection == ("speed", 0)
    assert calls == (["/velocity", True] if has_bag else ["自动识别"])
