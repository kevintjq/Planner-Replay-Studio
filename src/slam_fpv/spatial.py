"""Optional recorded mission geometry and sensor overlays for offline rendering."""
from bisect import bisect_right
from dataclasses import dataclass
from math import isfinite

import numpy as np

from .geometry import matrix, transform_points

PATH = "nav_msgs/msg/Path"
MARKER = "visualization_msgs/msg/Marker"
MARKERS = "visualization_msgs/msg/MarkerArray"
CLOUD = "sensor_msgs/msg/PointCloud2"
PLAN_TYPES = (PATH, MARKER, MARKERS)
CORRIDOR_TYPES = (MARKER, MARKERS)


@dataclass(frozen=True)
class SensorView:
    frame: str
    angles: tuple
    distance: float
    camera: bool
    elevation: float = 0.

    def __post_init__(self):
        if not self.frame.strip("/") or not all(isfinite(v) for v in (*self.angles, self.distance, self.elevation)):
            raise ValueError("传感器坐标系、视场角、距离必须有效。")
        horizontal, vertical = self.angles
        if not (0 < horizontal <= 360 and 0 < vertical < 180 and self.distance > 0):
            raise ValueError("水平视场须在 (0, 360]°，垂直视场须在 (0, 180)°，距离须大于 0。")
        if self.camera and (horizontal >= 180 or vertical >= 180):
            raise ValueError("相机的两个视场角均须小于 180°。")
        if not self.camera and abs(self.elevation) + vertical / 2 > 90:
            raise ValueError("雷达中心仰角 ± 半个垂直视场须在 [-90, 90]° 内。")


def sensor_edges(sensor):
    """Return local line segments in optical (+Z) or LiDAR (+X) convention."""
    h, v = np.deg2rad(np.array(sensor.angles) / 2)
    if sensor.camera:
        x, y = np.tan([h, v]) * sensor.distance
        ring = np.array([[-x,-y], [x,-y], [x,y], [-x,y]])
        ring = np.column_stack((ring, np.full(4, sensor.distance)))
        return np.concatenate((np.stack((ring, np.roll(ring, 1, axis=0)), axis=1),
                               np.stack((np.zeros_like(ring), ring), axis=1)))
    azimuth = np.linspace(-h, h, max(3, int(np.ceil(sensor.angles[0] / 10)) + 1))
    low, high = np.deg2rad(sensor.elevation) + np.array([-v, v])
    rims = np.array([np.column_stack((np.cos(azimuth)*np.cos(e),
                                    np.sin(azimuth)*np.cos(e),
                                    np.full_like(azimuth, np.sin(e))))
                     for e in (low, high)]) * sensor.distance
    segments = [np.stack((row[:-1], row[1:]), axis=1) for row in rims]
    full = sensor.angles[0] == 360
    ribs = np.linspace(0, len(azimuth)-1, 9 if full else 5, dtype=int)
    segments.append(rims[:, ribs[:-1] if full else ribs].transpose(1, 0, 2))
    if not full:
        boundary = rims[:, (0,-1)].reshape(-1, 3)
        segments.append(np.stack((np.zeros_like(boundary), boundary), axis=1))
    return np.concatenate(segments)


@dataclass(frozen=True)
class Geometry:
    lines: tuple = ()
    segments: tuple = ()
    triangles: tuple = ()

    def transformed(self, transform):
        def move(groups):
            return tuple(transform_points(transform, item) for item in groups)
        return Geometry(move(self.lines), move(self.segments), move(self.triangles))


def coordinates(points):
    return np.array([(p.x, p.y, p.z) for p in points], dtype=float).reshape(-1, 3)


CUBE_TRIANGLES = np.array([[0,2,1], [0,3,2], [4,5,6], [4,6,7], [0,1,5], [0,5,4],
                           [1,2,6], [1,6,5], [2,3,7], [2,7,6], [3,0,4], [3,4,7]])
CUBE_EDGES = np.array([[0,1], [1,2], [2,3], [3,0], [4,5], [5,6], [6,7], [7,4],
                       [0,4], [1,5], [2,6], [3,7]])
