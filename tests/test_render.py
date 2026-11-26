import json
import subprocess
import sys
import imageio.v2 as imageio
import numpy as np
import vtk
from slam_fpv.renderer import Renderer, view_camera
from slam_fpv.cloud import CloudMap
from slam_fpv.sources import DemoSource


def test_world_path_crossing_camera_is_clipped_without_a_bridge():
    from slam_fpv.renderer import clip_near_plane

    points = np.array([[0., 0., 2.], [0., 0., -1.], [4., 0., -1.], [4., 0., 2.]])
    left, right = clip_near_plane(points, points[:, 2], .1)
    np.testing.assert_allclose(left, [[0, 0, 2], [0, 0, .1]])
    np.testing.assert_allclose(right, [[4, 0, .1], [4, 0, 2]])


def test_range_colors_follow_body_without_a_new_cloud():
    from vtk.util.numpy_support import vtk_to_numpy
    from slam_fpv.geometry import matrix

    cloud = CloudMap(0, range_color=True, range_limits=(0., 10.))
    cloud.add(np.array([[0., 0., 0.], [8., 0., 0.]]))
    renderer = Renderer(320, 180, edl=False, view_modes=("oblique",))
    try:
        renderer.update_cloud(cloud)
        renderer.frame(matrix([0., 0., 0.], [0., 0., 0., 1.]), (), "")
        meshes = [actor.GetMapper().GetInput() for _, actor in renderer.cloud_actors]
        first = vtk_to_numpy(meshes[0].GetPointData().GetScalars()).copy()
        renderer.frame(matrix([8., 0., 0.], [0., 0., 0., 1.]), (), "")
        for mesh in meshes:
            np.testing.assert_array_equal(vtk_to_numpy(mesh.GetPointData().GetScalars()), first[::-1])
    finally:
        renderer.close()


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
    position, target, up = view_camera(pose, "fpv_raw", rear, high, 0.)
    np.testing.assert_allclose((target-position)/8, pose[:3, 0])
    np.testing.assert_allclose(up, pose[:3, 2])
    assert not np.allclose(view_camera(pose,"orbit",rear,high,0.)[0],
                           view_camera(pose,"orbit",rear,high,2.)[0])


def test_stabilized_fpv_reduces_jitter_without_wrapping_the_turn():
    from slam_fpv.geometry import fpv_angles, matrix, stabilized_fpv_pose

    class Source:
        first, last = 0, 2_000_000_000

        @staticmethod
        def pose(stamp):
            t = stamp / 1e9
            yaw = np.deg2rad(178 + 6*t + 5*np.sin(2*np.pi*8*t))
            roll = np.deg2rad(15*np.cos(2*np.pi*8*t))
            return matrix([t, .04 * np.cos(2*np.pi*8*t), 1],
                          [np.sin(roll/2)*np.cos(yaw/2), np.sin(roll/2)*np.sin(yaw/2),
                           np.cos(roll/2)*np.sin(yaw/2), np.cos(roll/2)*np.cos(yaw/2)])

    source = Source()
    times = np.arange(350_000_000, 1_650_000_000, 16_666_667)
    raw = [source.pose(int(stamp)) for stamp in times]
    smooth = [stabilized_fpv_pose(source, int(stamp), frame) for stamp, frame in zip(times, raw)]
    assert abs(smooth[0][3, 3]-1) < 1e-9
    assert all(abs(frame[2, 1]) < 1e-9 for frame in smooth)
    baseline = np.deg2rad(178 + 6 * times / 1e9)
    raw_yaw = np.unwrap([fpv_angles(frame)[0] for frame in raw])
    smooth_yaw = np.unwrap([fpv_angles(frame)[0] for frame in smooth])
    assert np.std(smooth_yaw - baseline) < .40 * np.std(raw_yaw - baseline)
    assert smooth_yaw[-1] - smooth_yaw[0] > np.deg2rad(6)
    assert np.std([frame[1, 3] for frame in smooth]) < .40 * np.std([frame[1, 3] for frame in raw])


def test_fpv_hides_trajectory_and_indicator_shows_unsmoothed_attitude():
    from slam_fpv.geometry import matrix

    pose = matrix([0., 0., 1.], [0., 0., 0., 1.])
    rolled = matrix([0., 0., 1.], [.30, 0., 0., .95])
    trajectory = [[1., .1, 1.], [2., .4, 1.2], [3., .5, 1.]]
    renderer = Renderer(480, 270, edl=False, clean=True, view_modes=("fpv_raw",), main_mode="fpv")
    try:
        stable, raw = renderer.frame(pose, [], "", fpv_pose=pose)
        with_path, raw_with_path = renderer.frame(pose, trajectory, "", fpv_pose=pose)
        np.testing.assert_array_equal(stable, with_path)
        np.testing.assert_array_equal(np.asarray(raw["fpv_raw"]), np.asarray(raw_with_path["fpv_raw"]))
        measured, raw_rolled = renderer.frame(rolled, [], "", fpv_pose=pose)
        assert np.count_nonzero(stable != measured) > 20
        assert np.count_nonzero(np.asarray(raw["fpv_raw"]) != np.asarray(raw_rolled["fpv_raw"])) > 10
    finally:
        renderer.close()


