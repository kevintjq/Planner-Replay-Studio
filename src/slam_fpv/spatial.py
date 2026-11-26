"""Optional recorded mission geometry and sensor overlays for offline rendering."""
from bisect import bisect_right
from dataclasses import dataclass
from math import isfinite
from hashlib import blake2b

import numpy as np

from .geometry import matrix, transform_points

PATH = "nav_msgs/msg/Path"
MARKER = "visualization_msgs/msg/Marker"
MARKERS = "visualization_msgs/msg/MarkerArray"
CLOUD = "sensor_msgs/msg/PointCloud2"
EGO_SPLINES = ("ego_planner/msg/Bspline", "traj_utils/msg/Bspline", "plan_manage/msg/Bspline")
PLAN_TYPES = (PATH, MARKER, MARKERS, *EGO_SPLINES)
CORRIDOR_TYPES = (MARKER, MARKERS)
POSE_STAMPED = "geometry_msgs/msg/PoseStamped"
POINT_STAMPED = "geometry_msgs/msg/PointStamped"
GOAL_TYPES = (MARKER, MARKERS, POSE_STAMPED, POINT_STAMPED)


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
    color: tuple | None = None
    goals: tuple = ()

    def transformed(self, transform):
        def move(groups):
            return tuple(transform_points(transform, item) for item in groups)
        return Geometry(move(self.lines), move(self.segments), move(self.triangles), self.color, move(self.goals))


def coordinates(points):
    return np.array([(p.x, p.y, p.z) for p in points], dtype=float).reshape(-1, 3)


def ego_curve(message):
    """Evaluate EGO's ROS1 B-spline in its knot domain with de Boor's algorithm."""
    points = coordinates(message.pos_pts)
    knots = np.asarray(message.knots, dtype=float)
    degree = int(message.order)
    count = len(points)
    if not (1 <= degree < count and degree <= 9 and len(knots) == count + degree + 1
            and np.isfinite(knots).all() and np.isfinite(points).all() and np.all(knots[1:] >= knots[:-1])):
        return None
    start, end = knots[degree], knots[count]
    if end <= start or not isfinite(end - start):
        return None
    size = int(np.clip(np.ceil((end-start)/.03), 16, 800)) + 1
    parameters = np.linspace(start, end, size)
    spans = np.clip(np.searchsorted(knots, parameters, side="right")-1, degree, count-1)
    values = points[spans[:, None]-degree+np.arange(degree+1)]
    for step in range(1, degree+1):
        for j in range(degree, step-1, -1):
            subspan = spans-degree+j
            left = knots[subspan]
            span = knots[subspan+degree-step+1]-left
            fraction = np.divide(parameters-left, span, out=np.zeros_like(left), where=span > 0)[:, None]
            values[:, j] = (1-fraction)*values[:, j-1] + fraction*values[:, j]
    return Geometry(lines=(values[:, degree],))


CUBE_TRIANGLES = np.array([[0,2,1], [0,3,2], [4,5,6], [4,6,7], [0,1,5], [0,5,4],
                           [1,2,6], [1,6,5], [2,3,7], [2,7,6], [3,0,4], [3,4,7]])
CUBE_EDGES = np.array([[0,1], [1,2], [2,3], [3,0], [4,5], [5,6], [6,7], [7,4],
                       [0,4], [1,5], [2,6], [3,7]])
CUBE_VERTICES = np.array([[-1,-1,-1], [1,-1,-1], [1,1,-1], [-1,1,-1],
                          [-1,-1,1], [1,-1,1], [1,1,1], [-1,1,1]]) / 2


