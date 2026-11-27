"""Project settings for independently rendered ROS topic displays."""
from dataclasses import dataclass, field
from re import fullmatch
from uuid import uuid4

from .sensor_topics import ODOM, SCAN_TYPES
from .spatial import EGO_SPLINES, MARKER, MARKERS, PATH, POINT_STAMPED, POSE_STAMPED, TF_MESSAGE

DISPLAY_TYPES = {
    "cloud": ("点云", tuple(kind for kind in SCAN_TYPES if kind != "sensor_msgs/msg/LaserScan")),
    "laser": ("激光扫描", ("sensor_msgs/msg/LaserScan",)),
    "path": ("路径", (PATH, *EGO_SPLINES)),
    "marker": ("标记", (MARKER, MARKERS)),
    "position": ("位置", (POSE_STAMPED, POINT_STAMPED, ODOM)),
    "tf": ("TF 坐标轴", (TF_MESSAGE,)),
    "image": ("图像", ("sensor_msgs/msg/Image", "sensor_msgs/msg/CompressedImage")),
    "message": ("消息字段", ()),
}
INSPECTOR_TYPES = frozenset({"message", "image"})
DISPLAY_COLORS = ("#4DDDF3", "#FFCA63", "#7AEC9D", "#B6A0FF", "#FB969C")


def supported_displays(message_type):
    return tuple(kind for kind, (_, types) in DISPLAY_TYPES.items() if message_type in types or kind == "message")


@dataclass(frozen=True)
class TopicDisplay:
    topic: str
    kind: str
    enabled: bool = True
    main_only: bool = False
    color: str = DISPLAY_COLORS[0]
    id: str = field(default_factory=lambda: uuid4().hex)
    name: str = ""

    def __post_init__(self):
        if (not isinstance(self.topic, str) or not self.topic.strip()
                or not isinstance(self.kind, str) or self.kind not in DISPLAY_TYPES or type(self.enabled) is not bool or type(self.main_only) is not bool
                or not isinstance(self.color, str) or not fullmatch(r"#[0-9a-fA-F]{6}", self.color)
                or not isinstance(self.id, str) or not self.id or not isinstance(self.name, str)):
            raise ValueError("话题显示配置无效。")

    @property
    def rgb(self):
        return tuple(int(self.color[i:i+2], 16) for i in (1, 3, 5))
