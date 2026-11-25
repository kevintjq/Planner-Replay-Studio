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
