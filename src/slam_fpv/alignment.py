"""Suggest motion events to check against an aligned view of the footage."""

import numpy as np


def motion_candidates(source, first, last, check_cancel):
    """Look for strong heading/velocity changes in a bounded number of pose samples."""
    last = min(last, (source.last-source.first)/1e9)
    if last-first < .8:
        return ()
    step = max(.15, (last-first)/1200)
    samples = []
    for elapsed in np.arange(first, last, step):
        check_cancel()
        try:
            pose = source.pose(source.first + round(elapsed*1e9))
        except LookupError:
            continue
        samples.append((elapsed, pose[:3, 3], np.arctan2(pose[1, 0], pose[0, 0])))
    if len(samples) < 5:
        return ()
    times = np.array([s[0] for s in samples])
    positions = np.stack([s[1] for s in samples])
    heading = np.unwrap([s[2] for s in samples])
    speeds = np.linalg.norm(np.diff(positions, axis=0), axis=1)/np.diff(times)
    candidates = []
    for i in range(1, len(samples)-2):
        delta = times[i+1]-times[i-1]
        if delta > 3*step:
            continue
        yaw = abs(heading[i+1]-heading[i-1])/delta
        dv = speeds[i]-speeds[i-1]
        if yaw >= .25 and yaw/.55 >= abs(dv)/.65:
            candidates.append((yaw/.55, times[i], "转弯"))
        elif dv < -.20:
            candidates.append((-dv/.40, times[i], "减速／急停"))
        elif positions[i+1, 2]-positions[i-1, 2] > .18 and speeds[i] > speeds[i-1]:
            candidates.append((dv/.40+.5, times[i], "上升／起飞"))
    result = []
    for _, elapsed, label in sorted(candidates, reverse=True):
        if all(abs(elapsed-other) >= 1.0 for other, _ in result):
            result.append((float(elapsed), label))
        if len(result) == 8:
            break
    return tuple(sorted(result))
