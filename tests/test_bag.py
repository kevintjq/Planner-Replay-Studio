"""Generated ROS1 bags are synthetic fixtures, not measured data."""
import numpy as np
import pytest
from rosbags.rosbag1 import Writer
from rosbags.typesys import Stores,get_typestore,get_types_from_msg
from planner_replay.sources import BagSource


@pytest.fixture
def bag(tmp_path):
    ts=get_typestore(Stores.ROS1_NOETIC)
    ts.register(get_types_from_msg('geometry_msgs/TransformStamped[] transforms\n','tf2_msgs/msg/TFMessage'))
    ts.register(get_types_from_msg(
        'int32 order\nint64 traj_id\ntime start_time\nfloat64[] knots\n'
        'geometry_msgs/Point[] pos_pts\nfloat64[] yaw_pts\nfloat64 yaw_dt\n', 'ego_planner/msg/Bspline'))
    types=ts.types
    def make(name,*args): return types[name](*args)
    def header(i,frame): return make('std_msgs/msg/Header',i,make('builtin_interfaces/msg/Time',1,i*100000000),frame)
    path=tmp_path/'synthetic.bag'
    with Writer(path) as writer:
        cs={topic:writer.add_connection(topic,typ,typestore=ts) for topic,typ in
            [('/points','sensor_msgs/msg/PointCloud2'),('/odom','nav_msgs/msg/Odometry'),
             ('/vehicle/velocity','geometry_msgs/msg/TwistStamped'),
             ('/drone/grid_map/occupancy','sensor_msgs/msg/PointCloud2'),
             ('/tf','tf2_msgs/msg/TFMessage'),('/tf_static','tf2_msgs/msg/TFMessage'),
             ('/plan','nav_msgs/msg/Path'),('/corridor','visualization_msgs/msg/MarkerArray'),
             ('/scan','sensor_msgs/msg/PointCloud2'), ('/camera/info','sensor_msgs/msg/CameraInfo'),
             ('/laser','sensor_msgs/msg/LaserScan'),
             ('/super/visualization/exp_traj','visualization_msgs/msg/MarkerArray'),
             ('/super/visualization/goal','visualization_msgs/msg/MarkerArray'),
             ('/goal','geometry_msgs/msg/PoseStamped'), ('/planning/bspline','ego_planner/msg/Bspline'),
             ('/ego_planner_node/goal_point','visualization_msgs/msg/Marker')]}
        for i in range(4):
            p=make('geometry_msgs/msg/Point',float(i),0.,0.)
            q=make('geometry_msgs/msg/Quaternion',0.,0.,0.,1.)
            pose=make('geometry_msgs/msg/Pose',p,q)
            v=make('geometry_msgs/msg/Vector3',0.,0.,0.)
            odom=make('nav_msgs/msg/Odometry',header(i,'map'),'body',
                      make('geometry_msgs/msg/PoseWithCovariance',pose,np.zeros(36)),
                      make('geometry_msgs/msg/TwistWithCovariance',make('geometry_msgs/msg/Twist',v,v),np.zeros(36)))
            transform=make('geometry_msgs/msg/Transform',make('geometry_msgs/msg/Vector3',float(i),0.,0.),q)
            tf=make('tf2_msgs/msg/TFMessage',[make('geometry_msgs/msg/TransformStamped',header(i,'map'),'body',transform)])
            fields=[make('sensor_msgs/msg/PointField',name,j*4,7,1) for j,name in enumerate(['x','y','z'])]
            cloud=make('sensor_msgs/msg/PointCloud2',header(i,'lidar'),1,1,fields,False,12,12,
                       np.array([1,0,0],dtype='<f4').view('u1'),True)
            map_header = make('std_msgs/msg/Header', i, make('builtin_interfaces/msg/Time',0,0), 'map')
            map_cloud = make('sensor_msgs/msg/PointCloud2',map_header,1,1,fields,False,12,12,cloud.data,True)
            writer.write(cs['/drone/grid_map/occupancy'], 1_000_000_000+i*100000000,
                         ts.serialize_ros1(map_cloud,map_cloud.__msgtype__))
            messages=[('/odom',odom),('/tf',tf),('/points',cloud)]
            if i in (1, 2):
                linear = make('geometry_msgs/msg/Vector3', 3.*i, 4.*i, 0.)
                twist = make('geometry_msgs/msg/Twist', linear, v)
                messages.append(('/vehicle/velocity', make('geometry_msgs/msg/TwistStamped', header(i, 'map'), twist)))
            if i == 1:
                second_pose = make('geometry_msgs/msg/Pose', make('geometry_msgs/msg/Point', 3., 0., 0.), q)
                plan = make('nav_msgs/msg/Path', header(i,'body'), [
                    make('geometry_msgs/msg/PoseStamped', header(i,'body'), item)
                    for item in (pose, second_pose)])
                messages.extend([('/plan', plan), ('/scan', cloud)])
            if i in (1, 2):
                def native_marker(namespace, kind, ident, action=0, points=(), marker_pose=pose):
                    zero = make('builtin_interfaces/msg/Time', 0, 0)
                    marker_header = make('std_msgs/msg/Header', 0, zero, '') if action else header(i, 'map')
                    return make('visualization_msgs/msg/Marker', marker_header, namespace, ident, kind, action,
                                marker_pose, make('geometry_msgs/msg/Vector3', .3, .3, .3),
                                make('std_msgs/msg/ColorRGBA', .12, .52, .9, 1.),
                                make('builtin_interfaces/msg/Duration', 0, 0), False, list(points), [], '', '', False)
                endpoint = make('geometry_msgs/msg/Point', 5.+i, -1., 2.)
                target_pose = make('geometry_msgs/msg/Pose', endpoint, q)
                clear = native_marker("", 0, 0, action=3)
                # SUPER ships zero-stamped DELETEALL separately before the new MarkerArray.
                for name, markers in (
                    ('/super/visualization/exp_traj', [native_marker('exp_traj', 0, i, points=(v, endpoint))]),
                    ('/super/visualization/goal', [native_marker('goal', 2, i*3, marker_pose=target_pose),
                                                  native_marker('goal', 2, i*3+1),
                                                  native_marker('goal_line', 5, i, points=(endpoint, p))]),
                ):
                    messages.extend((name, make('visualization_msgs/msg/MarkerArray', batch))
                                    for batch in ([clear], markers))
                messages.append(('/goal', make('geometry_msgs/msg/PoseStamped', header(i, 'map'), target_pose)))
                if i == 1:
                    controls = [make('geometry_msgs/msg/Point', *xyz) for xyz in
                                [(0.,0.,0.), (1.,2.,0.), (2.,2.,0.), (3.,0.,0.)]]
                    spline = make('ego_planner/msg/Bspline', 3, 42, header(i,'map').stamp,
                                  np.array([0.,0.,0.,0.,2.,2.,2.,2.]), controls, np.empty(0), 0.)
                    messages.extend([('/planning/bspline', spline),
                                     ('/ego_planner_node/goal_point', native_marker('', 2, 0, marker_pose=target_pose))])
                corridor = make('visualization_msgs/msg/Marker', header(i,'map'), 'corridor', 1,
                                1, 0 if i == 1 else 2, pose,
                                make('geometry_msgs/msg/Vector3', 1., 2., 1.),
                                make('std_msgs/msg/ColorRGBA', .3, .7, 1., .25),
                                make('builtin_interfaces/msg/Duration', 0, 0), False, [], [], '', '', False)
                messages.append(('/corridor', make('visualization_msgs/msg/MarkerArray', [corridor])))
            if i==0:
                camera = make('sensor_msgs/msg/CameraInfo', header(i,'camera_optical'), 480, 640, 'plumb_bob',
                              np.zeros(5), np.array([400.,0.,280.,0.,400.,240.,0.,0.,1.]),
                              np.eye(3).ravel(), np.zeros(12), 0, 0,
                              make('sensor_msgs/msg/RegionOfInterest', 0, 0, 0, 0, False))
                laser = make('sensor_msgs/msg/LaserScan', header(i,'lidar'), -1., 2., 1., 0., .1, .1, 10.,
                             np.array([1., np.inf, 2., 20.], dtype=np.float32), np.empty(0, dtype=np.float32))
                messages.extend([('/camera/info',camera), ('/laser',laser)])
                transform=make('geometry_msgs/msg/Transform',make('geometry_msgs/msg/Vector3',.5,0.,0.),q)
                messages.insert(0,('/tf_static',make('tf2_msgs/msg/TFMessage',[
                    make('geometry_msgs/msg/TransformStamped',header(i,'body'),'lidar',transform),
                    make('geometry_msgs/msg/TransformStamped',header(i,'body'),'camera_optical',transform)])))
            for topic,msg in messages:
                # Storage time deliberately unrelated to sensor header time.
                writer.write(cs[topic],9_000_000_000+i*100000000,ts.serialize_ros1(msg,msg.__msgtype__))
    return path


