"""Offline ROS1 bag reader. Bag storage time never substitutes for header time."""
from pathlib import Path
import numpy as np
from rosbags.highlevel import AnyReader
from .geometry import TransformTree, transform_points
from .cloud import decode


def inspect_bag(path):
    with AnyReader([path]) as reader:
        return dict(storage_start_ns=reader.start_time, storage_end_ns=reader.end_time,
                    topics=[dict(topic=c.topic,type=c.msgtype,count=c.msgcount) for c in reader.connections])


def stamp(msg):
    t = msg.header.stamp
    sec = getattr(t, 'sec', getattr(t, 'secs', 0))
    ns = getattr(t, 'nanosec', getattr(t, 'nsecs', 0))
    value = int(sec)*10**9+int(ns)
    if value <= 0:
        raise ValueError("Zero/negative header timestamp; repair data explicitly")
    return value


def components(obj):
    p, q = obj.position, obj.orientation
    return [p.x, p.y, p.z], [q.x, q.y, q.z, q.w]


def add_tf(tree, msg, static=False):
    for item in msg.transforms:
        p, q = item.transform.translation, item.transform.rotation
        tree.add(item.header.frame_id, item.child_frame_id, 0 if static else stamp(item),
                 [p.x,p.y,p.z], [q.x,q.y,q.z,q.w], static)


class BagSource:
    def __init__(self, path, cloud_topic, odom_topic, world, body, pose_source='odom', max_gap=.5, extrinsics=()):
        self.path, self.cloud_topic = Path(path), cloud_topic
        self.world, self.body = world.lstrip('/'), body.lstrip('/')
        self.tree = TransformTree(max_gap)
        self.pose_source = pose_source
        self.odom_topic = odom_topic
        self.extrinsics = extrinsics
        self.first = self.last = None
        self.count = 0
        self.leading_cloud_skips = 0

    def index(self, check_cancel=None):
        odom_count = 0
        with AnyReader([self.path]) as reader:
            if reader.is2 or self.path.suffix != '.bag':
                raise ValueError("Expected a ROS1 .bag")
            wanted = [c for c in reader.connections if c.topic in
                      (self.cloud_topic, self.odom_topic, '/tf', '/tf_static')]
            clouds = [c for c in wanted if c.topic == self.cloud_topic]
            if not clouds or any(c.msgtype != 'sensor_msgs/msg/PointCloud2' for c in clouds):
                raise ValueError("Cloud topic missing or not sensor_msgs/PointCloud2; Livox CustomMsg needs SLAM/driver conversion")
            previous = None
            for c, _, raw in reader.messages(connections=wanted):
                if check_cancel is not None:
                    check_cancel()
                msg = reader.deserialize(raw, c.msgtype)
                if c.topic == self.cloud_topic:
                    t = stamp(msg)
                    if previous is not None and t < previous:
                        raise ValueError("Cloud header time moved backwards; split/sort bag first")
                    previous = t
                    self.first = t if self.first is None else min(self.first, t)
                    self.last = t if self.last is None else max(self.last, t)
                    self.count += 1
                elif c.topic in ('/tf','/tf_static'):
                    # Selected odometry is authoritative for this child, avoid duplicate publishers.
                    if self.pose_source == 'odom':
                        msg.transforms = [x for x in msg.transforms if x.child_frame_id.lstrip('/') != self.body]
                    add_tf(self.tree, msg, c.topic == '/tf_static')
                elif c.topic == self.odom_topic and self.pose_source == 'odom':
                    if c.msgtype != 'nav_msgs/msg/Odometry':
                        raise ValueError("Odometry topic type mismatch")
                    if msg.child_frame_id.lstrip('/') != self.body:
                        raise ValueError("Odometry child_frame_id does not match --body")
                    p, q = components(msg.pose.pose)
                    self.tree.add(msg.header.frame_id, msg.child_frame_id, stamp(msg), p, q)
                    odom_count += 1
        if self.first is None or (self.pose_source == 'odom' and not odom_count):
            raise ValueError("Missing cloud or odometry samples")
        for parent, child, p, q in self.extrinsics:
            if (parent.lstrip('/'),child.lstrip('/')) in self.tree.edges:
                raise ValueError("Extrinsic duplicates recorded TF")
            self.tree.add(parent, child, 0, p, q, True)
        self.tree.prepare()
        return self

    def clouds(self):
        started = False
        with AnyReader([self.path]) as reader:
            cs = [c for c in reader.connections if c.topic == self.cloud_topic]
            for c, _, raw in reader.messages(connections=cs):
                msg = reader.deserialize(raw,c.msgtype)
                t = stamp(msg)
                xyz, rgb = decode(msg)
                try:
                    T = self.tree.lookup(self.world, msg.header.frame_id, t)
                except LookupError:
                    if started:
                        raise
                    self.leading_cloud_skips += 1
                    continue
                started = True
                yield t, transform_points(T, xyz), rgb

    def pose(self, t):
        return self.tree.lookup(self.world, self.body, t)


