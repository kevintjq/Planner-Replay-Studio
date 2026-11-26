from contextlib import ExitStack

import numpy as np
import pytest

from slam_fpv.cli import parser
from slam_fpv.sources import DemoSource
from slam_fpv.spatial import CameraView, SpatialOverlays, camera_edges


def test_camera_geometry_and_scan_expiry():
    camera = CameraView("optical", (90, 90), 3.)
    corners = camera_edges(camera).reshape(-1, 3)
    np.testing.assert_allclose(corners.max(axis=0), [3, 3, 3])
    np.testing.assert_allclose(corners.min(axis=0), [-3, -3, 0])
    with pytest.raises(ValueError):
        CameraView("optical", (180, 90))

    args = parser().parse_args(["demo", "--output", "preview.png", "--show-scan",
                               "--scan-max-age", ".2", "--show-plan", "--show-corridor"])
    source = DemoSource(3)
    with ExitStack() as stack:
        spatial = SpatialOverlays(args, source, stack)
        t, xyz, _ = next(source.clouds())
        spatial.update_map_scan(t, xyz)
        assert spatial.frame(t, source.pose(t)).scan is xyz
        t += 250_000_000
        frame = spatial.frame(t, source.pose(t))
        assert frame.scan is None
        assert frame.plan and frame.corridor


def test_suggested_scan_prefers_raw_livox_over_world_and_unlabelled_cloud():
    from slam_fpv.sensor_topics import LIVOX_TYPES, suggested_topic
    from slam_fpv.spatial import CLOUD
    topics = [{"type": CLOUD, "topic": "/cloud_registered", "frame": "map"},
              {"type": CLOUD, "topic": "/cloud", "frame": "sensor"},
              {"type": LIVOX_TYPES[0], "topic": "/livox/lidar", "frame": "livox"}]
    assert suggested_topic(topics, "scan", "map", "body") == "/livox/lidar"
    assert suggested_topic(topics[:1], "scan", "map", "body") == ""
