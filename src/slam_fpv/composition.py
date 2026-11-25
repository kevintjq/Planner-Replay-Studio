"""Render the same editable overlays in the preview and finished video."""

from dataclasses import dataclass
from functools import lru_cache
from math import cos, isfinite, radians, sin
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageColor, ImageDraw, ImageFont, ImageOps

from .footage import VideoEdit, video_rect


@dataclass(frozen=True)
class VideoLayer:
    path: Path
    edit: VideoEdit = VideoEdit()
    width: float = .29
    position: tuple = (.055, .84)
    opacity: float = .96
    backdrop: bool = False

    def __post_init__(self):
        if not .05 <= self.width <= .9 or not 0 < self.opacity <= 1:
            raise ValueError("宽度应为 5–90%，不透明度应为 1–100%。")


@dataclass(frozen=True)
class TextLayer:
    text: str
    start: float
    end: float
    position: tuple = (.05, .08)
    size: int = 32
    color: str = "#F7FAFF"

    def __post_init__(self):
        if not self.text.strip() or len(self.text) > 160:
            raise ValueError("请输入 1–160 个字符。")
        if not isfinite(self.start) or not isfinite(self.end) or not 0 <= self.start < self.end:
            raise ValueError("文字结束时间须晚于非负的开始时间。")
        if not 12 <= self.size <= 120:
            raise ValueError("字号应为 12–120。")
        ImageColor.getrgb(self.color)


@dataclass(frozen=True)
class Composition:
    videos: tuple = ()
    texts: tuple = ()
    speed_position: tuple = (.05, .32)
    show_speed: bool = False
    speed_limit: float = 10.
    speed_start: float = 0.
    speed_end: float | None = None

    def __post_init__(self):
        if sum(layer.backdrop for layer in self.videos) > 1:
            raise ValueError("请选择一路全屏底片。")
        if not isfinite(self.speed_limit) or self.speed_limit <= 0:
            raise ValueError("速度表量程须大于 0。")
        if not isfinite(self.speed_start) or self.speed_start < 0:
            raise ValueError("速度表开始时间须为非负数。")
        if self.speed_end is not None and (not isfinite(self.speed_end) or self.speed_end <= self.speed_start):
            raise ValueError("速度表结束时间须晚于开始时间。")


class SpeedHistory:
    def __init__(self):
        self.previous = None
        self.peak = None
        self.distance = self.duration = 0.

    def update(self, seconds, value):
        if value is not None:
            self.peak = value if self.peak is None else max(self.peak, value)
            if self.previous is not None:
                previous_time, previous_value = self.previous
                span = seconds - previous_time
                self.distance += span * (value + previous_value) * .5
                self.duration += span
            self.previous = seconds, value
        else:
            self.previous = None
        average = self.distance / self.duration if self.duration else self.peak
        return value, self.peak, average


@lru_cache(maxsize=64)
def font(size, bold=False):
    windows = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts"
    paths = [windows / ("msyhbd.ttc" if bold else "msyh.ttc"),
             Path("/usr/share/fonts/opentype/noto") / ("NotoSansCJK-Bold.ttc" if bold else "NotoSansCJK-Regular.ttc")]
    for path in paths:
        if path.is_file():
            return ImageFont.truetype(str(path), size, index=2 if path.name.startswith("Noto") else 0)
    return ImageFont.load_default(size=size)


@lru_cache(maxsize=64)
def rounded_mask(size, radius, opacity=255):
    mask = Image.new("L", (size[0] * 2, size[1] * 2))
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, mask.width - 1, mask.height - 1), radius * 2, fill=opacity)
    return mask.resize(size, Image.Resampling.LANCZOS)