@pytest.mark.parametrize('pose_source',['odom','tf'])
def test_direct_bag_tf_static_and_header_sync(bag,pose_source):
    s=BagSource(bag,'/points','/odom','map','body',pose_source).index()
    assert s.first==1_000_000_000 and s.count==4
    clouds=list(s.clouds())
    np.testing.assert_allclose(clouds[2][1],[[3.5,0,0]])
    assert s.pose(1_150_000_000)[0,3]==pytest.approx(1.5)
    with pytest.raises(LookupError): s.pose(999_000_000)


def test_speed_topic_uses_recorded_linear_velocity_even_when_pose_comes_from_tf(bag):
    from planner_replay.cli import open_source, parser, render
    from planner_replay.composition import Composition

    args = parser().parse_args(["bag", str(bag), "--output", "preview.png", "--cloud-topic", "/points",
                               "--odom-topic", "/odom", "--world", "map", "--pose-source", "tf"])
    automatic = open_source(args)
    assert automatic.speed_topic == "/odom"
    assert automatic.flight_speed(1_150_000_000) == 0.
    args.speed_topic = "/vehicle/velocity"
    source = open_source(args)
    assert source.flight_speed(source.first) is None
    assert source.flight_speed(1_100_000_000) == 5.
    assert source.flight_speed(1_150_000_000) == 7.5
    assert source.flight_speed(source.last) is None
    args.width, args.height, args.fps, args.seconds = 320, 180, 20, .3
    readings = []
    render(args, source, preview=lambda _frame, _videos, _elapsed, speed, _views: readings.append(speed),
           preview_offset=.15, project=Composition(views=(), show_speed=True))
    assert readings == [(7.5, 7.5, 6.25)]


