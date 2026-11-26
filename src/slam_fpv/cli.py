import argparse
from contextlib import ExitStack
from dataclasses import asdict
import re
import json
import sys
from pathlib import Path
import numpy as np
import imageio.v2 as imageio
from .cloud import CloudMap
from .geometry import stabilized_fpv_pose
from .composition import SpeedHistory, compose, estimated_speed
from .sources import BagSource, DemoSource, TrailDemoSource, inspect_bag
from .spatial import SpatialOverlays, camera_settings
from .sensor_topics import suggested_topic
from .encoding import h264_encoder
from .footage import FULL_FRAME, FlightVideo, VideoEdit, composite
from .export_media import mux_audio


def parser(parser_class=argparse.ArgumentParser):
    p=parser_class(description='SLAM-FPV dual view EDL renderer')
    sub=p.add_subparsers(dest='command',required=True)
    inspect=sub.add_parser('inspect',help='List bag topics, types, counts and storage time')
    inspect.add_argument('bag',type=Path)
    for name in ('demo','bag','listen'):
        x=sub.add_parser(name)
        if name=='bag': x.add_argument('bag',type=Path)
        if name=='demo': x.add_argument('--demo-scene',choices=('room','trail'),default='room')
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
        x.add_argument('--axis-width',type=float,default=5,help='TF axis line width in pixels')
        x.add_argument('--goal-size',type=float,default=24,help='Goal dot diameter in pixels')
        x.add_argument('--point-size',type=float,default=2)
        x.add_argument('--voxel',type=float,default=.08)
        x.add_argument('--max-points',type=int,default=200000)
        x.add_argument('--cloud-mode',choices=['accumulate','replace'],default='accumulate')
        x.add_argument('--color',choices=['rgb','height','scan','solid'],default='rgb')
        x.add_argument('--height-range',type=float,nargs=2,metavar=('MIN_Z','MAX_Z'))
        x.add_argument('--slam-view',action='store_true',help='Unlit LiDAR points and foreground trajectory')
        x.add_argument('--cloud-color',type=hex_color,default='#70d9ff')
        x.add_argument('--trajectory-color',type=hex_color,default=None)
        x.add_argument('--trajectory-width',type=float,default=5,help='Actual flight path width in pixels')
        if name != 'listen':
            x.add_argument('--plan-width',type=float,default=6,help='Planned trajectory width in pixels')
        x.add_argument('--supersample',type=int,choices=(1,2),default=1)
        x.add_argument('--crf',type=int,choices=range(0,52),default=None,metavar='0–51')
        x.add_argument('--encoder',choices=('auto','cpu','nvenc'),default='cpu')
        if name == 'bag':
            x.add_argument('--plan-topic', help='Planned Path, SUPER Marker[Array], or EGO Bspline')
            x.add_argument('--corridor-topic', help='Safety corridor Marker or MarkerArray')
            x.add_argument('--plan-frame', default='world', help='Frame used by EGO Bspline messages without a header')
            x.add_argument('--goal-topic', help='SUPER goal MarkerArray, PoseStamped or PointStamped')
            x.add_argument('--scan-topic', help='Separate PointCloud2, LaserScan or Livox CustomMsg topic for the latest scan')
            x.add_argument('--camera-info-topic', help='Override the automatically selected CameraInfo topic')
            x.add_argument('--lidar-fov-topic', dest='scan_topic', help=argparse.SUPPRESS)
        if name == 'demo':
            x.add_argument('--show-plan', action='store_true')
            x.add_argument('--show-corridor', action='store_true')
            x.add_argument('--show-goal', action='store_true')
        if name != 'listen':
            x.add_argument('--show-scan', action='store_true', help='Highlight the latest scan')
            x.add_argument('--show-camera-fov', action='store_true', help='Show the FOV detected from CameraInfo')
            x.add_argument('--show-lidar-fov', dest='show_scan', action='store_true', help=argparse.SUPPRESS)
            x.add_argument('--scan-max-age', type=float, default=.5, help='Hide scans older than this many seconds')
            x.add_argument('--camera-fov', nargs=4, metavar=('FRAME','HFOV','VFOV','DEPTH'))
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


