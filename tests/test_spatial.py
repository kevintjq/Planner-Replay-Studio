from contextlib import ExitStack

import numpy as np
import pytest

from slam_fpv.cli import parser
from slam_fpv.sources import DemoSource
from slam_fpv.spatial import SensorView, SpatialOverlays, sensor_edges


def test_sensor_geometry_and_scan_expiry():
    camera = SensorView("optical", (90, 90), 3., True)
    corners = sensor_edges(camera).reshape(-1, 3)
    np.testing.assert_allclose(corners.max(axis=0), [3, 3, 3])
    np.testing.assert_allclose(corners.min(axis=0), [-3, -3, 0])
    lidar = SensorView("lidar", (360, 40), 5., False, 10.)
    points = sensor_edges(lidar).reshape(-1, 3)
    np.testing.assert_allclose(np.linalg.norm(points, axis=1), 5.)
    assert points[:, 2].max() == pytest.approx(2.5)
    with pytest.raises(ValueError):
        SensorView("lidar", (360, 40), 5., False, 80.)

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
