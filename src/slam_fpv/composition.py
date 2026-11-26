"""Render the same editable overlays in the preview and finished video."""

from dataclasses import dataclass, field, replace
from functools import lru_cache
from uuid import uuid4
from math import cos, isfinite, radians, sin
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageColor, ImageDraw, ImageFont, ImageOps

from .footage import VideoEdit, video_rect


@dataclass(frozen=True)
class VideoLayer:
    path: Path
    edit: VideoEdit = VideoEdit()
    width: float = .29
    position: tuple = (.055, .84)
    opacity: float = .96
    backdrop: bool = False
    backdrop_scale: float = 1.
    backdrop_position: tuple = (.5, .5)
    transparent: bool = True
    id: str = field(default_factory=lambda: uuid4().hex)

    def __post_init__(self):
        if not .05 <= self.width <= .9 or not 0 < self.opacity <= 1:
            raise ValueError("宽度应为 5–90%，不透明度应为 1–100%。")
        if not .2 <= self.backdrop_scale <= 2.:
            raise ValueError("底片缩放应为 20–200%。")


@dataclass(frozen=True)
class TextLayer:
    text: str
    start: float
    end: float
    position: tuple = (.05, .08)
    size: int = 32
    color: str = "#F7FAFF"
    transparent: bool = True

    def __post_init__(self):
        if not self.text.strip() or len(self.text) > 160:
            raise ValueError("请输入 1–160 个字符。")
        if not isfinite(self.start) or not isfinite(self.end) or not 0 <= self.start < self.end:
            raise ValueError("文字结束时间须晚于非负的开始时间。")
        if not 12 <= self.size <= 120:
            raise ValueError("字号应为 12–120。")
        ImageColor.getrgb(self.color)


POINT_CLOUD_VIEWS = {
    "rear": ("后方跟随", (.04, .88)),
    "oblique": ("高位斜视", (.97, .05)),
    "fpv": ("稳定 FPV", (.04, .05)),
    "fpv_raw": ("原始 FPV", (.04, .48)),
    "front": ("正前回望", (.50, .05)),
    "left": ("左侧跟随", (.04, .88)),
    "right": ("右侧跟随", (.96, .88)),
    "rear_left": ("左后斜视", (.04, .47)),
    "rear_right": ("右后斜视", (.96, .47)),
    "high": ("高位近俯视", (.50, .88)),
    "low": ("低位追踪", (.23, .88)),
    "orbit": ("环绕观察", (.75, .88)),
}

VIEW_OVERLAYS = {
    "actual": "已飞轨迹",
    "plan": "规划轨迹",
    "goal": "目标点",
    "corridor": "安全走廊",
    "scan": "雷达当前帧",
    "camera": "相机 FOV",
}
FPV_MODES = {"fpv", "fpv_raw"}


def view_overlays(kind, selected=None):
    if selected is not None:
        return frozenset(selected) - ({"actual", "plan"} if kind in FPV_MODES else set())
    return frozenset({"scan"} if kind in FPV_MODES else VIEW_OVERLAYS)


@dataclass(frozen=True)
class PointCloudView:
    kind: str = "oblique"
    width: float = .32
    position: tuple = (.97, .05)
    transparent: bool = True
    overlays: tuple | None = None

    def __post_init__(self):
        if self.overlays is not None and (len(set(self.overlays)) != len(self.overlays)
                                         or not set(self.overlays) <= VIEW_OVERLAYS.keys()):
            raise ValueError("未知或重复的视角叠加项。")
        if self.kind not in POINT_CLOUD_VIEWS:
            raise ValueError("未知的点云视角。")
        if not .12 <= self.width <= 1.5:
            raise ValueError("视角宽度应为 12–150%。")


LAYOUT_NAMES = {"mix": "当前合成", "video": "实拍全屏", "cloud": "点云全屏"}


@dataclass(frozen=True)
class LayoutCue:
    time: float
    mode: str
    video_id: str | None = None

    def __post_init__(self):
        if not isfinite(self.time) or self.time < 0 or self.mode not in LAYOUT_NAMES:
            raise ValueError("布局标记无效。")
        if (self.mode == "video") != bool(self.video_id):
            raise ValueError("实拍全屏须选一路视频。")


