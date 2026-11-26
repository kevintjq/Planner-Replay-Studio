import imageio.v2 as imageio
import numpy as np

from slam_fpv.trajectory_image import track_trajectory


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
