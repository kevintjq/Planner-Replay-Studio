import argparse
from contextlib import ExitStack
import re
import json
import sys
from pathlib import Path
import numpy as np
import imageio.v2 as imageio
from .cloud import CloudMap
from .sources import BagSource, DemoSource, inspect_bag
from .encoding import h264_encoder
from .footage import FULL_FRAME, FlightVideo, VideoEdit, composite


def parser(parser_class=argparse.ArgumentParser):
    p=parser_class(description='SLAM-FPV dual view EDL renderer')
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
        x.add_argument('--color',choices=['rgb','height','solid'],default='rgb')
        x.add_argument('--cloud-color',type=hex_color,default='#70d9ff')
        x.add_argument('--trajectory-color',type=hex_color,default=None)
        x.add_argument('--trajectory-width',type=float,default=3)
        x.add_argument('--supersample',type=int,choices=(1,2),default=1)
        x.add_argument('--crf',type=int,choices=range(0,52),default=None,metavar='0–51')
        x.add_argument('--encoder',choices=('auto','cpu','nvenc'),default='cpu')
        if name != 'listen':
            x.add_argument('--video',type=Path)
            x.add_argument('--video-in',type=float,default=0)
            x.add_argument('--video-out',type=float)
            x.add_argument('--video-crop',type=float,nargs=4,default=FULL_FRAME,metavar=('X0','Y0','X1','Y1'))
            x.add_argument('--sync-anchor',type=float,nargs=2,action='append',default=[],
                           metavar=('BAG_SECONDS','VIDEO_SECONDS'))
            x.add_argument('--video-offset',type=float,default=0,help='Source time in seconds at the first video frame')
            x.add_argument('--video-width',type=float,default=.30,help='Video width as a fraction of the output frame')
            x.add_argument('--video-position',type=float,nargs=2,default=(.035,.87),metavar=('X','Y'),
                           help='Coordinates in the free space, from upper left (0 0) to lower right (1 1)')
        x.add_argument('--no-edl',action='store_true')
    return p


def hex_color(value):
    if not re.fullmatch(r'#[0-9a-fA-F]{6}', value):
        raise argparse.ArgumentTypeError('Enter a color as #RRGGBB')
    return value


def rgb_bytes(value):
    return tuple(bytes.fromhex(value[1:]))


def extrinsics(path):
    if path is None: return []
    data=json.loads(path.read_text(encoding='utf-8'))
    return [(v['parent'],v['child'],v['translation'],v['quaternion_xyzw']) for v in data]


def make_renderer(a):
    from .renderer import Renderer
    solid = rgb_bytes(a.cloud_color) if a.color == 'solid' else None
    trajectory = ((1,.73,.18) if a.trajectory_color is None
                  else tuple(c / 255 for c in rgb_bytes(a.trajectory_color)))
    return Renderer(a.width,a.height,not a.no_edl,a.axis,a.axis_pixels,a.point_size,
                    a.distance,a.camera_height,a.side,a.fov,a.inset_distance,a.inset_height,
                    supersample=a.supersample,cloud_color=solid,trajectory_color=trajectory,
                    trajectory_width=a.trajectory_width)


class Export:
    def __init__(self,a,path=None):
        self.path = a.output if path is None else path
        self.count = 0
        self.path.parent.mkdir(parents=True,exist_ok=True)
        self.writer = self.codec = None
        if self.path.suffix.lower() == '.mp4':
            self.codec = h264_encoder(a.encoder, a.width, a.height, a.fps)
            options = {}
            if self.codec == "h264_nvenc":
                options = dict(quality=None, output_params=[
                    "-preset", "p5", "-rc", "vbr", "-cq", str(a.crf if a.crf is not None else 18),
                    "-b:v", "0", "-movflags", "+faststart"])
            elif a.crf is not None:
                options = dict(quality=None, output_params=[
                    "-crf", str(a.crf), "-preset", "medium", "-movflags", "+faststart"])
            self.writer = imageio.get_writer(str(self.path), fps=a.fps, codec=self.codec,
                                             pixelformat="yuv420p", macro_block_size=2, **options)

    def append(self,frame):
        if self.writer: self.writer.append_data(frame)
        else: imageio.imwrite(str(self.path),frame)
        self.count+=1

    def close(self):
        if self.writer: self.writer.close()