@dataclass(frozen=True)
class Composition:
    videos: tuple = ()
    views: tuple = (PointCloudView(),)
    main_view: PointCloudView = PointCloudView("rear", 1., (.5, .5))
    texts: tuple = ()
    speed_position: tuple = (.05, .32)
    show_speed: bool = False
    speed_limit: float = 10.
    speed_scale: float = 1.
    speed_transparent: bool = True
    speed_start: float = 0.
    speed_end: float | None = None
    transparent_canvas: bool = False
    layouts: tuple = ()
    audio_id: str | None = None
    play_rate: float = 1.
    compare_plans: bool = False

    def __post_init__(self):
        ids = {layer.id for layer in self.videos}
        if len(ids) != len(self.videos) or (self.audio_id is not None and self.audio_id not in ids):
            raise ValueError("视频图层引用无效。")
        if (any(cue.video_id and cue.video_id not in ids for cue in self.layouts) or
                any(a.time >= b.time for a, b in zip(self.layouts, self.layouts[1:]))):
            raise ValueError("布局标记须按时间递增，并引用项目内的视频。")
        if self.play_rate not in (1., .5, .25):
            raise ValueError("导出速度须为 1×、0.5× 或 0.25×。")
        if not .35 <= self.speed_scale <= 2.5:
            raise ValueError("速度表缩放应为 35–250%。")
        if len({view.kind for view in self.views}) != len(self.views):
            raise ValueError("每个附加视角只能添加一次。")
        if sum(layer.backdrop for layer in self.videos) > 1:
            raise ValueError("请选择一路全屏底片。")
        if not isfinite(self.speed_limit) or self.speed_limit <= 0:
            raise ValueError("速度表量程须大于 0。")
        if not isfinite(self.speed_start) or self.speed_start < 0:
            raise ValueError("速度表开始时间须为非负数。")
        if self.speed_end is not None and (not isfinite(self.speed_end) or self.speed_end <= self.speed_start):
            raise ValueError("速度表结束时间须晚于开始时间。")


    def layout_at(self, elapsed):
        return next((cue for cue in reversed(self.layouts) if cue.time <= elapsed), None)


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


MATTE = "#101620"


def placement_axis(window, layer):
    gap = window - layer
    if gap == 0:
        travel = max(1, round(window * .4))
        return -travel / 2, travel
    return 0, gap


def scaled_rect(canvas, source, scale, position):
    width = max(1, round(canvas[0] * scale))
    height = max(1, round(width * source[1] / source[0]))
    x, y = (round(offset + travel * origin) for (offset, travel), origin in
            zip((placement_axis(space, size) for space, size in zip(canvas, (width, height))), position))
    return x, y, width, height


def rounded_tile(tile, transparent, radius):
    if not transparent:
        panel = Image.new("RGBA", tile.size, MATTE)
        panel.alpha_composite(tile)
        tile = panel
    tile.putalpha(ImageChops.multiply(tile.getchannel("A"), rounded_mask(tile.size, radius)))
    return tile


