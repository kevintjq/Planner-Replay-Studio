import argparse
import json
import sys
from pathlib import Path
import numpy as np
import imageio.v2 as imageio
from .cloud import CloudMap
from .sources import BagSource, DemoSource


def parser():
    p=argparse.ArgumentParser(description='SLAM-FPV dual view EDL renderer')
    sub=p.add_subparsers(dest='command',required=True)
    inspect=sub.add_parser('inspect',help='List bag topics, types, counts and storage time')
    inspect.add_argument('bag',type=Path)
    for name in ('demo','bag','listen'):
        x=sub.add_parser(name)
        if name=='bag': x.add_argument('bag',type=Path)
        if name=='listen':
            x.add_argument('--port',type=int,default=8765)
            x.add_argument('--delay',type=float,default=.3,help='TF lookahead buffering seconds')
        if name in ('bag','listen'):
            x.add_argument('--cloud-topic',default='/cloud_registered')
            x.add_argument('--odom-topic',default='/Odometry')
            x.add_argument('--world',default='camera_init')
            x.add_argument('--body',default='body')
            x.add_argument('--pose-source',choices=['odom','tf'],default='odom')
            x.add_argument('--max-gap',type=float,default=.5)
            x.add_argument('--extrinsics',type=Path,help='JSON list of parent,child,translation,quaternion_xyzw')
        x.add_argument('--output',type=Path,required=True,help='.mp4 or .png')
        x.add_argument('--fps',type=int,default=24)
        x.add_argument('--seconds',type=float,default=6)
        x.add_argument('--start',type=float,default=0,help='Seconds after first cloud header (bag mode)')
        x.add_argument('--width',type=int,default=1280)
        x.add_argument('--height',type=int,default=720)
        x.add_argument('--distance',type=float,default=3.8)
        x.add_argument('--camera-height',type=float,default=1.35)
        x.add_argument('--side',type=float,default=0)
        x.add_argument('--fov',type=float,default=65)
        x.add_argument('--inset-distance',type=float,default=10)
        x.add_argument('--inset-height',type=float,default=6.7)
        x.add_argument('--axis',type=float,default=.48)
        x.add_argument('--axis-pixels',type=float,default=28)
        x.add_argument('--point-size',type=float,default=2)
        x.add_argument('--voxel',type=float,default=.08)
        x.add_argument('--max-points',type=int,default=200000)
        x.add_argument('--cloud-mode',choices=['accumulate','replace'],default='accumulate')
        x.add_argument('--color',choices=['rgb','height'],default='rgb')
        x.add_argument('--no-edl',action='store_true')
    return p


def extrinsics(path):
    if path is None: return []
    data=json.loads(path.read_text(encoding='utf-8'))
    return [(v['parent'],v['child'],v['translation'],v['quaternion_xyzw']) for v in data]


def make_renderer(a):
    from .renderer import Renderer
    return Renderer(a.width,a.height,not a.no_edl,a.axis,a.axis_pixels,a.point_size,
                    a.distance,a.camera_height,a.side,a.fov,a.inset_distance,a.inset_height)


class Export:
    def __init__(self,a):
        self.a=a; self.count=0
        a.output.parent.mkdir(parents=True,exist_ok=True)
        self.writer=None if a.output.suffix=='.png' else imageio.get_writer(
            str(a.output),fps=a.fps,codec='libx264',pixelformat='yuv420p',macro_block_size=2)

    def append(self,frame):
        if self.writer: self.writer.append_data(frame)
        else: imageio.imwrite(str(self.a.output),frame)
        self.count+=1

    def close(self):
        if self.writer: self.writer.close()


