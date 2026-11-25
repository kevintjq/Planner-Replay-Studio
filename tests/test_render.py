import json
import subprocess
import sys
import imageio.v2 as imageio
import numpy as np
import vtk
from slam_fpv.renderer import Renderer, view_camera
from slam_fpv.cloud import CloudMap
from slam_fpv.sources import DemoSource


def test_edl_two_view_frame_and_rgb_axes():
    s=DemoSource(1); _,xyz,rgb=next(s.clouds())
    m=CloudMap(); m.add(xyz,rgb)
    r=Renderer(640,360)
    try:
        r.update_cloud(m)
        assert len(r.views)==2
        assert all(scene.GetPass().IsA('vtkEDLShading') for scene,_ in r.views)
        image=r.frame(s.pose(s.first),[[0,0,1],[.1,0,1]],'SYNTHETIC TEST')
        assert image.shape==(360,640,3) and image.std()>10
        inset=image[15:130,425:625]
        assert inset.std()>10
        for channel in range(3):
            other=np.delete(image,channel,axis=2).max(axis=2)
            assert ((image[:,:,channel]>170)&(image[:,:,channel].astype(int)-other>50)).sum()>8
    finally: r.close()


def test_video_export_frame_count_size_and_motion(tmp_path):
    output=tmp_path/'demo.mp4'
    subprocess.run([sys.executable,'-m','slam_fpv.cli','demo','--output',str(output),
                    '--seconds','1','--fps','8','--width','640','--height','360'],check=True,capture_output=True)
    r=imageio.get_reader(output)
    try:
        assert r.count_frames()==9
        assert r.get_meta_data()['fps']==8
        first,last=np.asarray(r.get_data(0)),np.asarray(r.get_data(8))
        assert first.shape==(360,640,3)
        assert np.abs(first.astype(float)-last).mean()>2
    finally: r.close()
    report=json.loads(output.with_suffix('.json').read_text())
    assert report['source']=='synthetic' and report['complete'] and report['frames']==9


def test_slam_outline_keeps_tf_and_transparent_inset_background():
    from slam_fpv.geometry import matrix

    r = Renderer(640, 360, edl=False, slam_view=True, clean=True)
    try:
        pose = matrix([0, 0, 0], [0.12, .1, .3, .94])
        frame = r.frame(pose, [], "")
        assert frame.shape == (360, 640, 4)
        # Inspect blank regions within each view and compare projected TF axes.
        assert frame[0, 0, 3] == frame[21, 431, 3] == 0
        for x0, y0, x1, y1 in ((0, 0, 415, 360), (423, 14, 630, 134)):
            region = frame[y0:y1, x0:x1]
            pixels = region[region[:, :, 3] == 255, :3].astype(int)
            for channel in range(3):
                others = np.delete(pixels, channel, axis=1).max(axis=1)
                assert np.any(pixels[:, channel] > others + 60)
    finally:
        r.close()


def test_configurable_point_cloud_cameras_are_separate_and_unlabelled():
    cloud = CloudMap()
    source = DemoSource(1)
    _, xyz, rgb = next(source.clouds())
    cloud.add(xyz, rgb)
    renderer = Renderer(480, 270, edl=False, slam_view=True, clean=True,
                        view_modes=("left", "right"))
    try:
        renderer.update_cloud(cloud)
        main, views = renderer.frame(source.pose(source.first), [], "")
        assert main.shape == (270, 480, 4)
        assert tuple(views) == ("left", "right")
        assert all(view.mode == "RGBA" for view in views.values())
        assert np.count_nonzero(np.asarray(views["left"])[:, :, 3]) > 0
        assert not np.array_equal(np.asarray(views["left"]), np.asarray(views["right"]))
        for _, overlay in renderer.views:
            props = overlay.GetViewProps()
            for index in range(props.GetNumberOfItems()):
                actor = props.GetItemAsObject(index)
                if isinstance(actor, (vtk.vtkBillboardTextActor3D, vtk.vtkTextActor)):
                    assert actor.GetInput() != "TF"
    finally:
        renderer.close()



def test_fpv_camera_tracks_body_pitch_and_roll_and_orbit_moves():
    from slam_fpv.geometry import matrix

    pose = matrix([3., 4., 5.], [0.12, .16, .3, .93])
    rear, high = (4., 2., 0.), (7., 4., 0.)
    position, target, up = view_camera(pose, "fpv", rear, high, 0.)
    np.testing.assert_allclose((target-position)/8, pose[:3, 0])
    np.testing.assert_allclose(up, pose[:3, 2])
    assert not np.allclose(view_camera(pose,"orbit",rear,high,0.)[0],
                           view_camera(pose,"orbit",rear,high,2.)[0])