def compose(base, project, videos, elapsed, speed, cloud_views=None, *, alpha=None):
    """Render a scene and return the draggable layer rectangles."""
    scene = base if isinstance(base, Image.Image) else Image.fromarray(base)
    color = scene.getpixel((0, 0)) if scene.mode == "RGBA" else None
    fallback = color[:3] if color is not None and color[3] == 0 else MATTE
    alpha = project.transparent_canvas if alpha is None else alpha
    cue = project.layout_at(elapsed)
    if cue and cue.mode == "video":
        footage = next((frame for layer, frame in zip(project.videos, videos)
                        if layer.id == cue.video_id), None)
        if footage is not None:
            image = footage if isinstance(footage, Image.Image) else Image.fromarray(footage)
            return ImageOps.fit(image.convert("RGBA" if alpha else "RGB"), scene.size, Image.Resampling.LANCZOS), {}
    if cue and cue.mode == "cloud":
        project = replace(project, videos=(), layouts=(), audio_id=None, views=(), texts=(), show_speed=False,
                          main_view=replace(project.main_view, width=1., position=(.5, .5)))
    image = Image.new("RGBA" if alpha else "RGB", scene.size, (0, 0, 0, 0) if alpha else fallback)

    def paste(tile, position, opacity=1.):
        mask = tile.getchannel("A")
        if opacity != 1.:
            mask = mask.point(lambda value: round(value * opacity))
        if alpha:
            if opacity != 1.:
                tile.putalpha(mask)
            image.alpha_composite(tile, position)
        else:
            image.paste(tile, position, mask)

    bounds = {}
    radius = max(2, round(image.height * .016))

    for index, (layer, footage) in enumerate(zip(project.videos, videos)):
        if layer.backdrop and footage is not None:
            source = footage if isinstance(footage, Image.Image) else Image.fromarray(footage)
            x, y, w, h = scaled_rect(image.size, image.size, layer.backdrop_scale, layer.backdrop_position)
            video = ImageOps.fit(source.convert("RGBA"), (w, h), Image.Resampling.LANCZOS)
            if layer.backdrop_scale < 1.:
                video = rounded_tile(video, layer.transparent, radius)
            paste(video, (x, y), layer.opacity)
            bounds["video", index] = x, y, w, h
            break

    main = project.main_view
    x, y, w, h = scaled_rect(image.size, scene.size, main.width, main.position)
    tile = scene.convert("RGBA").resize((w, h), Image.Resampling.LANCZOS)
    if main.width < 1.:
        tile = rounded_tile(tile, main.transparent, radius)
    elif not main.transparent:
        panel = Image.new("RGBA", tile.size, MATTE)
        panel.alpha_composite(tile)
        tile = panel
    paste(tile, (x, y))
    bounds["main", 0] = x, y, w, h

    for index, layer in enumerate(project.views):
        captured = None if cloud_views is None else cloud_views.get(layer.kind)
        if captured is None:
            continue
        inset = captured if isinstance(captured, Image.Image) else Image.fromarray(captured)
        x, y, w, h = scaled_rect(image.size, inset.size, layer.width, layer.position)
        tile = rounded_tile(inset.resize((w, h), Image.Resampling.LANCZOS), layer.transparent, radius)
        paste(tile, (x, y))
        bounds["view", index] = x, y, w, h

    for index, (layer, footage) in enumerate(zip(project.videos, videos)):
        if footage is None or layer.backdrop:
            continue
        inset = footage if isinstance(footage, Image.Image) else Image.fromarray(footage)
        x, y, w, h = video_rect(image.size, inset.size, layer.width, layer.position)
        if layer.transparent:
            tile = inset.convert("RGBA").resize((w, h), Image.Resampling.LANCZOS)
        else:
            # Visible matte makes the opaque background option clear for RGB footage.
            tile = Image.new("RGBA", (w, h), MATTE)
            stroke = min(radius // 2, (min(w, h) - 1) // 2)
            tile.paste(inset.resize((w - 2 * stroke, h - 2 * stroke), Image.Resampling.LANCZOS), (stroke, stroke))
        tile = rounded_tile(tile, layer.transparent, radius)
        paste(tile, (x, y), layer.opacity)
        bounds["video", index] = x, y, w, h

    for index, layer in enumerate(project.texts):
        if layer.start <= elapsed < layer.end:
            tile = label_tile(layer.text, max(6, round(layer.size * image.height / 1080)), layer.color)
            x, y = position_tile(image.size, tile, layer.position)
            if not layer.transparent:
                tile = rounded_tile(tile, False, radius)
            paste(tile, (x, y))
            bounds["text", index] = x, y, *tile.size
    if project.show_speed and elapsed >= project.speed_start and (project.speed_end is None or elapsed < project.speed_end):
        tile = speed_tile(speed, project.speed_limit, round(image.height * project.speed_scale))
        x, y = position_tile(image.size, tile, project.speed_position)
        if not project.speed_transparent:
            tile = rounded_tile(tile, False, radius)
        paste(tile, (x, y))
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
