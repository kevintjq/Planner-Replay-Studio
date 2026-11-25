"""Read an optional flight video and composite it in normalized frame coordinates."""

from math import floor, isfinite

import imageio.v2 as imageio
import numpy as np
from PIL import Image


class FlightVideo:
    def __init__(self, path, offset):
        self.path = path
        self.offset = offset
        self.reader = None

    def __enter__(self):
        if self.path is not None:
            self.reader = imageio.get_reader(str(self.path), format="FFMPEG")
            try:
                metadata = self.reader.get_meta_data()
                self.fps = metadata["fps"]
                if not isfinite(self.fps) or self.fps <= 0:
                    raise ValueError("Flight video has no usable frame rate")
                self.frame_count = self.reader.count_frames()
                if not self.frame_count:
                    raise ValueError("Flight video has no decodable frames")
            except Exception:
                self.reader.close()
                raise
        return self

    def __exit__(self, *_):
        if self.reader is not None:
            self.reader.close()

    def frame_at(self, bag_seconds):
        if self.reader is None:
            return None
        time = bag_seconds - self.offset
        if time < 0:
            return None
        index = floor(time * self.fps + 1e-8)
        if index >= self.frame_count:
            return None
        return self.reader.get_data(index)


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