def render(a,source):
    renderer=make_renderer(a); output=Export(a)
    cloud=CloudMap(a.voxel,a.max_points)
    trajectory=[]; dropped=0; transformed=0; failure=None; dirty=False
    iterator=iter(source.clouds())
    def advance():
        nonlocal dropped
        while True:
            try: return next(iterator)
            except LookupError as e:
                # A missing cloud transform aborts generator; explicit failure is safer than a silent identity.
                raise ValueError(str(e)) from e
            except StopIteration: return None
    try:
        pending=advance()
        start=source.first+round(a.start*1e9)
        end=min(source.last,start+round(a.seconds*1e9))
        if start>end: raise ValueError('--start beyond data')
        for i in range(int((end-start)*a.fps/1e9)+1):
            t=start+round(i*1e9/a.fps)
            while pending is not None and pending[0]<=t:
                _,xyz,rgb=pending
                cloud.add(xyz,None if a.color=='height' else rgb,a.cloud_mode=='replace')
                transformed+=1; dirty=True
                pending=advance()
            try: pose=source.pose(t)
            except LookupError:
                dropped+=1
                # Do not compress time across an internal tracking dropout.
                if output.count: raise ValueError('Pose unavailable inside export interval; shorten/split interval')
                continue
            if not len(cloud.xyz): continue
            if dirty:
                renderer.update_cloud(cloud); dirty=False
            trajectory.append(pose[:3,3].copy())
            label=('SYNTHETIC DEMO' if isinstance(source,DemoSource) else 'RECORDED ROS1 DATA')
            label+=f' | t={(t-source.first)/1e9:.3f}s | REAR CHASE + OBLIQUE'
            output.append(renderer.frame(pose,trajectory,label))
            if a.output.suffix=='.png': break
    except Exception as e:
        failure=str(e)
        raise
    finally:
        iterator.close()
        output.close(); renderer.close()
        report=dict(source='synthetic' if isinstance(source,DemoSource) else 'recorded_ros1',
                    frames=output.count,leading_pose_skips=dropped,clouds=transformed,
                    leading_cloud_skips=getattr(source,'leading_cloud_skips',0),
                    retained_points=len(cloud.xyz),fps=a.fps,edl_requested=not a.no_edl,
                    complete=failure is None and output.count>0,error=failure,
                    config={k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items() if k not in ('bag',)})
        a.output.with_suffix('.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(json.dumps(report,indent=2))
    if not output.count: raise ValueError('No synchronized frames rendered')


def validate(a):
    positive=['fps','seconds','width','height','distance','inset_distance','inset_height',
              'axis','axis_pixels','point_size','voxel','max_points']
    if any(not np.isfinite(getattr(a,k)) or getattr(a,k)<=0 for k in positive):
        raise ValueError('Sizes, distances, rates and duration must be finite and positive')
    if a.width%2 or a.height%2: raise ValueError('Use even output dimensions')
    if not 1<a.fov<170: raise ValueError('FOV must be between 1 and 170 degrees')
    if not np.isfinite([a.side,a.camera_height,a.start]).all() or a.start<0:
        raise ValueError('Invalid camera/start parameter')
    if a.output.suffix not in ('.png','.mp4'): raise ValueError('Output must be .png or .mp4')
    if a.command in ('bag','listen') and (not np.isfinite(a.max_gap) or a.max_gap<=0):
        raise ValueError('--max-gap must be positive')


def main():
    a=parser().parse_args()
    try:
        if a.command=='inspect':
            from rosbags.highlevel import AnyReader
            with AnyReader([a.bag]) as r:
                print(json.dumps(dict(storage_start_ns=r.start_time,storage_end_ns=r.end_time,
                    topics=[dict(topic=c.topic,type=c.msgtype,count=c.msgcount) for c in r.connections]),indent=2))
            return
        validate(a)
        if a.command=='listen':
            from .live import listen
            listen(a); return
        source=DemoSource(a.seconds) if a.command=='demo' else BagSource(
            a.bag,a.cloud_topic,a.odom_topic,a.world,a.body,a.pose_source,a.max_gap,extrinsics(a.extrinsics)).index()
        render(a,source)
    except (ValueError,LookupError,OSError,RuntimeError) as e:
        print(f'ERROR: {e}',file=sys.stderr); sys.exit(2)


if __name__=='__main__': main()
