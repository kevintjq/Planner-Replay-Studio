"""Import display settings from RViz and RViz2 YAML configurations."""
from dataclasses import dataclass

from ruamel.yaml import YAML
from ruamel.yaml.error import YAMLError

from .topic_displays import DISPLAY_COLORS, TopicDisplay

CLASS_TYPES = {
    "PointCloud2": "cloud",
    "LaserScan": "laser",
    "Path": "path",
    "Marker": "marker",
    "MarkerArray": "marker",
    "Pose": "position",
    "PointStamped": "position",
    "Odometry": "position",
    "TF": "tf",
    "Image": "image",
    "Camera": "image",
}


@dataclass(frozen=True)
class RvizConfig:
    fixed_frame: str
    displays: tuple
    skipped: tuple


def read_rviz(path):
    try:
        data = YAML(typ="safe").load(path.read_text(encoding="utf-8-sig"))
    except YAMLError as error:
        raise ValueError("无法解析 RViz YAML 配置。") from error
    return parse_rviz(data)


def parse_rviz(data):
    manager = data.get("Visualization Manager") if isinstance(data, dict) else None
    if not isinstance(manager, dict) or not isinstance(manager.get("Displays"), list):
        raise ValueError("缺少 Visualization Manager / Displays。")
    settings = manager.get("Global Options")
    frame = settings.get("Fixed Frame") if isinstance(settings, dict) else ""
    displays, skipped = [], []
    work = [(item, True, frozenset()) for item in reversed(manager["Displays"])]
    while work:
        entry, active, ancestors = work.pop()
        if not isinstance(entry, dict) or id(entry) in ancestors:
            continue
        plugin = str(entry.get("Class", "")).rsplit("/", 1)[-1]
        title = str(entry.get("Name") or plugin or "未命名显示")
        enabled = active and bool(entry.get("Enabled", entry.get("Value", True)))
        if plugin == "Group" and isinstance(entry.get("Displays"), list):
            nested = ancestors | {id(entry)}
            work.extend((child, enabled, nested) for child in reversed(entry["Displays"]))
            continue

        topic = entry.get("Topic") or entry.get("Marker Topic")
        if isinstance(topic, dict):
            topic = topic.get("Value")
        kind = CLASS_TYPES.get(plugin)
        if kind == "tf":
            topics = ("/tf", "/tf_static")
        elif isinstance(topic, str) and topic.strip():
            topics = (topic.strip(),)
            if kind is None:
                kind = "message"
                skipped.append(f"{title}（导入为消息字段）")
        else:
            skipped.append(title)
            continue
        color = DISPLAY_COLORS[len(displays) % len(DISPLAY_COLORS)]
        try:
            channels = tuple(int(value.strip()) for value in str(entry.get("Color", "")).split(";"))
            if len(channels) == 3 and all(0 <= value <= 255 for value in channels):
                color = "#" + "".join(f"{value:02x}" for value in channels)
        except ValueError:
            pass
        displays.extend(TopicDisplay(topic_name, kind, enabled=enabled, color=color, name=title) for topic_name in topics)
    return RvizConfig(frame.strip().strip("/") if isinstance(frame, str) else "",
                      tuple(displays), tuple(skipped))
