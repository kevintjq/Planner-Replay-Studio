import imageio.v2 as imageio
import numpy as np

from planner_replay.trajectory_image import timed_ghost_indices, track_trajectory


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


def test_ghost_edits_reuse_cached_sprites_and_remove_starting_frame(tmp_path, monkeypatch):
    from planner_replay.trajectory_image import TrajectoryComposer, TrajectoryTrack

    path = tmp_path / "ghost-editor.mp4"
    with imageio.get_writer(path, fps=10, codec="libx264", macro_block_size=2) as writer:
        for x in (10, 35, 60, 85, 110):
            image = np.zeros((100, 160, 3), dtype=np.uint8)
            image[30:48, x:x+16] = (230, 200, 150)
            writer.append_data(image)
    original = np.zeros((100, 160, 3), dtype=np.uint8)
    original[30:48, 10:26] = (230, 200, 150)

    from PIL import Image

    samples = tuple((i/10, (x, 30, 16, 18)) for i, x in enumerate((10, 35, 60, 85, 110)))
    track = TrajectoryTrack(path, Image.fromarray(original), samples, (), (160, 100), None)
    composer = TrajectoryComposer(track)
    original_ghosts = composer.render(lambda: None, sample_interval=.1, opacity=1.)
    assert len(original_ghosts.frames) == 5
    assert np.asarray(original_ghosts.image)[35, 15].mean() > 160

    def unexpected_reader(_path):
        raise AssertionError("Cached edits must not decode or track the video again")

    monkeypatch.setattr("planner_replay.trajectory_image.VideoReader", unexpected_reader)
    updated = composer.render(lambda: None, sample_interval=.2, opacity=.5, excluded={0, 4})
    assert [index for index, _, _ in updated.frames] == [0, 2, 4]
    pixels = np.asarray(updated.image)
    assert pixels[35, 15].max() < 10 and pixels[35, 115].max() < 10
    assert 50 < pixels[35, 65].mean() < 145


def test_native_mask_matches_three_reference_median():
    from planner_replay._ghost_mask import rgb_difference

    rng = np.random.default_rng(42)
    current, a, b, c = rng.integers(0, 256, size=(4, 13, 11, 3), dtype=np.uint8)
    median = np.median([a, b, c], axis=0)
    expected = np.max(np.abs(current - median), axis=2).astype(np.uint8)
    assert rgb_difference(current, a, b, c) == expected.tobytes()
