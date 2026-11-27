"""T_parent_child maps child coordinates into parent, quaternion xyzw."""
from bisect import bisect_left
from collections import deque
import numpy as np


def quaternion(q):
    q = np.asarray(q, dtype=float)
    n = np.linalg.norm(q)
    if q.shape != (4,) or not np.isfinite(q).all() or n < 1e-12:
        raise ValueError("Invalid quaternion (expected nonzero finite xyzw)")
    return q / n


def matrix(position, q):
    x, y, z, w = quaternion(q)
    T = np.eye(4)
    T[:3, :3] = [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                 [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                 [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]
    T[:3, 3] = position
    if not np.isfinite(T).all():
        raise ValueError("Nonfinite transform")
    return T


def slerp(a, b, f):
    a, b = quaternion(a), quaternion(b)
    d = float(np.dot(a, b))
    if d < 0:
        b, d = -b, -d
    if d > .9995:
        return quaternion(a + f*(b-a))
    angle = np.arccos(np.clip(d, -1, 1))
    return (np.sin((1-f)*angle)*a + np.sin(f*angle)*b)/np.sin(angle)


class Series:
    def __init__(self):
        self.samples = {}
        self.times = []

    def add(self, stamp, p, q):
        self.samples[int(stamp)] = (np.asarray(p, float), quaternion(q))
        self.times = []

    def prepare(self):
        self.times = sorted(self.samples)

    def at(self, stamp, max_gap):
        if not self.times:
            self.prepare()
        i = bisect_left(self.times, stamp)
        if i < len(self.times) and self.times[i] == stamp:
            return matrix(*self.samples[stamp])
        if i == 0 or i == len(self.times):
            raise LookupError("Transform extrapolation rejected")
        t0, t1 = self.times[i-1:i+1]
        if t1-t0 > max_gap*1e9:
            raise LookupError("Transform interpolation gap too large")
        f = (stamp-t0)/(t1-t0)
        p0, q0 = self.samples[t0]
        p1, q1 = self.samples[t1]
        return matrix(p0+f*(p1-p0), slerp(q0, q1, f))


class TransformTree:
    def __init__(self, max_gap=.5):
        self.edges = {}
        self.max_gap = max_gap

    def add(self, parent, child, stamp, p, q, static=False):
        parent, child = parent.lstrip('/'), child.lstrip('/')
        if not parent or not child or parent == child:
            raise ValueError("Invalid TF frame pair")
        parents = {c:p for p,c in self.edges}
        if child in parents and parents[child] != parent:
            raise ValueError(f"Conflicting TF parents for {child}")
        ancestor = parent
        while ancestor in parents:
            ancestor = parents[ancestor]
            if ancestor == child:
                raise ValueError("TF cycle rejected")
        key = (parent, child)
        if static:
            value = matrix(p, q)
            if key in self.edges and not isinstance(self.edges[key], np.ndarray):
                raise ValueError("Static/dynamic TF conflict")
            if key in self.edges and not np.allclose(self.edges[key], value):
                raise ValueError("Changing static TF")
            self.edges[key] = value
        else:
            if key in self.edges and isinstance(self.edges[key], np.ndarray):
                raise ValueError("Static/dynamic TF conflict")
            self.edges.setdefault(key, Series()).add(stamp, p, q)

    def prepare(self):
        for value in self.edges.values():
            if isinstance(value, Series):
                value.prepare()

    def lookup(self, target, source, stamp):
        target, source = target.lstrip('/'), source.lstrip('/')
        queue = deque([(source, np.eye(4))])
        seen = {source}
        while queue:
            frame, T_frame_source = queue.popleft()
            if frame == target:
                return T_frame_source
            for (parent, child), value in self.edges.items():
                if frame not in (parent, child):
                    continue
                other = parent if frame == child else child
                if other in seen:
                    continue
                try:
                    T = value if isinstance(value, np.ndarray) else value.at(stamp, self.max_gap)
                except LookupError:
                    continue
                if frame == parent:
                    T = np.linalg.inv(T)
                seen.add(other)
                queue.append((other, T @ T_frame_source))
        raise LookupError(f"No timestamped TF path {target} <- {source} at {stamp}")


def transform_points(T, xyz):
    return xyz @ T[:3, :3].T + T[:3, 3]


def camera(pose, distance=3.8, height=1.35, side=0., lookahead=1.5):
    # Yaw only: optical camera stays level while RGB axes show all six DoF.
    heading = np.arctan2(pose[1, 0], pose[0, 0])
    forward = np.array([np.cos(heading), np.sin(heading), 0])
    left = np.array([-forward[1], forward[0], 0])
    p = pose[:3, 3]
    return p-distance*forward+side*left+[0, 0, height], p+lookahead*forward


def level_fpv_pose(pose, angles):
    """Construct a camera pose from yaw and softened pitch with a level horizon."""
    yaw, pitch = angles
    pitch = np.clip(.30 * pitch, -np.deg2rad(12), np.deg2rad(12))
    horizontal = np.array([np.cos(yaw), np.sin(yaw), 0.])
    left = np.array([-horizontal[1], horizontal[0], 0.])
    forward = horizontal * np.cos(pitch) + np.array([0., 0., np.sin(pitch)])
    result = pose.copy()
    result[:3, :3] = np.column_stack((forward, left, np.cross(forward, left)))
    return result


def fpv_angles(pose):
    x, y, z = pose[:3, 0]
    return np.array([np.arctan2(y, x), np.arctan2(z, np.hypot(x, y))])


def stabilized_fpv_pose(source, stamp, pose):
    """Smooth only the viewing pose in a symmetric, frame-rate independent time window."""
    reference = fpv_angles(pose)
    angles = reference.copy()
    position = pose[:3, 3].copy()
    total = 1.
    for direction in (-1, 1):
        for offset, weight in ((90_000_000, .88), (180_000_000, .54), (270_000_000, .23)):
            sample_time = stamp + direction * offset
            if not source.first <= sample_time <= source.last:
                break
            try:
                sample = source.pose(sample_time)
            except LookupError:
                # Stay on the current side of any unavailable pose interval.
                break
            sample_angles = fpv_angles(sample)
            delta = sample_angles[0] - reference[0]
            sample_angles[0] = reference[0] + np.arctan2(np.sin(delta), np.cos(delta))
            angles += weight * sample_angles
            position += weight * sample[:3, 3]
            total += weight
    result = level_fpv_pose(pose, angles / total)
    result[:3, 3] = position / total
    return result
