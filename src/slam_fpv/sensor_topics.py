"""Read ROS1 topic metadata and estimate display envelopes in sensor-local frames."""
from collections import defaultdict

import numpy as np

from .cloud import decode
from .spatial import CLOUD, PATH, PLAN_TYPES, CORRIDOR_TYPES, SensorView, coordinates

CAMERA_INFO = "sensor_msgs/msg/CameraInfo"
LASER_SCAN = "sensor_msgs/msg/LaserScan"
LIVOX_TYPES = ("livox_ros_driver/msg/CustomMsg", "livox_ros_driver2/msg/CustomMsg")
POINT_TYPES = (CLOUD, *LIVOX_TYPES)
SCAN_TYPES = (*POINT_TYPES, LASER_SCAN)
ODOM = "nav_msgs/msg/Odometry"
TF = "tf2_msgs/msg/TFMessage"


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
    return SensorView(frame, tuple(map(float, angles)), 5., True, image_bounds=bounds)


def laser_view(message):
    frame = message.header.frame_id.strip("/")
    span = float(np.rad2deg(message.angle_max - message.angle_min))
    center = float(np.rad2deg((message.angle_max + message.angle_min) / 2))
    radius = min(8., float(message.range_max))
    if not frame or not np.isfinite((span, center, radius)).all() or span <= 0 or radius <= 0:
        return None
    return SensorView(frame, (min(360., span), 0.), radius, False, azimuth=center)


def scan_points(message, limit=None):
    kind = message.__msgtype__
    if kind == LASER_SCAN:
        ranges = np.asarray(message.ranges, dtype=float)
        keep = np.isfinite(ranges) & (ranges >= message.range_min) & (ranges <= message.range_max)
        angles = message.angle_min + np.flatnonzero(keep) * message.angle_increment
        ranges = ranges[keep]
        return np.column_stack((ranges*np.cos(angles), ranges*np.sin(angles), np.zeros(len(ranges))))
    if kind in LIVOX_TYPES:
        points = message.points
        stride = max(1, (len(points)+limit-1)//limit) if limit else 1
        xyz = coordinates(points[::stride])
        return xyz[np.isfinite(xyz).all(axis=1)]
    xyz, _ = decode(message)
    stride = max(1, (len(xyz)+limit-1)//limit) if limit else 1
    return xyz[::stride]


def cloud_view(frame, samples):
    xyz = np.concatenate(samples)
    if len(xyz) < 30 or not frame:
        return None
    radius = np.linalg.norm(xyz, axis=1)
    xyz, radius = xyz[radius > .05], radius[radius > .05]
    if len(xyz) < 30:
        return None
    azimuth = np.mod(np.rad2deg(np.arctan2(xyz[:, 1], xyz[:, 0])), 360.)
    # Find the smallest circular sector containing the occupied 2° bins.
    occupied = np.unique((azimuth / 2).astype(int))
    if len(occupied) < 2:
        return None
    gaps = np.diff(np.r_[occupied, occupied[0]+180])
    gap = int(gaps.argmax())
    span = float(360 - max(0, gaps[gap]-1)*2)
    if span >= 350:
        span = 360.
    center = float((occupied[(gap+1) % len(occupied)]*2 + span/2) % 360)
    elevation = np.rad2deg(np.arctan2(xyz[:, 2], np.hypot(xyz[:, 0], xyz[:, 1])))
    low, high = map(float, np.percentile(elevation, [.5, 99.5]))
    return SensorView(frame, (span, high-low), min(8., float(np.percentile(radius, 95))), False,
                      (low+high)/2, center)


def inspect_metadata(reader, check_cancel=None):
    """Read only a few early packets per topic, keeping world clouds out of FOV inference."""
    topics, pairs = [], set()
    for connection in reader.connections:
        topic, kind = connection.topic, connection.msgtype
        info = dict(topic=topic, type=kind, count=connection.msgcount)
        topics.append(info)
        if kind not in (*SCAN_TYPES, ODOM, TF, CAMERA_INFO) or not connection.msgcount:
            continue
        samples = defaultdict(list)
        limit = 6 if kind in (*SCAN_TYPES, CAMERA_INFO) else (12 if kind == TF else 1)
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
                elif kind in (CAMERA_INFO, LASER_SCAN):
                    sensor = camera_view(msg) if kind == CAMERA_INFO else laser_view(msg)
                    if sensor:
                        info["view"] = sensor
                        break
                elif kind in POINT_TYPES and is_lidar_cloud(info):
                    try:
                        samples[frame].append(scan_points(msg, limit=3000))
                    except ValueError:
                        break
            if index >= limit:
                break
        if len(samples) == 1:
            frame, points = next(iter(samples.items()))
            sensor = cloud_view(frame, points)
            if sensor:
                info["view"] = sensor
    return topics, sorted(pairs)


def is_lidar_cloud(info, world="", body=""):
    frame = info.get("frame", "")
    topic = info["topic"].lower()
    return (bool(frame) and frame not in (world, body)
            and frame.lower() not in {"world", "map", "odom", "camera_init", "body", "base_link"}
            and not any(word in topic for word in ("registered", "map", "global", "accumulated"))
            and not any(word in frame.lower() for word in ("optical", "camera", "depth"))
            and not any(word in topic for word in ("camera", "depth", "stereo")))


def sensor_choices(topics, name, world="", body=""):
    matches = [row for row in topics if row.get("view")
               and (row["type"] == CAMERA_INFO if name == "camera"
                    else row["type"] == LASER_SCAN or (row["type"] in POINT_TYPES and is_lidar_cloud(row, world, body)))]
    return sorted(matches, key=lambda row: (
        row["type"] == LASER_SCAN,
        not any(word in row["topic"].lower() for word in ("lidar", "livox", "velodyne", "ouster", "color")),
        row["topic"]))


def suggested_topic(topics, name, world="", body=""):
    ordered = sorted(topics, key=lambda row: row["topic"])
    if name == "scan":
        native = [row for row in ordered if row["type"] in POINT_TYPES and is_lidar_cloud(row, world, body)]
        row = next((row for row in native if row.get("view")), native[0] if native else None)
        row = row or next((row for row in ordered if row["type"] == LASER_SCAN and row.get("frame")), None)
    elif name == "plan":
        candidates = [row for row in ordered if row["type"] in PLAN_TYPES and
                      not any(word in row["topic"].lower() for word in ("actual", "odom", "history", "slam"))]
        row = next((row for row in candidates if any(word in row["topic"].lower()
                    for word in ("traj", "plan", "path")) and not any(word in row["topic"].lower()
                    for word in ("corridor", "sfc", "polytope"))), None)
        row = row or next((row for row in candidates if row["type"] == PATH), None)
    else:
        row = next((row for row in ordered if row["type"] in CORRIDOR_TYPES and any(
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
