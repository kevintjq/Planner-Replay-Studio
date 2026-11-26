"""Inspect recorded ROS1 topics and decode current scans."""
from collections import defaultdict

import numpy as np

from .cloud import decode
from .spatial import CAMERA_DISPLAY_DEPTH, CLOUD, PATH, EGO_SPLINES, PLAN_TYPES, CORRIDOR_TYPES, GOAL_TYPES, CameraView, coordinates

CAMERA_INFO = "sensor_msgs/msg/CameraInfo"
LASER_SCAN = "sensor_msgs/msg/LaserScan"
LIVOX_TYPES = ("livox_ros_driver/msg/CustomMsg", "livox_ros_driver2/msg/CustomMsg")
POINT_TYPES = (CLOUD, *LIVOX_TYPES)
SCAN_TYPES = (*POINT_TYPES, LASER_SCAN)
ODOM = "nav_msgs/msg/Odometry"
TF = "tf2_msgs/msg/TFMessage"


def planner_map_topic(topic):
    return any(("/" + topic.strip("/")).endswith("/" + suffix) for suffix in (
        "grid_map/occupancy", "grid_map/occupancy_inflate", "rog_map/occ", "rog_map/inf_occ"))


def camera_view(message):
    frame = message.header.frame_id.strip("/")
    if not frame or min(message.width, message.height) <= 0:
        return None
    # K describes the original image. Use P only when K is unavailable.
    k = np.asarray(getattr(message, "K", getattr(message, "k", ()))).reshape(3, 3)
    if not np.isfinite(k).all() or min(k[0, 0], k[1, 1]) <= 0:
        k = np.asarray(getattr(message, "P", getattr(message, "p", ()))).reshape(3, 4)
    fx, fy, cx, cy = k[0, 0], k[1, 1], k[0, 2], k[1, 2]
    if not np.isfinite((fx, fy, cx, cy)).all() or min(fx, fy) <= 0:
        return None
    roi = message.roi
    width, height = roi.width or message.width, roi.height or message.height
    x, y = (roi.x_offset, roi.y_offset) if roi.width and roi.height else (0, 0)
    bounds = ((x-cx)/fx, (x+width-cx)/fx, (y-cy)/fy, (y+height-cy)/fy)
    angles = np.rad2deg(np.diff(np.arctan(bounds).reshape(2, 2), axis=1).ravel())
    if not all(0 < angle < 180 for angle in angles):
        return None
    # Preserve the angles while keeping very wide lenses compact in the scene.
    depth = CAMERA_DISPLAY_DEPTH / max(1., max(map(abs, bounds)))
    return CameraView(frame, tuple(map(float, angles)), depth, bounds)


def scan_points(message):
    kind = message.__msgtype__
    if kind == LASER_SCAN:
        ranges = np.asarray(message.ranges, dtype=float)
        keep = np.isfinite(ranges) & (ranges >= message.range_min) & (ranges <= message.range_max)
        angles = message.angle_min + np.flatnonzero(keep) * message.angle_increment
        ranges = ranges[keep]
        return np.column_stack((ranges*np.cos(angles), ranges*np.sin(angles), np.zeros(len(ranges))))
    if kind in LIVOX_TYPES:
        xyz = coordinates(message.points)
        return xyz[np.isfinite(xyz).all(axis=1)]
    xyz, _ = decode(message)
    return xyz


def inspect_metadata(reader, check_cancel=None):
    """Read early headers and the first valid camera calibration."""
    topics, pairs = [], set()
    for connection in reader.connections:
        topic, kind = connection.topic, connection.msgtype
        info = dict(topic=topic, type=kind, count=connection.msgcount)
        topics.append(info)
        if kind not in (*SCAN_TYPES, ODOM, TF, CAMERA_INFO) or not connection.msgcount:
            continue
        limit = 6 if kind == CAMERA_INFO else (12 if kind == TF else 1)
        for index, (_, _, raw) in enumerate(reader.messages(connections=[connection]), 1):
            if check_cancel is not None:
                check_cancel()
            msg = reader.deserialize(raw, kind)
            if kind == TF:
                pairs.update((item.header.frame_id.strip("/"), item.child_frame_id.strip("/"))
                             for item in msg.transforms)
            else:
                frame = msg.header.frame_id.strip("/")
                info.setdefault("frame", frame)
                if kind == ODOM:
                    info["child"] = msg.child_frame_id.strip("/")
                elif kind == CAMERA_INFO:
                    view = camera_view(msg)
                    if view:
                        info["view"] = view
                        break
            if index >= limit:
                break
    return topics, sorted(pairs)


