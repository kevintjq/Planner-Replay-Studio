"""Track a user-selected target in fixed-camera footage and plot its image-plane path."""

from dataclasses import dataclass
from math import isfinite

import numpy as np
from PIL import Image, ImageDraw

from .composition import font
from .footage import VideoReader


@dataclass(frozen=True)
class TrajectoryResult:
    image: Image.Image
    points: tuple
    start: float
    end: float
    lost_at: float | None


def draw_trajectory(background, points):
    image = background.convert("RGB")
    draw = ImageDraw.Draw(image)
    width, height = image.size
    thickness = max(2, round(min(width, height)*.005))
    radius = max(4, thickness*2)
    draw.line(points, fill="#101B2A", width=thickness+4, joint="curve")
    draw.line(points, fill="#70ECF1", width=thickness, joint="curve")
    for (x, y), tint in ((points[0], "#46D79C"), (points[-1], "#FFB066")):
        draw.ellipse((x-radius, y-radius, x+radius, y+radius), fill=tint,
                     outline="#142133", width=max(1, thickness//2))
    size = max(9, round(min(width, height)*.025))
    inset = max(6, size//2)
    draw.text((inset, inset), "VIDEO TRACK  /  IMAGE PLANE", fill="white", font=font(size, True),
              stroke_width=max(1, size//10), stroke_fill="#142133")
    draw.text((inset, height-inset), "● START", fill="#46D79C", font=font(size, True),
              anchor="ls", stroke_width=max(1, size//10), stroke_fill="#142133")
    draw.text((width-inset, height-inset), "● END", fill="#FFB066", font=font(size, True),
              anchor="rs", stroke_width=max(1, size//10), stroke_fill="#142133")
    return image


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
            last = stamp
            if len(points) % 10 == 0:
                progress(last-first, max(0., total-first) if total is not None else None)
            frame = next(reader.frames, None)
        if len(points) < 2:
            raise ValueError("跟踪未得到连续轨迹。请重选起始帧，并紧贴无人机重新框选。")
        return TrajectoryResult(draw_trajectory(background, points), tuple(points), first, last, lost_at)