def render(a, source, *, preview=None, preview_offset=0, progress=None, check_cancel=None, staging=None):
    """Render an export, or deliver one uncomposited preview and its optional footage."""
    destination = a.output if staging is None else staging
    cloud = CloudMap(a.voxel, a.max_points)
    trajectory = []
    dropped = transformed = count = 0
    dirty = False
    failure = codec = None
    is_synthetic = isinstance(source, DemoSource)
    try:
        with ExitStack() as stack:
            video = stack.enter_context(FlightVideo(a.video, video_edit(a)))
            renderer = make_renderer(a)
            stack.callback(renderer.close)
            output = None
            if preview is None:
                output = Export(a, destination)
                codec = output.codec
                stack.callback(output.close)
            iterator = iter(source.clouds())
            stack.callback(iterator.close)

            start = source.first + round(a.start * 1e9)
            end = min(source.last, start + round(a.seconds * 1e9))
            if start > end:
                raise ValueError('--start beyond data')
            frame_count = int((end - start) * a.fps / 1e9) + 1
            preview_index = min(frame_count - 1, round(preview_offset * a.fps))
            pending = next(iterator, None)
            for i in range(frame_count):
                if check_cancel is not None:
                    check_cancel()
                t = start + round(i * 1e9 / a.fps)
                while pending is not None and pending[0] <= t:
                    _, xyz, rgb = pending
                    cloud.add(xyz, None if a.color == 'height' else rgb, a.cloud_mode == 'replace')
                    transformed += 1
                    dirty = True
                    if check_cancel is not None:
                        check_cancel()
                    pending = next(iterator, None)
                try:
                    pose = source.pose(t)
                except LookupError:
                    dropped += 1
                    if trajectory:
                        raise ValueError('Pose unavailable inside export interval; shorten/split interval') from None
                    continue
                if not len(cloud.xyz):
                    continue
                trajectory.append(pose[:3, 3].copy())
                if preview is not None and i < preview_index:
                    continue
                if dirty:
                    renderer.update_cloud(cloud)
                    dirty = False
                elapsed = (t - source.first) / 1e9
                label = 'SYNTHETIC DEMO' if is_synthetic else 'RECORDED ROS1 DATA'
                frame = renderer.frame(pose, trajectory, f'{label} | t={elapsed:.3f}s | REAR CHASE + OBLIQUE')
                footage = video.frame_at(elapsed)
                if preview is not None:
                    preview(frame, footage, elapsed)
                    return
                output.append(composite(frame, footage, a.video_width, a.video_position))
                count = output.count
                if progress is not None:
                    progress(i + 1, frame_count)
                if a.output.suffix.lower() == '.png':
                    break
            if not count:
                raise ValueError('No synchronized frames rendered')
    except Exception as error:
        failure = str(error)
        raise
    finally:
        if preview is None:
            report = dict(source='synthetic' if is_synthetic else 'recorded_ros1',
                          frames=count,leading_pose_skips=dropped,clouds=transformed,
                          leading_cloud_skips=getattr(source,'leading_cloud_skips',0),
                          retained_points=len(cloud.xyz),fps=a.fps,edl_requested=not a.no_edl,
                          complete=failure is None and count > 0,error=failure,encoder=codec,
                          config={k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items() if k != 'bag'})
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.with_suffix('.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
            if progress is None:
                print(json.dumps(report,indent=2))


def open_source(a, check_cancel=None):
    if a.command == 'demo':
        return DemoSource(a.seconds)
    return BagSource(a.bag,a.cloud_topic,a.odom_topic,a.world,a.body,a.pose_source,
                     a.max_gap,extrinsics(a.extrinsics)).index(check_cancel)


def video_edit(a):
    return VideoEdit(a.video_offset, a.video_in, a.video_out, tuple(a.video_crop), tuple(a.sync_anchor))


def validate(a):
    positive=['fps','seconds','width','height','distance','inset_distance','inset_height',
              'axis','axis_pixels','point_size','voxel','max_points','trajectory_width']
    if any(not np.isfinite(getattr(a,k)) or getattr(a,k)<=0 for k in positive):
        raise ValueError('Sizes, distances, rates and duration must be finite and positive')
    if min(a.width, a.height) < 8 or a.width%2 or a.height%2: raise ValueError('Use even output dimensions of at least 8 pixels')
    if not 1<a.fov<170: raise ValueError('FOV must be between 1 and 170 degrees')
    if not np.isfinite([a.side,a.camera_height,a.start]).all() or a.start<0:
        raise ValueError('Invalid camera/start parameter')
    if a.output.suffix.lower() not in ('.png','.mp4'): raise ValueError('Output must be .png or .mp4')
    if a.command in ('bag','listen') and (not np.isfinite(a.max_gap) or a.max_gap<=0):
        raise ValueError('--max-gap must be positive')

    if a.command != 'listen':
        video_edit(a)
        if not .05 <= a.video_width <= .90:
            raise ValueError('Invalid video time offset or size (5–90%)')
        if not all(0 <= coordinate <= 1 for coordinate in a.video_position):
            raise ValueError('Video position must be between 0 and 1')
    inputs = (getattr(a, 'bag', None), getattr(a, 'video', None), getattr(a, 'extrinsics', None))
    outputs = {a.output.resolve(), a.output.with_suffix('.json').resolve()}
    if any(path is not None and path.resolve() in outputs for path in inputs):
        raise ValueError('Choose an output path different from the input files')


def main():
    a=parser().parse_args()
    try:
        if a.command=='inspect':
            print(json.dumps(inspect_bag(a.bag),indent=2))
            return
        validate(a)
        if a.command=='listen':
            from .live import listen
            listen(a); return
        render(a,open_source(a))
    except (ValueError,LookupError,OSError,RuntimeError) as e:
        print(f'ERROR: {e}',file=sys.stderr); sys.exit(2)


if __name__=='__main__': main()
