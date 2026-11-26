import imageio.v2 as imageio
import numpy as np

from slam_fpv.cli import open_source, parser, render, validate
from slam_fpv.footage import composite, video_rect


def test_video_rect_keeps_aspect_and_clamps_tall_footage():
    assert video_rect((640, 360), (400, 100), .5, (1, 1)) == (320, 280, 320, 80)
    assert video_rect((640, 360), (100, 400), .9, (0, 0)) == (0, 0, 81, 324)


def test_preview_and_export_apply_same_video_time_and_position(tmp_path):
    # Three distinct frames reveal synchronization errors at both clip boundaries.
    clip = tmp_path / "flight.mp4"
    with imageio.get_writer(clip, fps=4, codec="libx264", macro_block_size=2) as writer:
        for rgb in [(250, 16, 16), (16, 248, 16), (16, 16, 246)]:
            writer.append_data(np.full((32, 64, 3), rgb, dtype=np.uint8))

    output = tmp_path / "render.mp4"
    args = parser().parse_args([
        "demo", "--output", str(output), "--seconds", "1", "--fps", "4",
        "--width", "480", "--height", "270", "--crf", "15", "--supersample", "2",
        "--color", "solid", "--cloud-color", "#77deff", "--trajectory-color", "#e250f4",
        "--video", str(clip), "--video-offset", ".25", "--video-position", "0", "1",
        "--video-width", ".25",
    ])
    validate(args)
    progress = []
    render(args, open_source(args), progress=lambda *value: progress.append(value))
    assert progress[-1] == (5, 5)

    snapshots = []
    render(args, open_source(args), preview=lambda *value: snapshots.append(value), preview_offset=.5)
    frame, footage, elapsed = snapshots[0]
    assert elapsed == .5 and frame.shape == (270, 480, 3)
    expected = composite(frame, footage, args.video_width, args.video_position)
    with imageio.get_reader(output) as reader:
        assert reader.count_frames() == 5
        for index in (0, 4):
            assert (reader.get_data(index)[235:260, :60, 1] < 180).mean() > .8
        exported = reader.get_data(2)
        assert exported[245, 30, 1] > 220
        assert np.abs(exported.astype(float) - expected).mean() < 7


def test_pts_crop_trim_and_two_anchor_drift(tmp_path):
    from fractions import Fraction

    import av

    from slam_fpv.footage import FlightVideo, VideoEdit, VideoReader

    clip = tmp_path / "variable-rate.mp4"
    with av.open(str(clip), mode="w") as output:
        stream = output.add_stream("libx264", rate=60)
        stream.width, stream.height, stream.pix_fmt = 64, 32, "yuv420p"
        stream.codec_context.time_base = Fraction(1, 60)
        for pts, color in [(0, (210, 10, 40)), (3, (10, 210, 40)), (24, (10, 40, 210))]:
            pixels = np.empty((32, 64, 3), dtype=np.uint8)
            pixels[:, :32] = (245, 245, 245)
            pixels[:, 32:] = color
            frame = av.VideoFrame.from_ndarray(pixels, format="rgb24")
            frame.pts, frame.time_base = pts, Fraction(1, 60)
            output.mux(stream.encode(frame))
        output.mux(stream.encode())

    edit = VideoEdit(trim_in=.05, trim_out=.38, crop=(.5, 0, 1, 1),
                     anchors=((1., .05), (2., .25)))
    assert abs(edit.rate - .2) < 1e-9
    with FlightVideo(clip, edit) as footage:
        assert footage.frame_at(.99) is None
        image = footage.frame_at(2.)
        assert image.shape == (32, 32, 3)
        assert image[16, 16, 1] > 190
        assert footage.frame_at(2.7) is None
    with VideoReader(clip) as reader:
        assert abs(reader.time(reader.select(.4)) - .4) < 1e-9
        assert reader.time(reader.select(.01)) == 0


def test_video_edit_inverse_mapping():
    import pytest
    from slam_fpv.footage import VideoEdit

    for edit in (VideoEdit(offset=.2), VideoEdit(anchors=((1., .2), (11., 10.5)))):
        for time in (0., 3., 7.5):
            assert edit.bag_time(edit.video_time(time)) == pytest.approx(time)


def test_cut_segments_keep_source_timestamps_and_skip_removed_media(tmp_path):
    from slam_fpv.footage import FlightVideo, VideoEdit

    clip = tmp_path / "cut.mp4"
    with imageio.get_writer(clip, fps=4, codec="libx264", macro_block_size=2) as writer:
        for rgb in [(250, 16, 16), (16, 248, 16), (16, 16, 246)]:
            writer.append_data(np.full((32, 64, 3), rgb, dtype=np.uint8))
    edit = VideoEdit(anchors=((2., 0.), (3., 1.)), segments=((0., .25), (.5, None)))
    with FlightVideo(clip, edit) as footage:
        assert footage.frame_at(2.)[0, 0, 0] > 200
        assert footage.frame_at(2.25) is None
        assert footage.frame_at(2.49) is None
        assert footage.frame_at(2.5)[0, 0, 2] > 200