def is_lidar_cloud(info, world="", body=""):
    frame = info.get("frame", "")
    topic = info["topic"].lower()
    return (bool(frame) and frame not in (world, body)
            and frame.lower() not in {"world", "map", "odom", "camera_init", "body", "base_link"}
            and not any(word in topic for word in ("registered", "map", "global", "accumulated"))
            and not any(word in frame.lower() for word in ("optical", "camera", "depth"))
            and not any(word in topic for word in ("camera", "depth", "stereo")))


def camera_choices(topics):
    matches = (row for row in topics if row["type"] == CAMERA_INFO and row.get("view"))
    return sorted(matches, key=lambda row: ("color" not in row["topic"].lower(), row["topic"]))


def suggested_topic(topics, name, world="", body=""):
    ordered = sorted(topics, key=lambda row: row["topic"])

    def native(types, *names):
        return next((row for ending in names for row in ordered if row["type"] in types
                     and (row["topic"].strip("/") == ending or row["topic"].endswith("/" + ending))), None)

    if name == "scan":
        native = [row for row in ordered if row["type"] in POINT_TYPES and is_lidar_cloud(row, world, body)]
        row = min(native, key=lambda row: (row["type"] not in LIVOX_TYPES,
                  not any(word in row["topic"].lower() for word in ("lidar", "livox", "velodyne", "ouster")),
                  row["topic"])) if native else None
        row = row or next((row for row in ordered if row["type"] == LASER_SCAN and row.get("frame")), None)
    elif name == "plan":
        candidates = [row for row in ordered if row["type"] in PLAN_TYPES and
                      not any(word in row["topic"].lower() for word in ("actual", "odom", "history", "slam"))]
        row = native(PLAN_TYPES, "visualization/exp_traj", "visualization/committed_traj")
        row = row or native(EGO_SPLINES, "planning/bspline")
        row = row or next((row for row in candidates if row["type"] in EGO_SPLINES
                           and "broadcast" not in row["topic"].lower()), None)
        row = row or native(PLAN_TYPES, "optimal_list")
        row = row or next((row for row in candidates if any(word in row["topic"].lower()
                    for word in ("traj", "plan", "path")) and not any(word in row["topic"].lower()
                    for word in ("corridor", "sfc", "polytope"))), None)
        row = row or next((row for row in candidates if row["type"] == PATH), None)
    elif name == "goal":
        row = native(GOAL_TYPES, "visualization/goal", "goal_point", "goal", "goal_pose", "move_base_simple/goal")
    else:
        row = native(CORRIDOR_TYPES, "visualization/exp_sfc")
        row = row or next((row for row in ordered if row["type"] in CORRIDOR_TYPES and any(
            word in row["topic"].lower() for word in ("corridor", "sfc", "polytope"))), None)
    return row["topic"] if row else ""


def connected_frames(world, pairs):
    neighbours = defaultdict(set)
    for parent, child in pairs:
        neighbours[parent].add(child)
        neighbours[child].add(parent)
    seen = {world}
    pending = [world]
    while pending:
        others = neighbours[pending.pop()] - seen
        seen.update(others)
        pending.extend(others)
    return seen


def pose_frames(topics, pairs, odometry, cloud):
    row = next((row for row in topics if row["topic"] == odometry and row.get("child")), None)
    if row:
        return row["frame"], row["child"]
    cloud_frame = next((row.get("frame") for row in topics if row["topic"] == cloud), None)
    frames = {frame for pair in pairs for frame in pair if frame}
    children = {child for _, child in pairs}
    roots = sorted(frames - children)
    world = next((item for item in (cloud_frame, "map", "world", "camera_init", "odom")
                  if item in roots), roots[0] if roots else cloud_frame)
    body = next((item for item in ("body", "base_link", "base_footprint", "drone", "uav")
                 if item in children), None)
    if body is None:
        body = next((child for parent, child in pairs if parent == world and
                     not any(name in child.lower() for name in ("camera", "optical", "lidar", "laser"))), None)
    return world or "", body or ""