def make_renderer(a, *, clean=False, view_modes=None, main_mode="rear", view_widths=None, overlays=None, view_zooms=None):
    from .renderer import Renderer
    solid = rgb_bytes(a.cloud_color) if a.color == 'solid' else None
    trajectory = ((1,.73,.18) if a.trajectory_color is None
                  else tuple(c / 255 for c in rgb_bytes(a.trajectory_color)))
    return Renderer(a.width,a.height,not a.no_edl and not a.slam_view,a.axis,a.axis_pixels,a.point_size,
                    a.distance,a.camera_height,a.side,a.fov,a.inset_distance,a.inset_height,
                    supersample=a.supersample,cloud_color=solid,trajectory_color=trajectory,
                    trajectory_width=a.trajectory_width, plan_width=getattr(a, 'plan_width', 6.),
                    axis_width=a.axis_width, goal_size=a.goal_size, clean=clean,slam_view=a.slam_view,view_modes=view_modes,
                    main_mode=main_mode,view_widths=view_widths,overlays=overlays,view_zooms=view_zooms)


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


def render(a, source, *, preview=None, preview_offset=0, preview_data=None, event_data=None, media_data=None, progress=None, check_cancel=None, staging=None, project=None):
    """Render an export, or deliver one uncomposited preview and its optional footage."""
    destination = a.output if staging is None else staging
    cloud = CloudMap(a.voxel, a.max_points, scan_height=a.color == "scan", height_limits=a.height_range)
    trajectory = []
    speed_history = SpeedHistory()
    dropped = transformed = count = 0
    dirty = False
    failure = codec = first_elapsed = None
    video_export = preview is None and a.output.suffix.lower() == ".mp4"
    playback = project.play_rate if video_export and project else 1.
    audio = next((layer for layer in project.videos if layer.id == project.audio_id), None) if video_export and project else None
    is_synthetic = isinstance(source, DemoSource)
    try:
        with ExitStack() as stack:
            video = stack.enter_context(FlightVideo(a.video, video_edit(a)))
            layers = [stack.enter_context(FlightVideo(layer.path, layer.edit)) for layer in project.videos] if project else ()
            view_modes = None if project is None else tuple(view.kind for view in project.views)
            renderer = make_renderer(a, clean=project is not None, view_modes=view_modes,
                                     main_mode="rear" if project is None else project.main_view.kind,
                                     view_widths=None if project is None else tuple(view.width for view in project.views),
                                     overlays=None if project is None else tuple(
                                         view.overlays for view in (project.main_view, *project.views)),
                                     view_zooms=None if project is None else tuple(
                                         view.zoom for view in (project.main_view, *project.views)))
            stack.callback(renderer.close)
            output = None
            if preview is None:
                output = Export(a, destination)
                codec = output.codec
                stack.callback(output.close)
            spatial = SpatialOverlays(a, source, stack, compare_plans=bool(project and project.compare_plans))
            iterator = iter(source.clouds())
            stack.callback(iterator.close)

            start = source.first + round(a.start * 1e9)
            end = min(source.last, start + round(a.seconds * 1e9))
            if start > end:
                raise ValueError('--start beyond data')
            frame_count = int((end - start) * a.fps / (1e9 * playback)) + 1
            preview_index = min(frame_count - 1, round(preview_offset * a.fps))
            pending = next(iterator, None)
            for i in range(frame_count):
                if check_cancel is not None:
                    check_cancel()
                t = min(end, start + round(i * 1e9 * playback / a.fps))
                while pending is not None and pending[0] <= t:
                    cloud_stamp, xyz, rgb = pending
                    spatial.update_map_scan(cloud_stamp, xyz)
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
                elapsed = (t - source.first) / 1e9
                speed = (speed_history.update(elapsed, estimated_speed(source, t, pose))
                         if project is not None and (preview is not None or project.show_speed) else None)
                if preview is not None and i < preview_index:
                    continue
                if dirty:
                    renderer.update_cloud(cloud)
                    dirty = False
                label = 'SYNTHETIC DEMO' if is_synthetic else 'RECORDED ROS1 DATA'
                camera_pose = stabilized_fpv_pose(source, t, pose) if "fpv" in renderer.modes else None
                spatial_frame = spatial.frame(t, pose, check_cancel) if spatial else None
                frame = renderer.frame(pose, trajectory, f'{label} | t={elapsed:.3f}s | REAR CHASE + OBLIQUE',
                                       elapsed=elapsed, fpv_pose=camera_pose, spatial=spatial_frame)
                if preview is not None:
                    if preview_data is not None:
                        preview_data((source.last - source.first) / 1e9, spatial.status(t, spatial_frame))
                    if event_data is not None:
                        event_data(() if is_synthetic else source.timeline_events())
                if first_elapsed is None:
                    first_elapsed = elapsed
                if project is not None:
                    if preview is not None and media_data is not None:
                        media_data(tuple(layer.reader.duration for layer in layers))
                    frame, cloud_views = frame
                    footage = tuple(layer.frame_at(elapsed) for layer in layers)
                    if preview is not None:
                        preview(frame, footage, elapsed, speed, cloud_views)
                        return
                    combined, _ = compose(frame, project, footage, elapsed, speed, cloud_views,
                                          alpha=project.transparent_canvas and a.output.suffix.lower() == ".png")
                    output.append(np.asarray(combined))
                else:
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
        if audio is not None:
            mux_audio(destination, audio, first_elapsed, count/a.fps, playback, check_cancel)
    except Exception as error:
        failure = str(error)
        raise
    finally:
        if preview is None:
            report = dict(source='synthetic' if is_synthetic else 'recorded_ros1',
                          frames=count,leading_pose_skips=dropped,clouds=transformed,
                          leading_cloud_skips=getattr(source,'leading_cloud_skips',0),
                          retained_points=len(cloud.xyz),fps=a.fps,edl_requested=not a.no_edl and not a.slam_view,
                          complete=failure is None and count > 0,error=failure,encoder=codec,
                          config={k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items() if k != 'bag'})
            if project is not None:
                report['composition'] = asdict(project)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.with_suffix('.json').write_text(json.dumps(report,indent=2,default=str),encoding='utf-8')
            if progress is None:
                print(json.dumps(report,indent=2))


