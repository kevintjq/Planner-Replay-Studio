"""Layer translucent drone silhouettes from fixed-camera footage."""

from collections import OrderedDict
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from math import floor, hypot, isfinite

import numpy as np
from PIL import Image
from . import _ghost_mask
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


def timed_ghost_indices(samples, interval):
    """Take the first tracked frame at or after each interval on the video clock."""
    if interval <= 1e-8:
        return list(range(len(samples)))
    first = samples[0][0]
    chosen = [0]
    tick = 1
    for i, (stamp, _) in enumerate(samples[1:], 1):
        position = (stamp - first + 1e-8) / interval
        if position >= tick:
            chosen.append(i)
            tick = floor(position) + 1
    return chosen


@dataclass(frozen=True)
class TrajectoryTrack:
    path: Path
    background: Image.Image
    samples: tuple
    points: tuple
    size: tuple
    lost_at: float | None

    @property
    def start(self):
        return self.samples[0][0]

    @property
    def end(self):
        return self.samples[-1][0]


@dataclass(frozen=True)
class GhostRender:
    image: Image.Image
    frames: tuple


class TrajectoryComposer:
    """Retain only reference frames and cropped sprites across edits."""
    def __init__(self, track):
        self.track = track
        self.reference = tuple(np.linspace(0, len(track.samples)-1, min(12, len(track.samples))).round().astype(int))
        self.frames = {}
        self.sprites = OrderedDict()
        self.cache_bytes = 0
        self.clean_plate = None
        self.preview_plate = None

    def rgb(self, reader, index):
        frame = reader.select(self.track.samples[index][0])
        if frame is None:
            return None
        image = reader.image(frame)
        return np.asarray(image.resize(self.track.size, Image.Resampling.BILINEAR))

    def crop_bounds(self, index):
        width, height = self.track.size
        x, y, w, h = self.track.samples[index][1]
        pad = max(1, round(min(w, h)*.09))
        return max(0, x-pad), max(0, y-pad), min(width, x+w+pad), min(height, y+h+pad)

    def reference_crops(self, index, bounds):
        x0, y0, x1, y1 = bounds

        def overlap(candidate):
            a, b, c, d = self.track.samples[candidate][1]
            return max(0, min(x1, a+c)-max(x0, a))*max(0, min(y1, b+d)-max(y0, b))

        alternatives = sorted((j for j in self.frames if j != index), key=overlap)
        if not alternatives:
            return (), False
        clear = [j for j in alternatives if overlap(j) == 0]
        # Use an odd number of patches so the median is an actual image value.
        chosen = clear[:3] if len(clear) >= 3 else [alternatives[0]]
        return tuple(np.ascontiguousarray(self.frames[j][y0:y1, x0:x1]) for j in chosen), bool(clear)

    def target_box(self, bounds, target=None):
        width, height = self.track.size
        horizontal, vertical = target or self.track.background.size
        return tuple(round(value * (horizontal/width if i % 2 == 0 else vertical/height))
                     for i, value in enumerate(bounds))

    def init_background(self, check_cancel):
        import cv2

        with VideoReader(self.track.path) as reader:
            for i in self.reference:
                check_cancel()
                image = self.rgb(reader, i)
                if image is not None:
                    self.frames[i] = image

        bounds = self.crop_bounds(0)
        references, clear = self.reference_crops(0, bounds)
        original = self.track.background
        left, top, right, bottom = self.target_box(bounds)
        if clear:
            background = np.median(references, axis=0).astype(np.uint8)
            patch = Image.fromarray(background).resize((right-left, bottom-top), Image.Resampling.LANCZOS)
        else:
            # When the target never clears its starting box, estimate the small missing region.
            x, y, w, h = self.track.samples[0][1]
            left_roi, top_roi, right_roi, bottom_roi = self.target_box((x, y, x+w, y+h))
            pad = max(4, min(right-left, bottom-top)//2)
            area = (max(0, left-pad), max(0, top-pad),
                    min(original.width, right+pad), min(original.height, bottom+pad))
            pixels = np.asarray(original.crop(area))
            mask = np.zeros(pixels.shape[:2], dtype=np.uint8)
            mask[top_roi-area[1]:bottom_roi-area[1], left_roi-area[0]:right_roi-area[0]] = 255
            filled = cv2.inpaint(pixels, mask, 3, cv2.INPAINT_TELEA)
            patch = Image.fromarray(filled).crop((left-area[0], top-area[1], right-area[0], bottom-area[1]))
        self.clean_plate = original.convert("RGBA")
        self.clean_plate.paste(patch, (left, top))

    def make_sprite(self, index, rgb):
        import cv2

        bounds = self.crop_bounds(index)
        x0, y0, x1, y1 = bounds
        if rgb is None or x0 >= x1 or y0 >= y1:
            return None
        references, _ = self.reference_crops(index, bounds)
        crop = np.ascontiguousarray(rgb[y0:y1, x0:x1])
        if references:
            difference = np.frombuffer(_ghost_mask.rgb_difference(crop, *references), dtype=np.uint8)
            difference = difference.reshape(crop.shape[:2])
            border = np.concatenate((difference[0], difference[-1], difference[:, 0], difference[:, -1]))
            threshold = float(np.clip(np.median(border)+13., 14., 40.))
            mask = cv2.morphologyEx((difference > threshold).astype(np.uint8)*255, cv2.MORPH_CLOSE,
                                    np.ones((3, 3), np.uint8))
            _, _, w, h = self.track.samples[index][1]
            if np.count_nonzero(mask) >= max(6, .035*w*h):
                return bounds, Image.fromarray(crop), Image.fromarray(cv2.GaussianBlur(mask, (3, 3), 0))
        if index == 0:
            # The actual starting frame is always recoverable, even if segmentation is uncertain.
            mask = Image.new("L", (x1-x0, y1-y0), 255)
            return bounds, Image.fromarray(crop), mask
        return None

    def remember(self, index, sprite):
        cost = 1 if sprite is None else sprite[1].width * sprite[1].height * 4
        self.sprites[index] = sprite, cost
        self.cache_bytes += cost
        while len(self.sprites) > 1 and self.cache_bytes > 96 * 1024 * 1024:
            _, (_, used) = self.sprites.popitem(last=False)
            self.cache_bytes -= used

    def render(self, check_cancel, *, sample_interval=None, opacity=.9, excluded=frozenset(), preview=False):
        if sample_interval is not None and (not isfinite(sample_interval) or sample_interval <= 0):
            raise ValueError("采样间隔须为大于零的秒数。")
        if not isfinite(opacity) or not 0 <= opacity <= 1:
            raise ValueError("不透明度须为 0–100%。")
        if self.clean_plate is None:
            self.init_background(check_cancel)
        samples = self.track.samples
        chosen = (ghost_indices([box for _, box in samples]) if sample_interval is None
                  else timed_ghost_indices(samples, sample_interval))
        if preview and self.preview_plate is None:
            self.preview_plate = self.clean_plate.copy()
            self.preview_plate.thumbnail((1120, 760), Image.Resampling.LANCZOS)
        canvas = (self.preview_plate if preview else self.clean_plate).copy()
        alpha_table = [round(value*opacity) for value in range(256)]
        frames = []
        with ExitStack() as stack:
            reader = None
            for index in chosen:
                check_cancel()
                if index in excluded:
                    frames.append((index, samples[index][0], False))
                    continue
                if index not in self.sprites:
                    image = self.frames.get(index)
                    if image is None:
                        if reader is None:
                            reader = stack.enter_context(VideoReader(self.track.path))
                        image = self.rgb(reader, index)
                    self.remember(index, self.make_sprite(index, image))
                self.sprites.move_to_end(index)
                sprite, _ = self.sprites[index]
                frames.append((index, samples[index][0], sprite is not None))
                if sprite is None or opacity == 0:
                    continue
                bounds, foreground, alpha = sprite
                left, top, right, bottom = self.target_box(bounds, canvas.size)
                size = max(1, right-left), max(1, bottom-top)
                rgba = foreground.convert("RGBA")
                rgba.putalpha(alpha.point(alpha_table))
                if rgba.size != size:
                    rgba = rgba.resize(size, Image.Resampling.LANCZOS)
                canvas.alpha_composite(rgba, (left, top))
        return GhostRender(canvas.convert("RGB"), tuple(frames))


def track_motion(path, start, end, region, check_cancel, progress):
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
        x0, y0, x1, y1 = region
        if not (0. <= x0 < x1 <= 1. and 0. <= y0 < y1 <= 1.):
            raise ValueError("请在起始画面上框选无人机。")
        target = min((x1-x0)*original_width, (y1-y0)*original_height)
        scale = min(1., max(960/max(background.size), 24/max(target, 1)))
        width, height = max(1, round(original_width*scale)), max(1, round(original_height*scale))

        def cv_image(frame):
            if getattr(frame, "rotation", 0):
                image = reader.image(frame).resize((width, height), Image.Resampling.BILINEAR)
                return cv2.cvtColor(np.asarray(image), cv2.COLOR_RGB2BGR)
            return frame.to_ndarray(format="bgr24", width=width, height=height)
        x, y = round(x0*width), round(y0*height)
        w, h = round(x1*width)-x, round(y1*height)-y
        if min(w, h) < 6:
            raise ValueError("框选范围太小。请框住整架无人机。")

        def center(rect):
            x, y, w, h = rect
            return (x+w/2)*original_width/width, (y+h/2)*original_height/height

        params = cv2.TrackerCSRT_Params()
        params.template_size = 160
        params.number_of_scales = 17
        params.admm_iterations = 3
        tracker = cv2.TrackerCSRT_create(params)
        initial_box = x, y, w, h
        tracker.init(cv_image(initial), initial_box)
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
            valid, rect = tracker.update(cv_image(frame))
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
    return TrajectoryTrack(Path(path), background, tuple(samples), tuple(points), (width, height), lost_at)


def track_trajectory(path, start, end, region, check_cancel, progress, *, sample_interval=None):
    track = track_motion(path, start, end, region, check_cancel, progress)
    rendered = TrajectoryComposer(track).render(check_cancel, sample_interval=sample_interval)
    return TrajectoryResult(rendered.image, track.points, track.start, track.end, track.lost_at)