class DemoSource:
    """Deterministic synthetic room and prescribed motion, never a measured flight."""
    def __init__(self, seconds=6):
        self.first, self.last = 10**9, int((seconds+1)*1e9)
        self.count = int(seconds*10)+1

    def pose(self, t):
        from .geometry import matrix
        s = (t-self.first)/1e9
        p = [s*.8, np.sin(s*1.2)*1.1, 1.2+.15*np.sin(s)]
        yaw = np.arctan2(1.32*np.cos(s*1.2),.8)
        roll = .12*np.sin(s*1.2)
        return matrix(p, [np.sin(roll/2)*np.cos(yaw/2),np.sin(roll/2)*np.sin(yaw/2),
                          np.cos(roll/2)*np.sin(yaw/2),np.cos(roll/2)*np.cos(yaw/2)])

    def clouds(self):
        rng = np.random.default_rng(7)
        n=14000
        floor = np.column_stack([rng.uniform(-4,14,n),rng.uniform(-6,6,n),rng.normal(0,.015,n)])
        walls = np.column_stack([rng.uniform(-4,14,n),rng.choice([-6.,6.],n),rng.uniform(0,4,n)])
        pillars=[]
        for x,y in [(3,-1.7),(6,1.7),(9,-1)]:
            a=rng.uniform(0,2*np.pi,3500)
            pillars.append(np.column_stack([x+.45*np.cos(a),y+.45*np.sin(a),rng.uniform(0,3.5,len(a))]))
        xyz=np.concatenate([floor,walls,*pillars])
        for t in range(self.first,self.last+1,100000000):
            # Incrementally reveal points within a forward-moving range.
            x=self.pose(t)[0,3]
            mask=(xyz[:,0]<x+5)&(xyz[:,0]>x-5)
            yield t, xyz[mask], None


class TrailDemoSource(DemoSource):
    """Synthetic roadside LiDAR-like returns, revealed as a vehicle follows the trail."""

    @staticmethod
    def center(x):
        return 5.4 * np.sin(x / 8.3) + .1 * x

    @staticmethod
    def ground(x):
        return .24 * np.sin(x / 5.8)

    def pose(self, t):
        from .geometry import matrix
        x = (t - self.first) / 1e9 * 2.8
        yaw = np.arctan(.1 + 5.4 / 8.3 * np.cos(x / 8.3))
        return matrix([x, self.center(x), 1.25 + self.ground(x)],
                      [0, 0, np.sin(yaw / 2), np.cos(yaw / 2)])

    def clouds(self):
        rng = np.random.default_rng(27)
        start, end = -7, (self.last - self.first) / 1e9 * 2.8 + 12
        # Narrow sweep bands on the road and uneven strips behind the shoulders.
        x, d = np.meshgrid(np.arange(start, end, .061), np.arange(-5.5, 5.6, .060))
        x, d = x.ravel(), d.ravel()
        d += rng.normal(0, .032, len(d))
        y = self.center(x) + d
        z = self.ground(x) + np.maximum(0, np.abs(d) - 2) ** 1.7 * (
            .10 + .034 * np.cos(x / 2.4)) + rng.normal(0, .024, len(x))
        clouds = [np.column_stack((x, y, z))]
        for x in np.arange(start + 1, end, .69):
            for side in (-1, 1):
                if rng.random() < .18:
                    continue
                ground = self.ground(x)
                y = self.center(x) + side * rng.uniform(3.1, 5.5)
                height = rng.uniform(.75, 4.4) * (1 if side > 0 else .72)
                samples = rng.integers(260, 680)
                h = rng.uniform(0, height, samples)
                ar = rng.uniform(0, 2 * np.pi, samples)
                r = rng.uniform(.07, .69, samples) * (.35 + .65 * np.sin(np.pi * h / height))
                clouds.append(np.column_stack((x + np.cos(ar) * r, y + np.sin(ar) * r, ground + h)))
        xyz = np.concatenate(clouds)
        xyz = xyz[np.argsort(xyz[:, 0])]
        previous = 0
        for t in range(self.first, self.last + 1, 100_000_000):
            front = self.pose(t)[0, 3] + 10
            end_index = np.searchsorted(xyz[:, 0], front)
            if end_index > previous:
                yield t, xyz[previous:end_index], None
            previous = end_index