def open_source(a, check_cancel=None, metadata=None):
    if a.command == 'demo':
        return (TrailDemoSource if a.demo_scene == "trail" else DemoSource)(a.start + a.seconds)
    auto_camera = a.show_camera_fov and camera_settings(a) is None
    auto_scan = a.show_scan and not a.scan_topic
    if metadata is None and (auto_camera or auto_scan):
        metadata = inspect_bag(a.bag, check_cancel)
    scan_topic = a.scan_topic
    if auto_scan:
        scan_topic = suggested_topic(metadata["topics"], "scan", a.world.lstrip("/"), a.body.lstrip("/"))
    source = BagSource(a.bag, a.cloud_topic, a.odom_topic, a.world, a.body, a.pose_source,
                       a.max_gap, extrinsics(a.extrinsics), plan_topic=a.plan_topic,
                       corridor_topic=a.corridor_topic, scan_topic=scan_topic, goal_topic=a.goal_topic, plan_frame=a.plan_frame).index(check_cancel)
    if auto_camera:
        source.detect_camera(a.camera_info_topic, metadata)
    return source


def video_edit(a):
    return VideoEdit(a.video_offset, a.video_in, a.video_out, tuple(a.video_crop), tuple(a.sync_anchor))


def validate(a):
    positive=['fps','seconds','width','height','distance','inset_distance','inset_height',
              'axis','axis_pixels','point_size','max_points','trajectory_width','axis_width','goal_size']
    if any(not np.isfinite(getattr(a,k)) or getattr(a,k)<=0 for k in positive):
        raise ValueError('Sizes, distances, rates and duration must be finite and positive')
    if not np.isfinite(a.voxel) or a.voxel < 0:
        raise ValueError('Voxel size must be nonnegative; use 0 for raw sampling')
    if a.height_range is not None and (not np.isfinite(a.height_range).all() or a.height_range[0] >= a.height_range[1]):
        raise ValueError('Height range must contain increasing, finite values')
    if min(a.width, a.height) < 8 or a.width%2 or a.height%2: raise ValueError('Use even output dimensions of at least 8 pixels')
    if not 1<a.fov<170: raise ValueError('FOV must be between 1 and 170 degrees')
    if not np.isfinite([a.side,a.camera_height,a.start]).all() or a.start<0:
        raise ValueError('Invalid camera/start parameter')
    if a.output.suffix.lower() not in ('.png','.mp4'): raise ValueError('Output must be .png or .mp4')
    if a.command in ('bag','listen') and (not np.isfinite(a.max_gap) or a.max_gap<=0):
        raise ValueError('--max-gap must be positive')

    if a.command != 'listen':
        camera_settings(a)
        if not np.isfinite(a.plan_width) or a.plan_width <= 0:
            raise ValueError("规划轨迹线宽须大于 0。")
        if not np.isfinite(a.scan_max_age) or a.scan_max_age <= 0:
            raise ValueError('扫描保留时间须是大于 0 的有限值。')
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
            print(json.dumps(inspect_bag(a.bag), indent=2, default=asdict))
            return
        validate(a)
        if a.command=='listen':
            from .live import listen
            listen(a); return
        render(a,open_source(a))
    except (ValueError,LookupError,OSError,RuntimeError) as e:
        print(f'ERROR: {e}',file=sys.stderr); sys.exit(2)


if __name__=='__main__': main()
