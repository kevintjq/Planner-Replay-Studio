import numpy as np

from slam_fpv.alignment import motion_candidates


def test_alignment_candidates_find_a_turn_with_wrapped_yaw():
    class Source:
        first, last = 0, 4_000_000_000

        @staticmethod
        def pose(stamp):
            time = stamp / 1e9
            heading = 3 + .9*min(1., max(0., time-1.))
            c, s = np.cos(heading), np.sin(heading)
            return np.array([[c, -s, 0, time], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])

    results = motion_candidates(Source(), 0., 4., lambda: None)
    assert len(results) == 1
    elapsed, name = results[0]
    assert 1. <= elapsed <= 2.
    assert name == "转弯"
