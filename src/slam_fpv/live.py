"""Loopback-only ROS1 bridge receiver. Bounded pending clouds and TF history."""
import base64
from collections import deque
import json
import socket
from types import SimpleNamespace
import numpy as np
from .geometry import TransformTree, Series, transform_points
from .sources import stamp, components, add_tf
from .cloud import decode, CloudMap


def objectify(v):
    if isinstance(v,dict): return SimpleNamespace(**{k:objectify(x) for k,x in v.items()})
    if isinstance(v,list): return [objectify(x) for x in v]
    return v


def listen(a):
    from .cli import make_renderer,Export,extrinsics
    if a.delay<0 or not np.isfinite(a.delay): raise ValueError('Invalid --delay')
    tree=TransformTree(a.max_gap)
    for parent,child,p,q in extrinsics(a.extrinsics): tree.add(parent,child,0,p,q,True)
    cloud=CloudMap(a.voxel,a.max_points,scan_height=a.color=="scan",height_limits=a.height_range); trajectory=deque(maxlen=10000)
    pending=deque(maxlen=32); latest=0; first=None; last_cloud=None; frame_index=0
    renderer=make_renderer(a); output=Export(a)
    stats=dict(source='ros1_bridge',frames=0,dropped_clouds=0,complete=False)
    try:
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            server.bind(('127.0.0.1',a.port)); server.listen(1)
            print(f'Waiting for ROS1 bridge on 127.0.0.1:{a.port}',flush=True)
            conn,_=server.accept()
            with conn,conn.makefile('rb') as stream:
                for raw in iter(lambda:stream.readline(32*1024*1024),b''):
                    if len(raw)>=32*1024*1024 or not raw.endswith(b'\n'): raise ValueError('Bridge message exceeds 32 MiB')
                    record=json.loads(raw); topic=record['topic']; msg=objectify(record['message'])
                    if topic==a.cloud_topic:
                        t=stamp(msg)
                        if last_cloud is not None and t<last_cloud: raise ValueError('Cloud clock moved backwards; restart bridge/listener for each replay')
                        last_cloud=t
                        msg.data=base64.b64decode(msg.data)
                        if len(pending)==pending.maxlen: stats['dropped_clouds']+=1
                        pending.append((t,msg))
                    elif topic==a.odom_topic and a.pose_source=='odom':
                        if msg.child_frame_id.lstrip('/')!=a.body.lstrip('/'): raise ValueError('Odometry child frame mismatch')
                        tree.add(msg.header.frame_id,msg.child_frame_id,stamp(msg),*components(msg.pose.pose))
                        latest=max(latest,stamp(msg))
                    elif topic in ('/tf','/tf_static'):
                        if a.pose_source=='odom': msg.transforms=[x for x in msg.transforms if x.child_frame_id.lstrip('/')!=a.body.lstrip('/')]
                        add_tf(tree,msg,topic=='/tf_static')
                        if a.pose_source=='tf' and topic=='/tf':
                            latest=max([latest]+[stamp(x) for x in msg.transforms if x.child_frame_id.lstrip('/')==a.body.lstrip('/')])
                    tree.prepare()
                    while pending and pending[0][0]<=latest-round(a.delay*1e9):
                        t,msg=pending.popleft()
                        try:
                            pose=tree.lookup(a.world,a.body,t)
                            T=tree.lookup(a.world,msg.header.frame_id,t)
                        except LookupError:
                            stats['dropped_clouds']+=1; continue
                        if first is None: first=t; next_frame=t
                        # Render intermediate times using only the previous cloud.
                        while next_frame<t:
                            try: frame_pose=tree.lookup(a.world,a.body,next_frame)
                            except LookupError:
                                stats['dropped_clouds']+=1
                                frame_index+=1; next_frame=first+round(frame_index*1e9/a.fps); continue
                            trajectory.append(frame_pose[:3,3].copy())
                            output.append(renderer.frame(frame_pose,list(trajectory),f'ROS1 BRIDGE | t={(next_frame-first)/1e9:.3f}s'))
                            frame_index+=1; next_frame=first+round(frame_index*1e9/a.fps)
                            if (next_frame-first)/1e9>=a.seconds or a.output.suffix.lower()=='.png':
                                stats['complete']=True; return
                        xyz,rgb=decode(msg)
                        cloud.add(transform_points(T,xyz),None if a.color=='height' else rgb,a.cloud_mode=='replace')
                        renderer.update_cloud(cloud)
                    for value in tree.edges.values():
                        if isinstance(value,Series) and len(value.samples)>6000:
                            value.samples={t:value.samples[t] for t in sorted(value.samples)[-3000:]}; value.prepare()
        stats['complete']=output.count>0
    finally:
        stats['frames']=output.count; stats['pending_clouds']=len(pending)
        output.close(); renderer.close()
        a.output.with_suffix('.json').write_text(json.dumps(stats,indent=2),encoding='utf-8')
        print(json.dumps(stats,indent=2))
    if not output.count: raise ValueError('Bridge ended without synchronized frames')
