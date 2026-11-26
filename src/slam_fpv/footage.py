"""Timestamp-based footage editing and picture-in-picture composition."""

from dataclasses import dataclass
from math import isfinite

import av
import numpy as np
from PIL import Image


FULL_FRAME = (0., 0., 1., 1.)


@dataclass(frozen=True)
class VideoEdit:
    offset: float = 0.
    trim_in: float = 0.
    trim_out: float | None = None
    crop: tuple = FULL_FRAME
    anchors: tuple = ()

    def __post_init__(self):
        if not isfinite(self.offset) or not isfinite(self.trim_in) or self.trim_in < 0:
            raise ValueError("视频偏移须为有限数值，入点须为非负数")
        if self.trim_out is not None and (not isfinite(self.trim_out) or self.trim_out <= self.trim_in):
            raise ValueError("视频出点必须晚于入点")
        x0, y0, x1, y1 = self.crop
        if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
            raise ValueError("视频裁剪框无效")
        if len(self.anchors) > 2 or any(not isfinite(t) or t < 0 for pair in self.anchors for t in pair):
            raise ValueError("最多设置两组非负时间锚点")
        if len(self.anchors) == 2:
            (bag_a, video_a), (bag_b, video_b) = sorted(self.anchors)
            if bag_b <= bag_a or video_b <= video_a:
                raise ValueError("两组锚点必须在两个时间轴上依次递增")
            if not isfinite((video_b - video_a) / (bag_b - bag_a)):
                raise ValueError("两组锚点的时间间隔无效")

    @property
    def rate(self):
        if len(self.anchors) < 2:
            return 1.
        (bag_a, video_a), (bag_b, video_b) = sorted(self.anchors)
        return (video_b - video_a) / (bag_b - bag_a)

    def video_time(self, bag_seconds):
        if self.anchors:
            bag_anchor, video_anchor = self.anchors[0]
            return video_anchor + (bag_seconds - bag_anchor) * self.rate
        return bag_seconds - self.offset


    def bag_time(self, video_seconds):
        return (video_seconds - self.video_time(0.)) / self.rate


class VideoReader:
    """Use the presented timestamp and keep one lookahead frame for sequential export."""

    def __init__(self, path):
        self.container = av.open(str(path))
        try:
            if not self.container.streams.video:
                raise ValueError("文件中没有视频轨道")
            self.stream = self.container.streams.video[0]
            self.frames = self._timestamped_frames()
            self.current = next(self.frames, None)
            if self.current is None:
                raise ValueError("视频没有可读取的带时间戳帧")
            self.origin = self._timestamp(self.current)
            self.following = next(self.frames, None)
            self.tick = float(self.stream.time_base)
            self.duration = None
            if self.stream.duration is not None and self.stream.start_time is not None:
                self.duration = (self.stream.start_time + self.stream.duration) * self.tick - self.origin
            elif self.container.duration is not None:
                self.duration = self.container.duration / av.time_base
            self.nominal_step = 1 / float(self.stream.average_rate or 30)
        except BaseException:
            self.close()
            raise

    def _timestamped_frames(self):
        return (frame for frame in self.container.decode(self.stream) if frame.pts is not None)

    @staticmethod
    def _timestamp(frame):
        return float(frame.pts * frame.time_base)

    def time(self, frame):
        return self._timestamp(frame) - self.origin

    def select(self, seconds):
        if seconds < 0 or (self.duration is not None and seconds >= self.duration - 1e-9):
            return None
        current_time = self.time(self.current)
        if seconds < current_time - 1e-9 or seconds > current_time + 2:
            timestamp = int((self.origin + seconds) / self.tick + 1e-7)
            self.frames.close()
            self.container.seek(timestamp, stream=self.stream, backward=True)
            self.frames = self._timestamped_frames()
            self.current = next(self.frames, None)
            self.following = next(self.frames, None)
            if self.current is None:
                self.container.seek(int(round(self.origin / self.tick)), stream=self.stream, backward=True)
                self.frames = self._timestamped_frames()
                self.current = next(self.frames)
                self.following = next(self.frames, None)
        while self.following is not None and self.time(self.following) <= seconds + 1e-9:
            self.current, self.following = self.following, next(self.frames, None)
        time = self.time(self.current)
        if self.following is None:
            frame_duration = float((self.current.duration or 0) * self.current.time_base)
            end = self.duration if self.duration is not None and self.duration > time else time + (frame_duration or self.nominal_step)
            if seconds >= end - 1e-9:
                return None
        return self.current

    @staticmethod
    def image(frame):
        image = Image.fromarray(frame.to_ndarray(format="rgb24"))
        rotation = getattr(frame, "rotation", 0)
        if rotation:
            image = image.rotate(rotation, expand=True)
        return image

    def close(self):
        self.container.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class FlightVideo:
    def __init__(self, path, edit):
        self.path = path
        self.edit = edit
        self.reader = None
        self.cached_time = None
        self.cached_frame = None

    def __enter__(self):
        if self.path is not None:
            self.reader = VideoReader(self.path)
        return self

    def __exit__(self, *_):
        if self.reader is not None:
            self.reader.close()

    def frame_at(self, bag_seconds):
        if self.reader is None:
            return None
        time = self.edit.video_time(bag_seconds)
        if time < self.edit.trim_in - 1e-9 or (self.edit.trim_out is not None and time >= self.edit.trim_out - 1e-9):
            return None
        frame = self.reader.select(max(0., time))
        if frame is None:
            return None
        pts = self.reader.time(frame)
        if pts != self.cached_time:
            image = self.reader.image(frame)
            if self.edit.crop != FULL_FRAME:
                width, height = image.size
                x0, y0, x1, y1 = self.edit.crop
                left, top = min(width - 1, round(x0 * width)), min(height - 1, round(y0 * height))
                image = image.crop((left, top, max(left + 1, round(x1 * width)),
                                                max(top + 1, round(y1 * height))))
            self.cached_frame = np.asarray(image)
            self.cached_time = pts
        return self.cached_frame


def video_rect(canvas_size, video_size, fraction, position):
    width, height = canvas_size
    video_width, video_height = video_size
    scale = min(width * fraction / video_width, height * .9 / video_height)
    w, h = max(1, round(video_width * scale)), max(1, round(video_height * scale))
    x, y = position
    return round((width - w) * x), round((height - h) * y), w, h


def composite(frame, video, fraction, position):
    if video is None:
        return frame
    base = Image.fromarray(frame)
    inset = Image.fromarray(video)
    x, y, w, h = video_rect(base.size, inset.size, fraction, position)
    base.paste(inset.resize((w, h), Image.Resampling.LANCZOS), (x, y))
    return np.asarray(base)