def test_native_ego_map_with_zero_header_stamp_replays_on_bag_clock(bag):
    source = BagSource(bag, '/drone/grid_map/occupancy', '/odom', 'map', 'body').index()
    assert source.first == 1_000_000_000 and source.last == 1_300_000_000
    frames = list(source.clouds())
    assert len(frames) == 4 and frames[1][0] == 1_100_000_000
    np.testing.assert_allclose(frames[1][1], [[1., 0., 0.]])
    np.testing.assert_allclose(source.pose(frames[1][0])[:3, 3], [1., 0., 0.])


def test_overlay_diagnostics_distinguish_first_message_missing_tf_and_removed_corridor(bag):
    from contextlib import ExitStack
    from planner_replay.cli import parser
    from planner_replay.spatial import SpatialOverlays

    plan = "/planning/bspline"
    args = parser().parse_args(["bag", str(bag), "--output", "preview.png", "--plan-topic", plan,
                               "--corridor-topic", "/corridor"])
    source = BagSource(bag, "/points", "/odom", "map", "body", plan_topic=plan,
                       plan_frame="unconnected", corridor_topic="/corridor").index()
    with ExitStack() as stack:
        layers = SpatialOverlays(args, source, stack)

        def diagnostics_at(t):
            return layers.diagnostics(t, layers.frame(t, source.pose(t)))

        assert diagnostics_at(source.first)["plan"].label == "等待首帧"
        active = diagnostics_at(source.first + 100_000_000)
        assert active["plan"].label == "缺少 TF"
        assert "unconnected → map" in active["plan"].detail
        assert active["corridor"].label == "已读取"
        assert diagnostics_at(source.last)["corridor"].label == "当前无数据"


def test_missing_cloud_and_frames_rejected(bag):
    with pytest.raises(ValueError): BagSource(bag,'/missing','/odom','map','body').index()
    with pytest.raises(ValueError): BagSource(bag,'/points','/odom','map','wrong_body').index()


def test_export_synthetic_bag_cli(bag,tmp_path):
    import subprocess,sys,json
    output=tmp_path/'bag.png'
    subprocess.run([sys.executable,'-m','planner_replay.cli','bag',str(bag),'--cloud-topic','/points',
                    '--odom-topic','/odom','--world','map','--body','body','--output',str(output),
                    '--width','640','--height','360'],check=True,capture_output=True)
    report=json.loads(output.with_suffix('.json').read_text())
    # Input format labels recorded_ros1; fixture provenance remains synthetic.
    assert report['frames']==1 and report['complete']


