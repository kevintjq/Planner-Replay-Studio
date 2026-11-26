import imageio.v2 as imageio
import numpy as np

from slam_fpv.trajectory_image import timed_ghost_indices, track_trajectory


def test_timed_ghost_sampling_uses_video_timestamps_and_skips_missing_frames():
    # Variable-frame-rate timestamps with a gap in the middle.
    samples = [(t, None) for t in (2.0, 2.09, 2.25, 2.6, 3.04, 3.05, 3.51)]
    assert timed_ghost_indices(samples, .25) == [0, 2, 3, 4, 6]
    assert timed_ghost_indices(samples, .5) == [0, 3, 4, 6]
    assert timed_ghost_indices(samples, 2.) == [0]


def test_fixed_camera_tracking_uses_selected_start_and_end(tmp_path):
    path = tmp_path / "stationary-camera.mp4"
    tile = np.random.default_rng(4).integers(80, 245, size=(20, 26, 3), dtype=np.uint8)
    with imageio.get_writer(path, fps=12, codec="libx264", macro_block_size=2) as writer:
        for i in range(23):
            image = np.zeros((160, 240, 3), dtype=np.uint8)
            image[68:88, 32+i*3:58+i*3] = tile
            writer.append_data(image)
    result = track_trajectory(path, .25, 1.25, (41/240, 68/160, 67/240, 88/160),
                              lambda: None, lambda *_: None)
    assert len(result.points) == 13
    assert result.image.size == (240, 160) and result.lost_at is None
    assert result.points[0] == (54., 78.)
    assert 83 < result.points[-1][0] < 96
    assert abs(result.points[-1][1]-78) < 5
    assert result.start == .25 and result.end == 1.25
    rendered = np.asarray(result.image)
    assert rendered[74:82, 84:92].mean() > 25
    assert rendered[:48].max() < 20 and rendered[105:].max() < 20
