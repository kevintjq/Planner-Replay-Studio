"""Layer translucent drone silhouettes from fixed-camera footage."""

from dataclasses import dataclass
from math import hypot, isfinite

import numpy as np
from PIL import Image
from .footage import VideoReader


@dataclass(frozen=True)
class TrajectoryResult:
    image: Image.Image
    points: tuple
    start: float
    end: float
    lost_at: float | None


def ghost_indices(boxes):
    """Avoid stacking copies at nearly identical positions when the drone hovers."""
    spacing = max(6., .55 * float(np.median([max(w, h) for _, _, w, h in boxes])))
    centers = [(x+w/2, y+h/2) for x, y, w, h in boxes]
    chosen = [0]
    for i, (x, y) in enumerate(centers[1:], 1):
        if all(hypot(x-centers[j][0], y-centers[j][1]) >= spacing for j in chosen):
            chosen.append(i)
    if len(chosen) > 7:
        chosen = [chosen[i] for i in np.linspace(0, len(chosen)-1, 7).round().astype(int)]
    return chosen


def draw_motion_shadows(reader, original, samples, size, check_cancel):
    import cv2

    boxes = [box for _, box in samples]
    chosen = ghost_indices(boxes)
    if len(chosen) < 2:
        raise ValueError("这段画面里无人机移动太小。请选一段位移更明显的视频。")
    reference = tuple(np.linspace(0, len(samples)-1, min(12, len(samples))).round().astype(int))
    frames = {}
    for i in sorted(set(reference) | set(chosen[1:])):
        check_cancel()
        frame = reader.select(samples[i][0])
        if frame is not None:
            frames[i] = np.asarray(reader.image(frame).resize(size, Image.Resampling.BILINEAR))
    canvas = original.convert("RGBA")
    width, height = size
    sx, sy = canvas.width/width, canvas.height/height
    painted = 0
    for step, i in enumerate(chosen[1:], 1):
        check_cancel()
        if i not in frames:
            continue
        x, y, w, h = boxes[i]
        pad = max(1, round(min(w, h)*.09))
        x0, y0, x1, y1 = max(0, x-pad), max(0, y-pad), min(width, x+w+pad), min(height, y+h+pad)

        def overlap(j):
            a, b, c, d = boxes[j]
            return max(0, min(x1, a+c)-max(x0, a))*max(0, min(y1, b+d)-max(y0, b))

        alternatives = sorted((j for j in reference if j != i and j in frames), key=overlap)
        if not alternatives:
            continue
        clear = [j for j in alternatives if overlap(j) == 0][:3]
        candidates = clear or alternatives[:1]
        crop = frames[i][y0:y1, x0:x1]
        base = np.median([frames[j][y0:y1, x0:x1] for j in candidates], axis=0)
        difference = np.max(np.abs(crop.astype(np.float32)-base), axis=2)
        border = np.concatenate((difference[0], difference[-1], difference[:, 0], difference[:, -1]))
        threshold = float(np.clip(np.median(border)+13., 14., 40.))
        mask = (difference > threshold).astype(np.uint8)*255
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
        if np.count_nonzero(mask) < max(6, .035*w*h):
            continue
        opacity = .44+.36*step/(len(chosen)-1)
        mask = np.rint(cv2.GaussianBlur(mask, (3, 3), 0)*opacity).astype(np.uint8)
        sprite = Image.fromarray(crop).convert("RGBA")
        sprite.putalpha(Image.fromarray(mask))
        left, top = round(x0*sx), round(y0*sy)
        span = round(x1*sx)-left, round(y1*sy)-top
        if sprite.size != span:
            sprite = sprite.resize(span, Image.Resampling.LANCZOS)
        canvas.alpha_composite(sprite, (left, top))
        painted += 1
    if not painted:
        raise ValueError("无法从背景分离无人机。请选光照稳定、目标清晰的固定机位片段。")
    return canvas.convert("RGB")


def track_trajectory(path, start, end, region, check_cancel, progress):
    import cv2

    if not isfinite(start) or start < 0 or (end is not None and (not isfinite(end) or end <= start)):
        raise ValueError("结束时间须晚于起始帧。")
    with VideoReader(path) as reader:
        check_cancel()
        initial = reader.select(start)
        if initial is None:
            raise ValueError("起始时间没有可读取的视频帧。")
        background = reader.image(initial)
        original_width, original_height = background.size
        scale = min(1., 1920/max(background.size))
        width, height = max(1, round(original_width*scale)), max(1, round(original_height*scale))

        def cv_image(image):
            if image.size != (width, height):
                image = image.resize((width, height), Image.Resampling.BILINEAR)
            return np.ascontiguousarray(np.asarray(image)[..., ::-1])

        x0, y0, x1, y1 = region
        if not (0. <= x0 < x1 <= 1. and 0. <= y0 < y1 <= 1.):
            raise ValueError("请在起始画面上框选无人机。")
        x, y = round(x0*width), round(y0*height)
        w, h = round(x1*width)-x, round(y1*height)-y
        if min(w, h) < 6:
            raise ValueError("框选范围太小。请框住整架无人机。")

        def center(rect):
            x, y, w, h = rect
            return (x+w/2)*original_width/width, (y+h/2)*original_height/height

        tracker = cv2.TrackerCSRT_create()
        initial_box = x, y, w, h
        tracker.init(cv_image(background), initial_box)
        points = [center(initial_box)]
        first = last = reader.time(initial)
        samples = [(first, initial_box)]
        lost_at = None
        frame = reader.following
        total = end if end is not None else reader.duration
        while frame is not None:
            check_cancel()
            stamp = reader.time(frame)
            if end is not None and stamp > end + 1e-8:
                break
            valid, rect = tracker.update(cv_image(reader.image(frame)))
            x, y, w, h = rect
            if not valid or min(w, h) <= 0 or not (0 <= x+w/2 < width and 0 <= y+h/2 < height):
                lost_at = stamp
                break
            points.append(center(rect))
            samples.append((stamp, rect))
            last = stamp
            if len(points) % 10 == 0:
                progress(last-first, max(0., total-first) if total is not None else None)
            frame = next(reader.frames, None)
        if len(points) < 2:
            raise ValueError("跟踪未得到连续轨迹。请重选起始帧，并紧贴无人机重新框选。")
    # Tracking consumes the frame iterator directly. Use a fresh decoder for the selected timestamps.
    with VideoReader(path) as reader:
        image = draw_motion_shadows(reader, background, samples, (width, height), check_cancel)
    return TrajectoryResult(image, tuple(points), first, last, lost_at)