CUBE_VERTICES = np.array([[-1,-1,-1], [1,-1,-1], [1,1,-1], [-1,1,-1],
                          [-1,-1,1], [1,-1,1], [1,1,1], [-1,1,1]]) / 2


def marker_shape(marker):
    points = coordinates(marker.points)
    kind = marker.type
    if kind == 4 and len(points) >= 2:       # LINE_STRIP
        shape = Geometry(lines=(points,))
    elif kind == 5 and len(points) >= 2:     # LINE_LIST
        shape = Geometry(segments=(points[:len(points)//2*2].reshape(-1, 2, 3),))
    elif kind == 11 and len(points) >= 3:    # TRIANGLE_LIST
        shape = Geometry(triangles=(points[:len(points)//3*3].reshape(-1, 3, 3),))
    elif kind == 1:                          # CUBE
        scale = np.array([marker.scale.x, marker.scale.y, marker.scale.z])
        vertices = CUBE_VERTICES * scale
        shape = Geometry(segments=(vertices[CUBE_EDGES],), triangles=(vertices[CUBE_TRIANGLES],))
    else:
        return None
    p, q = marker.pose.position, marker.pose.orientation
    rotation = (q.x, q.y, q.z, q.w)
    if not any(rotation):
        rotation = (0., 0., 0., 1.)
    return shape.transformed(matrix((p.x, p.y, p.z), rotation))


def stamp_ns(time):
    return int(time.sec) * 10**9 + int(time.nanosec)


class GeometryTimeline:
    """Replay path replacement and ROS marker identities, deletion and lifetime."""
    def __init__(self):
        self.events = []
        self.times = []
        self.active = {}
        self.cursor = 0

    def add(self, typename, message, recorded):
        if typename == PATH:
            t = stamp_ns(message.header.stamp) or recorded
            points = coordinates(item.pose.position for item in message.poses)
            self.events.append((t, "", 0, "path", message.header.frame_id,
                                Geometry(lines=(points,)) if len(points) >= 2 else None, False, 0))
            return
        for marker in message.markers if typename == MARKERS else (message,):
            t = stamp_ns(marker.header.stamp) or recorded
            shape = marker_shape(marker) if marker.action == 0 else None
            self.events.append((t, marker.ns, marker.id, marker.action, marker.header.frame_id,
                                shape, marker.frame_locked, stamp_ns(marker.lifetime)))

    def prepare(self):
        self.events.sort(key=lambda event: event[0])
        self.times = [event[0] for event in self.events]

    def shapes(self, tree, world, now):
        end = bisect_right(self.times, now)
        if end < self.cursor:
            self.cursor = 0
            self.active.clear()
        for event in self.events[self.cursor:end]:
            t, ns, key, action, frame, shape, locked, lifetime = event
            identity = ns, key, action == "path"
            if action == 3:
                self.active.clear()
            elif action == 2 or shape is None:
                self.active.pop(identity, None)
            elif action in (0, "path"):
                self.active[identity] = (frame, shape, locked, t, lifetime)
        self.cursor = end
        for frame, shape, locked, t, lifetime in self.active.values():
            if lifetime > 0 and now >= t + lifetime:
                continue
            try:
                yield shape.transformed(tree.lookup(world, frame, now if locked else t))
            except LookupError:
                continue


def demo_plan(source, t):
    times = np.minimum(t + np.arange(0, 4.01, .2)*1e9, source.last)
    points = np.array([source.pose(round(s))[:3,3] for s in times])
    return points[np.r_[True, np.any(np.diff(points, axis=0), axis=1)]]


def demo_corridor(path):
    if len(path) < 2:
        return Geometry()
    starts = path[np.arange(0, len(path)-1, 5)]
    ends = path[np.minimum(np.arange(0, len(path)-1, 5)+5, len(path)-1)]
    boxes = []
    for start, end in zip(starts, ends):
        forward = end[:2] - start[:2]
        length = np.linalg.norm(forward)
        if length < .01:
            continue
        forward /= length
        left = np.array([-forward[1], forward[0]])
        vertices = CUBE_VERTICES * [length+.75, 2.6, 1.65]
        vertices[:, :2] = vertices[:, :2] @ np.array([forward, left])
        boxes.append(vertices + (start+end)/2)
    return Geometry(segments=tuple(box[CUBE_EDGES] for box in boxes),
                    triangles=tuple(box[CUBE_TRIANGLES] for box in boxes))


@dataclass(frozen=True)
class SpatialFrame:
    plan: tuple
    corridor: tuple
    camera: np.ndarray | None
    lidar: np.ndarray | None
    scan: np.ndarray | None


def sensor_settings(args, name):
    values = getattr(args, name + "_fov", None)
    if values is None:
        return None
    frame, horizontal, vertical, distance, *elevation = values
    return SensorView(frame, (float(horizontal), float(vertical)), float(distance),
                      name == "camera", float(elevation[0]) if elevation else 0.)


class SpatialOverlays:
    def __init__(self, args, source, stack):
        self.source = source
        self.demo = args.command == "demo"
        self.plan = getattr(args, "plan_topic", None) or getattr(args, "show_plan", False)
        self.corridor = getattr(args, "corridor_topic", None) or getattr(args, "show_corridor", False)
        self.show_scan = getattr(args, "show_scan", False) or getattr(args, "scan_topic", None)
        self.scan_age = round(getattr(args, "scan_max_age", .5) * 1e9)
        self.sensors = {name: (sensor, sensor_edges(sensor).reshape(-1, 3))
                        for name in ("camera", "lidar") if (sensor := sensor_settings(args, name))}
        self.scans = None
        self.pending = None
        self.latest = (0, None)
        if getattr(args, "scan_topic", None) and args.scan_topic != args.cloud_topic:
            self.scans = source.scans()
            stack.callback(self.scans.close)
            self.pending = next(self.scans, None)
        if self.sensors and not self.demo:
            frames = {source.world, source.body, *(frame for pair in source.tree.edges for frame in pair)}
            for sensor, _ in self.sensors.values():
                if sensor.frame.lstrip("/") not in frames:
                    raise ValueError(f"找不到传感器坐标系 {sensor.frame}，请检查 TF 或外参 JSON。")

    def __bool__(self):
        return bool(self.plan or self.corridor or self.show_scan or self.sensors)

    def update_map_scan(self, stamp, xyz):
        if self.show_scan and self.scans is None:
            self.latest = stamp, xyz

    def frame(self, stamp, body_pose, check_cancel=None):
        while self.pending is not None and self.pending[0] <= stamp:
            if check_cancel is not None:
                check_cancel()
            self.latest = self.pending
            self.pending = next(self.scans, None)
        scan = self.latest[1] if stamp - self.latest[0] <= self.scan_age else None
        if self.demo:
            path = demo_plan(self.source, stamp) if self.plan or self.corridor else ()
            plan = (Geometry(lines=(path,)),) if self.plan and len(path) >= 2 else ()
            corridor = (demo_corridor(path),) if self.corridor else ()
        else:
            plan = self.source.mission_shapes("plan", stamp) if self.plan else ()
            corridor = self.source.mission_shapes("corridor", stamp) if self.corridor else ()

        def sensor_frame(name):
            if name not in self.sensors:
                return None
            sensor, points = self.sensors[name]
            if self.demo:
                offset = np.eye(4)
                if sensor.camera:
                    # Optical right, down, forward in a body with +X forward and +Z up.
                    offset[:3,:3] = [[0,0,1], [-1,0,0], [0,-1,0]]
                    offset[:3,3] = [.27, 0, .06]
                else:
                    offset[:3,3] = [0, 0, .18]
                pose = body_pose @ offset
            else:
                try:
                    pose = self.source.sensor_pose(sensor, stamp)
                except LookupError:
                    return None
            return transform_points(pose, points).reshape(-1, 2, 3)

        return SpatialFrame(plan, corridor, sensor_frame("camera"), sensor_frame("lidar"), scan)