def test_mission_topics_obey_headers_tf_deletes_and_separate_scans(bag):
    source = BagSource(bag, '/points', '/odom', 'map', 'body',
                       plan_topic='/plan', corridor_topic='/corridor', scan_topic='/scan').index()
    start, plan_time = source.first, source.first + 100_000_000
    assert source.mission_shapes('plan', start) == ()
    assert source.mission_shapes('corridor', start) == ()
    plan, = source.mission_shapes('plan', plan_time)
    np.testing.assert_allclose(plan.lines[0], [[2, 0, 0], [4, 0, 0]])
    corridor, = source.mission_shapes('corridor', plan_time)
    assert corridor.triangles[0].shape == (12, 3, 3)
    assert source.mission_shapes('corridor', plan_time + 100_000_000) == ()
    assert source.mission_shapes('corridor', plan_time)
    (time, scan), = list(source.scans())
    assert time == plan_time
    np.testing.assert_allclose(scan, [[2.5, 0, 0]])
    with pytest.raises(ValueError, match='/missing'):
        BagSource(bag, '/points', '/odom', 'map', 'body', scan_topic='/missing').index()


def test_detect_sensor_topics_and_frames(bag):
    from planner_replay.cli import open_source, parser
    from planner_replay.sources import inspect_bag
    from planner_replay.sensor_topics import pose_frames
    from planner_replay.spatial import camera_edges

    metadata = inspect_bag(bag)
    assert pose_frames(metadata["topics"], metadata["tf_pairs"], "/odom", "/points") == ("map", "body")
    args = parser().parse_args(["bag", str(bag), "--cloud-topic", "/points", "--odom-topic", "/odom",
                               "--world", "map", "--body", "body", "--output", "preview.png",
                               "--show-camera-fov", "--scan-topic", "/laser"])
    source = open_source(args, metadata=metadata)
    camera = source.detected_camera
    assert camera.frame == "camera_optical"
    assert camera.angles[0] == pytest.approx(np.degrees(np.arctan(360/400)+np.arctan(280/400)))
    assert camera_edges(camera)[:, :, 0].max() == pytest.approx(.765)
    (stamp, points), = list(source.scans())
    assert stamp == source.first
    assert len(points) == 2 and np.allclose(points[:, 2], 0)
    np.testing.assert_allclose(source.sensor_pose(camera, stamp)[:3, 3], [.5, 0, 0])


def test_super_trajectory_color_and_native_goal_replace_per_replan(bag):
    from planner_replay.sensor_topics import suggested_topic
    from planner_replay.sources import inspect_bag

    topics = inspect_bag(bag)["topics"]
    plan = suggested_topic(topics, "plan")
    goal = suggested_topic(topics, "goal")
    assert plan == "/super/visualization/exp_traj"
    assert goal == "/super/visualization/goal"
    source = BagSource(bag, '/points', '/odom', 'map', 'body', plan_topic=plan, goal_topic=goal).index()
    assert not source.mission_shapes("goal", source.first)
    assert source.timeline_events() == ((.1, "目标更新"), (.1, "首次规划"), (.2, "目标更新"), (.2, "重规划"))
    for i in (1, 2):
        at = source.first + i*100_000_000
        line, = source.mission_shapes("plan", at)
        target, = source.mission_shapes("goal", at)
        np.testing.assert_allclose(line.segments[0], [[[float(i), 0., 0.], [5.+2*i, -1., 2.]]])
        assert line.color == pytest.approx((.12, .52, .9))
        np.testing.assert_allclose(target.goals[0], [[5.+i, -1., 2.]])
    direct = BagSource(bag, '/points', '/odom', 'map', 'body', goal_topic='/goal').index()
    target, = direct.mission_shapes("goal", source.last)
    np.testing.assert_allclose(target.goals[0], [[7., -1., 2.]])


def test_ego_bspline_uses_start_time_and_samples_curve_in_chosen_frame(bag):
    from planner_replay.sensor_topics import suggested_topic
    from planner_replay.sources import inspect_bag

    topics = [row for row in inspect_bag(bag)["topics"] if not row["topic"].startswith("/super/")]
    plan = suggested_topic(topics, "plan")
    goal = suggested_topic(topics, "goal")
    assert plan == "/planning/bspline"
    assert goal == "/ego_planner_node/goal_point"
    source = BagSource(bag, '/points', '/odom', 'map', 'body', plan_topic=plan,
                       goal_topic=goal, plan_frame="body").index()
    assert not source.mission_shapes("plan", source.first)
    curve, = source.mission_shapes("plan", source.last)
    np.testing.assert_allclose(curve.lines[0][[0, -1]], [[1.,0.,0.], [4.,0.,0.]])
    assert abs(curve.lines[0][:, 1].max() - 1.5) < .005
    target, = source.mission_shapes("goal", source.last)
    np.testing.assert_allclose(target.goals[0], [[6., -1., 2.]])
