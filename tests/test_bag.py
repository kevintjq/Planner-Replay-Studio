"""Generated ROS1 bags are synthetic fixtures, not measured data."""
import numpy as np
import pytest
from rosbags.rosbag1 import Writer
from rosbags.typesys import Stores,get_typestore,get_types_from_msg
from slam_fpv.sources import BagSource


@pytest.fixture
def bag(tmp_path):
    ts=get_typestore(Stores.ROS1_NOETIC)
    ts.register(get_types_from_msg('geometry_msgs/TransformStamped[] transforms\n','tf2_msgs/msg/TFMessage'))
    types=ts.types
    def make(name,*args): return types[name](*args)
    def header(i,frame): return make('std_msgs/msg/Header',i,make('builtin_interfaces/msg/Time',1,i*100000000),frame)
    path=tmp_path/'synthetic.bag'
    with Writer(path) as writer:
        cs={topic:writer.add_connection(topic,typ,typestore=ts) for topic,typ in
            [('/points','sensor_msgs/msg/PointCloud2'),('/odom','nav_msgs/msg/Odometry'),
             ('/tf','tf2_msgs/msg/TFMessage'),('/tf_static','tf2_msgs/msg/TFMessage'),
             ('/plan','nav_msgs/msg/Path'),('/corridor','visualization_msgs/msg/MarkerArray'),
             ('/scan','sensor_msgs/msg/PointCloud2')]}
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
            messages=[('/odom',odom),('/tf',tf),('/points',cloud)]
            if i == 1:
                second_pose = make('geometry_msgs/msg/Pose', make('geometry_msgs/msg/Point', 3., 0., 0.), q)
                plan = make('nav_msgs/msg/Path', header(i,'body'), [
                    make('geometry_msgs/msg/PoseStamped', header(i,'body'), item)
                    for item in (pose, second_pose)])
                messages.extend([('/plan', plan), ('/scan', cloud)])
            if i in (1, 2):
                corridor = make('visualization_msgs/msg/Marker', header(i,'map'), 'corridor', 1,
                                1, 0 if i == 1 else 2, pose,
                                make('geometry_msgs/msg/Vector3', 1., 2., 1.),
                                make('std_msgs/msg/ColorRGBA', .3, .7, 1., .25),
                                make('builtin_interfaces/msg/Duration', 0, 0), False, [], [], '', '', False)
                messages.append(('/corridor', make('visualization_msgs/msg/MarkerArray', [corridor])))
            if i==0:
                transform=make('geometry_msgs/msg/Transform',make('geometry_msgs/msg/Vector3',.5,0.,0.),q)
                messages.insert(0,('/tf_static',make('tf2_msgs/msg/TFMessage',[
                    make('geometry_msgs/msg/TransformStamped',header(i,'body'),'lidar',transform)])))
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


def test_missing_cloud_and_frames_rejected(bag):
    with pytest.raises(ValueError): BagSource(bag,'/missing','/odom','map','body').index()
    with pytest.raises(ValueError): BagSource(bag,'/points','/odom','map','wrong_body').index()


def test_export_synthetic_bag_cli(bag,tmp_path):
    import subprocess,sys,json
    output=tmp_path/'bag.png'
    subprocess.run([sys.executable,'-m','slam_fpv.cli','bag',str(bag),'--cloud-topic','/points',
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