def marker_shape(marker, *, goal=False):
    points = coordinates(marker.points)
    kind = marker.type
    if goal:
        if kind in (1, 2) or (kind == 0 and not len(points)):  # CUBE, SPHERE, pose-style ARROW
            shape = Geometry(goals=(np.zeros((1, 3)),))
        elif kind in (7, 8) and len(points):                  # SPHERE_LIST, POINTS
            shape = Geometry(goals=(points,))
        elif kind in (0, 4, 5) and len(points):              # Endpoint of a line or arrow
            shape = Geometry(goals=(points[-1:],))
        else:
            return None
    elif kind == 0 and len(points) in (0, 2):                # ARROW, including SUPER segments
        segment = points if len(points) else np.array([[0., 0., 0.], [marker.scale.x, 0., 0.]])
        color = (marker.color.r, marker.color.g, marker.color.b)
        shape = Geometry(segments=(segment[None, :, :],),
                         color=tuple(np.clip(color, 0., 1.)) if all(map(isfinite, color)) else None)
    elif kind == 4 and len(points) >= 2:                     # LINE_STRIP
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
    if not all(np.isfinite(item).all() for group in (shape.lines, shape.segments, shape.triangles, shape.goals)
               for item in group):
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
    def __init__(self, *, goal=False, native_goal=False, plan_frame="world"):
        self.goal = goal
        self.native_goal = native_goal
        self.plan_frame = plan_frame
        self.events = []
        self.updates = []
        self.times = []
        self.active = {}
        self.cursor = 0

    def add(self, typename, message, recorded):
        if typename in EGO_SPLINES:
            shape = ego_curve(message)
            if shape is not None:
                t = stamp_ns(message.start_time) or recorded
                self.events.append((t, "", 0, "path", self.plan_frame, shape, False, 0, False))
                self.record_update(t, [(self.plan_frame, shape)])
            return
        if typename in (PATH, POSE_STAMPED, POINT_STAMPED):
            t = stamp_ns(message.header.stamp) or recorded
            if typename == PATH:
                points = coordinates(item.pose.position for item in message.poses)
                shape = Geometry(lines=(points,)) if len(points) >= 2 else None
            else:
                point = message.pose.position if typename == POSE_STAMPED else message.point
                points = coordinates((point,))
                shape = Geometry(goals=(points,)) if np.isfinite(points).all() else None
            self.events.append((t, "", 0, "path", message.header.frame_id, shape, False, 0, False))
            self.record_update(t, [(message.header.frame_id, shape)])
            return
        markers = message.markers if typename == MARKERS else (message,)
        native_goal = self.native_goal or (self.goal and any(marker.ns == "goal_line" for marker in markers))
        if native_goal:
            # SUPER publishes [goal sphere, vehicle sphere, connecting line].
            target = next((marker for marker in markers if marker.action == 0 and marker.type == 2
                           and marker.ns == "goal"), None)
            markers = [marker for marker in markers if marker.action != 0] + ([target] if target else [])
        updates = []
        for marker in markers:
            header_time = stamp_ns(marker.header.stamp)
            shape = marker_shape(marker, goal=self.goal) if marker.action == 0 else None
            namespace, identity = marker.ns, marker.id
            action = "target" if native_goal and shape is not None else marker.action
            self.events.append((header_time or recorded, namespace, identity, action,
                                marker.header.frame_id, shape, marker.frame_locked,
                                stamp_ns(marker.lifetime), not header_time and marker.action in (2, 3)))
            if shape is not None:
                updates.append((header_time or recorded, marker.header.frame_id, shape))
        if updates:
            self.record_update(max(stamp for stamp, _, _ in updates), [(frame, shape) for _, frame, shape in updates])

    def record_update(self, stamp, shapes):
        digest = blake2b(digest_size=12)
        count = 0
        for frame, shape in shapes:
            if shape is None:
                continue
            for group in (*shape.lines, *shape.segments, *shape.goals):
                digest.update(frame.encode())
                digest.update(np.round(group, 3).tobytes())
                count += len(group)
        if count:
            self.updates.append((stamp, digest.digest()))

    def change_times(self):
        previous = None
        for stamp, digest in sorted(self.updates, key=lambda entry: entry[0]):
            if digest != previous:
                yield stamp
            previous = digest

    def prepare(self):
        # A zero-stamped delete sent just before a replacement may be recorded after
        # the replacement's header time. Keep the delete before that next marker.
        next_stamp = None
        for index in range(len(self.events)-1, -1, -1):
            event = self.events[index]
            if event[-1]:
                if next_stamp is not None and next_stamp < event[0]:
                    self.events[index] = (next_stamp, *event[1:])
            elif event[3] in (0, "path", "target"):
                next_stamp = event[0]
        self.events.sort(key=lambda event: event[0])
        self.times = [event[0] for event in self.events]

    def shapes(self, tree, world, now):
        end = bisect_right(self.times, now)
        if end < self.cursor:
            self.cursor = 0
            self.active.clear()
        for event in self.events[self.cursor:end]:
            t, ns, key, action, frame, shape, locked, lifetime, _ = event
            identity = ns, key, action == "path"
            if action == 3:
                self.active.clear()
            elif action == 2 or shape is None:
                self.active.pop(identity, None)
            elif action in (0, "path", "target"):
                if action == "target":
                    self.active.clear()
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
    goal: tuple = ()
    previous_plan: tuple = ()