def test_actual_and_planned_trajectory_use_independent_widths():
    from slam_fpv.renderer import SPATIAL_COLORS
    from slam_fpv.spatial import Geometry, SpatialFrame

    source = DemoSource(1)
    path = np.array([[.5, 0, 1.], [1., 0, 1.]])
    spatial = SpatialFrame((Geometry(lines=(path,)),), (), None, None)
    renderer = Renderer(320, 180, edl=False, slam_view=True, view_modes=(),
                        trajectory_width=2., plan_width=6.)
    try:
        renderer.frame(source.pose(source.first), path, "", spatial=spatial)
        widths = {actor.GetProperty().GetColor(): actor.GetProperty().GetLineWidth()
                  for _, actor in renderer.dynamic if isinstance(actor, vtk.vtkActor2D)}
        assert widths[renderer.trajectory_color] == 2.
        assert widths[SPATIAL_COLORS["plan"]] == 6.
    finally:
        renderer.close()


def test_per_view_filters_keep_scan_out_of_selected_fpvs():
    from slam_fpv.spatial import Geometry, SpatialFrame

    source = DemoSource(1)
    path = np.array([[.5, 0, 1.], [1., 0, 1.]])
    spatial = SpatialFrame((Geometry(lines=(path,)),), (), None, path)
    renderer = Renderer(320, 180, edl=False, slam_view=True, view_modes=("oblique", "fpv"),
                        overlays=(("scan", "actual"), ("plan",), ()))
    try:
        renderer.frame(source.pose(source.first), path, "", spatial=spatial)
        assert renderer.visible_overlays == ({"scan", "actual"}, {"plan"}, set())
        assert len(renderer.scan_actors) == 1
        assert renderer.scan_actors[0][0] == renderer.views[0][0]
        assert any(scene == renderer.views[1][1] for scene, _ in renderer.dynamic)
    finally:
        renderer.close()


def test_views_zoom_independently_and_goal_is_a_sized_disk():
    from slam_fpv.renderer import SPATIAL_COLORS, zoomed_fov
    from slam_fpv.spatial import Geometry, SpatialFrame

    source = DemoSource(1)
    pose = source.pose(source.first)
    base, focus, _ = view_camera(pose, "rear", (5., 3., 0.), (10., 7., 0.), 0.)
    near, target, _ = view_camera(pose, "rear", (5., 3., 0.), (10., 7., 0.), 0., zoom=2.)
    np.testing.assert_allclose(target, focus)
    np.testing.assert_allclose(near-focus, (base-focus)/2.)
    raw = view_camera(pose, "fpv_raw", (5., 3., 0.), (10., 7., 0.), 0., zoom=3.)[0]
    np.testing.assert_allclose(raw, pose[:3, 3] + pose[:3, 0]*.12)

    goal = Geometry(goals=(focus[None, :],))
    spatial = SpatialFrame((), (), None, None, (goal,))
    renderer = Renderer(320, 180, edl=False, slam_view=True, view_modes=("fpv",),
                        view_zooms=(1.25, 2.), axis_width=6, goal_size=30,
                        overlays=(("goal",), ()))
    try:
        renderer.frame(pose, (), "", spatial=spatial)
        main, fpv = (scene.GetActiveCamera() for scene, _ in renderer.views)
        assert main.GetViewAngle() == 65
        assert abs(fpv.GetViewAngle() - zoomed_fov(65, 2.)) < 1e-6
        actors = [actor for _, actor in renderer.dynamic if isinstance(actor, vtk.vtkActor2D)]
        disk, = [actor for actor in actors if actor.GetProperty().GetColor() == SPATIAL_COLORS["goal"]]
        mesh = disk.GetMapper().GetInput()
        assert mesh.GetNumberOfPolys() == 1 and mesh.GetNumberOfLines() == 0
        left, right, bottom, top, *_ = mesh.GetBounds()
        np.testing.assert_allclose((right-left, top-bottom), (30., 30.))
        assert any(actor.GetProperty().GetLineWidth() == 6 for actor in actors)
    finally:
        renderer.close()


def test_proxy_stream_delivers_ordered_previews_without_writing_a_file(tmp_path):
    from slam_fpv.cli import open_source, parser, render
    from slam_fpv.composition import Composition

    output = tmp_path / "proxy.png"
    args = parser().parse_args([
        "demo", "--output", str(output), "--seconds", ".6",
        "--fps", "10", "--width", "320", "--height", "180", "--slam-view",
    ])
    frames, events = [], []
    render(args, open_source(args), preview=lambda *frame: frames.append(frame),
           preview_stream=True, preview_offset=.2, event_data=events.append,
           project=Composition(views=()))
    assert [frame[2] for frame in frames] == [.2, .3, .4, .5, .6]
    assert events == [()]
    assert frames[0][0].shape == (180, 320, 4)
    assert not output.exists()
