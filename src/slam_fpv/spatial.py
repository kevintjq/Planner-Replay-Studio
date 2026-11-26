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


CAMERA_DISPLAY_DEPTH = .85


@dataclass(frozen=True)
class CameraView:
    frame: str
    angles: tuple
    distance: float = CAMERA_DISPLAY_DEPTH
    image_bounds: tuple | None = None

    def __post_init__(self):
        if not self.frame.strip("/") or not all(isfinite(v) for v in (*self.angles, self.distance)):
            raise ValueError("相机坐标系、视场角和距离必须有效。")
        if not all(0 < angle < 180 for angle in self.angles) or self.distance <= 0:
            raise ValueError("相机视场角须在 (0, 180)° 内，距离须大于 0。")


def camera_edges(camera):
    """Camera frustum lines in the optical frame (+Z forward)."""
    x, y = np.tan(np.deg2rad(np.array(camera.angles) / 2))
    left, right, top, bottom = camera.image_bounds or (-x, x, -y, y)
    ring = np.array([[left, top, 1.], [right, top, 1.],
                     [right, bottom, 1.], [left, bottom, 1.]]) * camera.distance
    return np.concatenate((np.stack((ring, np.roll(ring, 1, axis=0)), axis=1),
                           np.stack((np.zeros_like(ring), ring), axis=1)))


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
    scan: np.ndarray | None


def camera_settings(args):
    values = getattr(args, "camera_fov", None)
    if values is None:
        return None
    frame, horizontal, vertical, distance = values
    return CameraView(frame, (float(horizontal), float(vertical)), float(distance))


class SpatialOverlays:
    def __init__(self, args, source, stack):
        self.source = source
        self.demo = args.command == "demo"
        self.plan = getattr(args, "plan_topic", None) or getattr(args, "show_plan", False)
        self.corridor = getattr(args, "corridor_topic", None) or getattr(args, "show_corridor", False)
        self.show_scan = getattr(args, "show_scan", False) or getattr(args, "scan_topic", None)
        self.scan_age = round(getattr(args, "scan_max_age", .5) * 1e9)
        self.camera = camera_settings(args)
        if self.camera is None and getattr(args, "show_camera_fov", False):
            self.camera = (CameraView("camera_optical_frame", (82, 53)) if self.demo
                           else source.detected_camera)
        self.camera_points = camera_edges(self.camera).reshape(-1, 3) if self.camera else None
        self.scans = None
        self.pending = None
        self.latest = (0, None)
        if not self.demo and source.scan_topic and source.scan_topic != source.cloud_topic:
            self.scans = source.scans()
            stack.callback(self.scans.close)
            self.pending = next(self.scans, None)
        if self.camera and not self.demo:
            frames = {source.world, source.body, *(frame for pair in source.tree.edges for frame in pair)}
            if self.camera.frame.lstrip("/") not in frames:
                raise ValueError(f"找不到相机坐标系 {self.camera.frame}，请检查 TF 或外参 JSON。")

    def __bool__(self):
        return bool(self.plan or self.corridor or self.show_scan or self.camera)

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

        return SpatialFrame(plan, corridor, self.camera_frame(stamp, body_pose), scan)

    def camera_frame(self, stamp, body_pose):
        if self.camera is None:
            return None
        if self.demo:
            offset = np.eye(4)
            # Optical right, down, forward in a body with +X forward and +Z up.
            offset[:3, :3] = [[0, 0, 1], [-1, 0, 0], [0, -1, 0]]
            offset[:3, 3] = [.27, 0, .06]
            pose = body_pose @ offset
        else:
            try:
                pose = self.source.sensor_pose(self.camera, stamp)
            except LookupError:
                return None
        return transform_points(pose, self.camera_points).reshape(-1, 2, 3)