def camera_settings(args):
    values = getattr(args, "camera_fov", None)
    if values is None:
        return None
    frame, horizontal, vertical, distance = values
    return CameraView(frame, (float(horizontal), float(vertical)), float(distance))


class SpatialOverlays:
    def __init__(self, args, source, stack, *, compare_plans=False):
        self.source = source
        self.demo = args.command == "demo"
        self.plan = getattr(args, "plan_topic", None) or getattr(args, "show_plan", False)
        self.corridor = getattr(args, "corridor_topic", None) or getattr(args, "show_corridor", False)
        self.goal = getattr(args, "goal_topic", None) or getattr(args, "show_goal", False)
        self.show_scan = getattr(args, "show_scan", False) or getattr(args, "scan_topic", None)
        self.scan_age = round(getattr(args, "scan_max_age", .5) * 1e9)
        self.wants_camera = getattr(args, "show_camera_fov", False) or bool(getattr(args, "camera_fov", None))
        self.camera = camera_settings(args)
        if self.camera is None and getattr(args, "show_camera_fov", False):
            self.camera = (CameraView("camera_optical_frame", (82, 53)) if self.demo
                           else source.detected_camera)
        self.camera_points = camera_edges(self.camera).reshape(-1, 3) if self.camera else None
        self.scans = None
        self.pending = None
        self.latest = (0, None)
        self.plan_updates = tuple(source.mission_times("plan")) if compare_plans and not self.demo else ()
        self.comparison_stamp = None
        self.previous_plan = ()
        if not self.demo and source.scan_topic and source.scan_topic != source.cloud_topic:
            self.scans = source.scans()
            stack.callback(self.scans.close)
            self.pending = next(self.scans, None)
        if self.camera and not self.demo:
            frames = {source.world, source.body, *(frame for pair in source.tree.edges for frame in pair)}
            if self.camera.frame.lstrip("/") not in frames:
                raise ValueError(f"找不到相机坐标系 {self.camera.frame}，请检查 TF 或外参 JSON。")

    def __bool__(self):
        return bool(self.plan or self.corridor or self.goal or self.show_scan or self.camera)

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
            goal = (Geometry(goals=(self.source.pose(self.source.last)[:3, 3][None, :],)),) if self.goal else ()
        else:
            self.update_comparison(stamp)
            plan = self.source.mission_shapes("plan", stamp) if self.plan else ()
            corridor = self.source.mission_shapes("corridor", stamp) if self.corridor else ()
            goal = self.source.mission_shapes("goal", stamp) if self.goal else ()

        return SpatialFrame(plan, corridor, self.camera_frame(stamp, body_pose), scan, goal, self.previous_plan)

    def update_comparison(self, stamp):
        index = bisect_right(self.plan_updates, stamp) - 1
        event = self.plan_updates[index] if index > 0 else None
        if event is not None and stamp-event > 1_500_000_000:
            event = None
        if event != self.comparison_stamp:
            self.comparison_stamp = event
            self.previous_plan = self.source.mission_shapes("plan", event-1) if event is not None else ()

    def status(self, stamp, frame):
        lines = []
        if self.show_scan:
            topic = "模拟扫描" if self.demo else (self.source.scan_topic or self.source.cloud_topic)
            last, points = self.latest
            if last == 0:
                state = "等待首帧"
            elif stamp - last > self.scan_age:
                state = f"扫描已过期 ({(stamp-last)/1e9:.2f} s)"
            elif points is None:
                state = "当前帧缺少 TF"
            elif not len(points):
                state = "当前帧无有效点"
            else:
                state = f"{len(points):,} 点 · 帧龄 {(stamp-last)/1e6:.0f} ms"
            lines.append(f"雷达 {topic}  ·  {state}")
        if self.wants_camera:
            if self.camera is None:
                state = self.source.camera_issue
            elif frame is None or frame.camera is None:
                state = f"{self.camera.frame} 当前缺少 TF"
            else:
                topic = "模拟标定" if self.demo else (self.source.camera_topic or "手动标定")
                state = f"{topic} · {self.camera.frame}"
            lines.append(f"相机 {state}")
        for name, enabled, label in (("plan", self.plan, "规划轨迹"), ("corridor", self.corridor, "安全走廊"),
                                     ("goal", self.goal, "目标点")):
            if enabled and (frame is None or not any(shape.lines or shape.segments or shape.triangles or shape.goals
                                                     for shape in getattr(frame, name))):
                lines.append(f"{label}当前无可绘制数据")
        return tuple(lines)

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