def label_tile(text, size, color):
    face = font(size, True)
    left, top, right, bottom = face.getbbox(text) if "\n" not in text else ImageDraw.Draw(
        Image.new("L", (1, 1))).multiline_textbbox((0, 0), text, font=face, spacing=size // 4)
    pad = max(3, size // 7)
    tile = Image.new("RGBA", (max(1, right - left + pad * 2), max(1, bottom - top + pad * 2)))
    ImageDraw.Draw(tile).multiline_text((pad - left, pad - top), text, font=face, fill=color,
                                       spacing=size // 4, stroke_width=max(1, size // 20),
                                       stroke_fill=(4, 10, 18, 195))
    return tile


@lru_cache(maxsize=32)
def gauge_face(size, limit):
    face = Image.new("RGBA", (size, round(size * .89)))
    draw = ImageDraw.Draw(face)
    center = size * .5, size * .48
    radius = size * .37
    box = (center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius)
    stroke = max(2, round(size * .025))
    draw.arc(box, 148, 392, fill=(43, 76, 101, 205), width=stroke)
    tick_font = font(max(7, round(size * .044)))
    for i in range(6):
        angle = radians(148 + 244 * i / 5)
        def point(r):
            return center[0] + r * cos(angle), center[1] + r * sin(angle)
        draw.line((point(radius - stroke), point(radius + stroke)), fill=(167, 215, 234, 205),
                  width=max(1, size // 150))
        draw.text(point(radius + size * .072), f"{i * limit / 5:g}", font=tick_font,
                  anchor="mm", fill="#F2F7FE", stroke_width=1, stroke_fill=(9, 16, 24, 185))
    return face


def speed_tile(readings, limit, height):
    size = max(115, round(height * .29))
    tile = gauge_face(size, limit).copy()
    draw = ImageDraw.Draw(tile)
    current, peak, average = readings or (None, None, None)
    center, radius = (size * .5, size * .48), size * .322
    box = (center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius)
    stop = min(1., max(0., current / limit)) if current is not None else 0.
    colors = ((93, 111, 249), (56, 212, 243), (106, 230, 171), (252, 208, 104), (250, 124, 95))
    draw.arc(box, 148, 392, fill=(57, 87, 113, 115), width=max(2, round(size * .025)))
    for index in range(40):
        x = index / 40
        segment = index // 10
        mix = (index % 10) / 10
        color = tuple(round(a + (b - a) * mix) for a, b in zip(colors[segment], colors[segment + 1]))
        draw.arc(box, 148 + 244 * x, 148 + 244 * (index + 1) / 40 + .8,
                 fill=(*color, 245 if x < stop else 128), width=max(2, round(size * .029)))
    if current is not None:
        angle = radians(148 + 244 * stop)
        x, y = center[0] + radius * cos(angle), center[1] + radius * sin(angle)
        dot = max(2, size * .018)
        draw.ellipse((x-dot, y-dot, x+dot, y+dot), fill="#FFFFFF")
    def value_text(value):
        return "—" if value is None else f"{value:.1f}"
    def center_text(x, y, text, frac, color, bold=False):
        draw.text((size*x, size*y), text, anchor="mm", fill=color, stroke_width=1,
                  stroke_fill=(7, 13, 21, 187), font=font(max(7, round(size*frac)), bold))
    center_text(.5, .32, "FLIGHT SPEED", .048, "#BEEBFA", True)
    center_text(.5, .48, value_text(current), .165, "#FFFFFF", True)
    center_text(.5, .60, "m/s", .049, "#DDEDF8")
    for x, name, value, color in ((.34, "MAX", peak, "#FFB483"), (.66, "AVG", average, "#89E7F4")):
        center_text(x, .72, name, .041, color, True)
        center_text(x, .81, value_text(value), .067, "#FFFFFF", True)
    return tile


def position_tile(canvas, tile, position):
    if tile.width > canvas[0] or tile.height > canvas[1]:
        tile.thumbnail(canvas, Image.Resampling.LANCZOS)
    return (round((canvas[0] - tile.width) * position[0]), round((canvas[1] - tile.height) * position[1]))


def compose(base, project, videos, elapsed, speed):
    """Return pixels and visible hit boxes in the same coordinates as the supplied frame."""
    scene = base if isinstance(base, Image.Image) else Image.fromarray(base)
    image = scene.convert("RGB")
    alpha = scene.getchannel("A") if scene.mode == "RGBA" else None
    for layer, footage in zip(project.videos, videos):
        if layer.backdrop and footage is not None and alpha is not None:
            source = footage if isinstance(footage, Image.Image) else Image.fromarray(footage)
            video = ImageOps.fit(source.convert("RGB"), image.size, Image.Resampling.LANCZOS)
            if layer.opacity < 1.:
                video = Image.blend(image, video, layer.opacity)
            video.paste(image, (0, 0), alpha)
            image = video
            break
    bounds = {}
    for index, (layer, footage) in enumerate(zip(project.videos, videos)):
        if footage is None or layer.backdrop:
            continue
        inset = footage if isinstance(footage, Image.Image) else Image.fromarray(footage)
        x, y, w, h = video_rect(image.size, inset.size, layer.width, layer.position)
        tile = inset.resize((w, h), Image.Resampling.LANCZOS)
        mask = rounded_mask((w, h), max(2, round(image.height * .016)), round(255 * layer.opacity))
        image.paste(tile, (x, y), mask)
        bounds["video", index] = x, y, w, h
    for index, layer in enumerate(project.texts):
        if layer.start <= elapsed < layer.end:
            tile = label_tile(layer.text, max(6, round(layer.size * image.height / 1080)), layer.color)
            x, y = position_tile(image.size, tile, layer.position)
            image.paste(tile, (x, y), tile)
            bounds["text", index] = x, y, *tile.size
    if project.show_speed and elapsed >= project.speed_start and (project.speed_end is None or elapsed < project.speed_end):
        tile = speed_tile(speed, project.speed_limit, image.height)
        x, y = position_tile(image.size, tile, project.speed_position)
        image.paste(tile, (x, y), tile)
        bounds["speed", 0] = x, y, *tile.size
    return image, bounds


def estimated_speed(source, time, pose):
    """A short displacement estimate using the same world-frame pose as the renderer."""
    radius = 120_000_000
    samples = []
    for bound, offset, limit in ((source.first, -radius, max), (source.last, radius, min)):
        stamp = limit(bound, time + offset)
        if stamp != time:
            try:
                samples.append((stamp, source.pose(stamp)[:3, 3]))
            except LookupError:
                pass
    if not samples:
        return None
    samples.append((time, pose[:3, 3]))
    samples.sort(key=lambda sample: sample[0])
    (start, p), (end, q) = samples[0], samples[-1]
    return float(np.linalg.norm(q - p) * 1e9 / (end - start))
