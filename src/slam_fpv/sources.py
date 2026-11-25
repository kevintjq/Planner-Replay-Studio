"""Offline ROS1 bag reader. Bag storage time never substitutes for header time."""
from pathlib import Path
import numpy as np
from rosbags.highlevel import AnyReader
from .geometry import TransformTree, transform_points
from .cloud import decode


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

    def index(self):
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
