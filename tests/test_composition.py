from dataclasses import replace
from pathlib import Path

from PIL import Image
import numpy as np

from slam_fpv.composition import (
    Composition, LayoutCue, PointCloudView, SpeedHistory, TextLayer, VideoLayer, compose, estimated_speed,
)
from slam_fpv.sources import DemoSource


def test_layer_blend_bounds_and_intervals():
    base = Image.new("RGB", (640, 360), (10, 20, 30))
    red = Image.new("RGB", (160, 90), (210, 20, 30))
    green = Image.new("RGB", (160, 90), (10, 220, 30))
    project = Composition(
        videos=(VideoLayer(Path("front.mp4"), position=(0, 1), opacity=.5),
                VideoLayer(Path("side.mp4"), position=(1, 1), opacity=1)),
        texts=(TextLayer("Outdoor segment", 1., 2., position=(.1, 0)),),
        show_speed=True, speed_position=(.1, .4), speed_start=1., speed_end=2.,
    )
    image, bounds = compose(base, project, (red, green), 1.5, (2., 2.5, 1.5))
    for key, rgb in ((("video", 0), (110, 20, 30)), (("video", 1), (10, 220, 30))):
        x, y, w, h = bounds[key]
        assert np.max(np.abs(np.array(image.getpixel((x + w // 2, y + h // 2))) - rgb)) <= 1
        assert np.max(np.abs(np.array(image.getpixel((x, y))) - base.getpixel((x, y)))) <= 2
    assert ("text", 0) in bounds and ("speed", 0) in bounds
    _, later = compose(base, project, (red, green), 2., (2., 2.5, 1.5))
    assert set(later) == {("main", 0), ("video", 0), ("video", 1)}


def test_speed_uses_timestamps_and_treats_tracking_gaps_as_missing():
    source = DemoSource(1)
    stamp = source.first + 300_000_000
    valid = estimated_speed(source, stamp, source.pose(stamp))
    assert .8 < valid < 1.7
    stats = SpeedHistory()
    assert stats.update(0., 1.) == (1., 1., 1.)
    assert stats.update(2., 3.) == (3., 3., 2.)
    stats.update(3., None)
    assert stats.update(10., 7.) == (7., 7., 2.)


def test_video_backdrop_keeps_foreground_and_draws_insets_above_it():
    scene = Image.new("RGBA", (200, 100), (10, 15, 20, 0))
    scene.putpixel((100, 50), (4, 6, 8, 255))
    footage = Image.new("RGB", (80, 120), (55, 90, 130))
    inset = Image.new("RGB", (160, 90), (240, 70, 50))
    project = Composition(videos=(
        VideoLayer(Path("inset.mp4"), width=.25, position=(1, 1), opacity=1),
        VideoLayer(Path("fpv.mp4"), backdrop=True, opacity=1),
    ))
    image, bounds = compose(scene, project, (inset, footage), 1., None)
    assert image.getpixel((0, 0)) == (55, 90, 130)
    assert image.getpixel((100, 50)) == (4, 6, 8)
    assert image.getpixel((175, 85)) == (240, 70, 50)
    assert set(bounds) == {("main", 0), ("video", 0), ("video", 1)}
    gap, _ = compose(scene, project, (None, None), 5., None)
    assert gap.getpixel((0, 0)) == (10, 15, 20)



def test_main_and_view_plate_geometry():
    scene = Image.new("RGBA", (320, 180), (10, 15, 20, 0))
    scene.putpixel((160, 90), (80, 210, 130, 255))
    inset = Image.new("RGBA", (160, 90), (0, 0, 0, 0))
    project = Composition(main_view=PointCloudView("fpv", .6, (0., 0.)),
                          views=(PointCloudView("right", .30, (1., 1.), False),))
    image, bounds = compose(scene, project, (), 1., None, {"right": inset})
    assert bounds["main", 0] == (0, 0, 192, 108)
    x, y, w, h = bounds["view", 0]
    assert image.getpixel((x+w//2, y+h//2)) == (16, 22, 32)
    transparent = replace(project, views=(replace(project.views[0], transparent=True),))
    image, _ = compose(scene, transparent, (), 1., None, {"right": inset})
    assert image.getpixel((x+w//2, y+h//2)) == (10, 15, 20)


def test_transparent_canvas_keeps_real_alpha_and_can_flatten_for_mp4():
    scene = Image.new("RGBA", (200, 100), (10, 15, 20, 0))
    scene.putpixel((100, 50), (60, 90, 130, 120))
    project = Composition(views=(), transparent_canvas=True)
    alpha, _ = compose(scene, project, (), 1., None)
    assert alpha.mode == "RGBA"
    assert alpha.getpixel((0, 0))[3] == 0
    assert alpha.getpixel((100, 50)) == (60, 90, 130, 120)
    flattened, _ = compose(scene, project, (), 1., None, alpha=False)
    assert flattened.mode == "RGB"
    assert flattened.getpixel((0, 0)) == (10, 15, 20)


def test_layout_cues_switch_at_source_time_and_retain_video_identity():
    base = Image.new("RGB", (160, 90), (40, 75, 120))
    first, second = VideoLayer(Path("left.mp4")), VideoLayer(Path("right.mp4"))
    red = Image.new("RGB", (160, 90), (210, 30, 40))
    green = Image.new("RGB", (160, 90), (30, 210, 40))
    project = Composition(videos=(second, first), views=(),
                          layouts=(LayoutCue(1., "video", first.id), LayoutCue(2., "cloud"), LayoutCue(3., "mix")))
    full, bounds = compose(base, project, (green, red), 1., None)
    assert full.getpixel((0, 0)) == (210, 30, 40) and not bounds
    _, cloud = compose(base, project, (green, red), 2., None)
    assert set(cloud) == {("main", 0)}
    _, mix = compose(base, project, (green, red), 3., None)
    assert set(mix) == {("main", 0), ("video", 0), ("video", 1)}


def test_one_clip_shrinks_from_behind_the_cloud_to_its_inset():
    scene = Image.new("RGBA", (320, 180), (20, 25, 30, 0))
    scene.putpixel((160, 90), (30, 210, 60, 255))
    footage = Image.new("RGB", (160, 90), (210, 45, 55))
    clip = VideoLayer(Path("flight.mp4"), width=.25, position=(1., 1.), opacity=1.)
    project = Composition(videos=(clip,), views=(), audio_id=clip.id,
                          layouts=(LayoutCue(1., "backdrop", clip.id), LayoutCue(5., "inset", clip.id)))
    for alpha in (False, True):
        source, initial = compose(scene, project, (footage,), 4.99, None, alpha=alpha)
        start, start_bounds = compose(scene, project, (footage,), 5., None, alpha=alpha)
        assert initial == start_bounds
        assert start.tobytes() == source.tobytes()
        assert source.getpixel((0, 0))[:3] == (210, 45, 55)
        assert source.getpixel((160, 90))[:3] == (30, 210, 60)
        widths = [compose(scene, project, (footage,), t, None, alpha=alpha)[1]["video", 0][2]
                  for t in (5., 5.1, 5.2, 5.3, 5.4)]
        np.testing.assert_allclose(widths, [320, 282.5, 200, 117.5, 80], atol=.5)
        finish, final = compose(scene, project, (footage,), 5.4, None, alpha=alpha)
        assert final["video", 0] == (240, 135, 80, 45)
        assert finish.getpixel((280, 155))[:3] == (210, 45, 55)
        assert finish.getpixel((160, 90))[:3] == (30, 210, 60)
        assert ("video", 0) not in compose(scene, project, (None,), 5.2, None, alpha=alpha)[1]


def test_inset_marker_sets_its_own_size_position_and_duration():
    scene = Image.new("RGBA", (320, 180))
    footage = Image.new("RGB", (160, 90), (210, 45, 55))
    clip = VideoLayer(Path("flight.mp4"), width=.25, position=(1., 1.))
    project = Composition(videos=(clip,), views=(), layouts=(
        LayoutCue(0., "backdrop", clip.id),
        LayoutCue(1., "inset", clip.id, transition=1., width=.5, position=(0., 0.)),
        LayoutCue(3., "inset", clip.id, transition=0., width=.25, position=(1., 1.)),
    ))
    for stamp, expected in ((1., (0, 0, 320, 180)), (1.5, (0, 0, 240, 135)),
                            (2., (0, 0, 160, 90)), (3., (240, 135, 80, 45))):
        assert compose(scene, project, (footage,), stamp, None)[1]["video", 0] == expected


def test_all_visible_windows_shrink_independently_with_eased_geometry():
    from slam_fpv.composition import LayerMotion

    scene = Image.new("RGBA", (640, 360))
    cloud = Image.new("RGBA", (320, 180), (15, 70, 80, 150))
    footage = Image.new("RGB", (160, 90), (210, 45, 55))
    video = VideoLayer(Path("flight.mp4"), position=(1., 1.))
    view = PointCloudView("fpv", .3, (1., 0.))
    caption = TextLayer("SLAM", 0., 3., position=(0., 0.))
    base = Composition(videos=(video,), views=(view,), texts=(caption,), show_speed=True,
                       main_view=PointCloudView("rear", .46, (.2, .8)), transparent_canvas=True)
    targets = (("main", None, .83), ("view", view.id, .75), ("video", video.id, .71),
               ("text", caption.id, .43), ("speed", None, .54))
    motions = tuple(LayerMotion(kind, key, .1, .5, 1., size, (0., 0.)) for kind, key, size in targets)
    composition = replace(base, motions=motions)
    snapshots = [
        compose(scene, composition, (footage,), t, None, {"fpv": cloud})
        for t in (0., .3, .5, .75, 1., 1.25, 1.5)
    ]
    regular, hold, start, first_step, half, last_step, end = [bounds for _, bounds in snapshots]
    assert regular == end
    assert hold == start
    for key in regular:
        widths = [bounds[key][2] for bounds in (start, first_step, half, last_step, end)]
        assert widths == sorted(widths, reverse=True) and widths[0] > widths[-1]
        assert widths[0]-widths[1] < widths[1]-widths[2]
        assert widths[3]-widths[4] < widths[2]-widths[3]
        assert hold[key][:2] == (0, 0)
    assert all(image.mode == "RGBA" for image, _ in snapshots)
