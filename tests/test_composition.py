from pathlib import Path

from PIL import Image
import numpy as np

from slam_fpv.composition import Composition, SpeedHistory, TextLayer, VideoLayer, compose, estimated_speed
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
    assert set(later) == {("video", 0), ("video", 1)}


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
