"""Preflight the selected interval and optionally mux one aligned audio track."""
from dataclasses import dataclass
from errno import EXDEV
from math import ceil, isfinite
from pathlib import Path
import subprocess
from shutil import copyfileobj
from tempfile import NamedTemporaryFile, TemporaryDirectory

from imageio_ffmpeg import get_ffmpeg_exe
import numpy as np

from .footage import VideoReader


def publish_output(staged, destination):
    """Publish a staged file, including to a different drive."""
    try:
        staged.replace(destination)
        return
    except OSError as error:
        if error.errno != EXDEV and getattr(error, "winerror", None) != 17:
            raise
    pending = None
    try:
        with staged.open("rb") as source, NamedTemporaryFile(dir=destination.parent, delete=False) as target:
            pending = Path(target.name)
            copyfileobj(source, target)
        pending.replace(destination)
    finally:
        if pending is not None:
            pending.unlink(missing_ok=True)


@dataclass(frozen=True)
class MediaInfo:
    duration: float | None
    origin: float
    has_audio: bool


def media_info(path):
    with VideoReader(path) as reader:
        return MediaInfo(reader.duration, reader.origin, bool(reader.container.streams.audio))


def inspect_export(args, source, project, check_cancel):
    start = source.first + round(args.start * 1e9)
    end = min(source.last, start + round(args.seconds * 1e9))
    if start > end:
        raise ValueError("导出起点超出录包。")
    samples = np.linspace(0, end-start, min(121, max(2, ceil((end-start)/1e8)+1))).astype(np.int64)
    stamps = tuple(start + int(offset) for offset in samples)
    lines, blockers = [], []
    gaps = 0
    for stamp in stamps:
        check_cancel()
        try:
            source.pose(stamp)
        except LookupError:
            gaps += 1
    if gaps:
        blockers.append(f"位姿或 TF 在 {gaps}/{len(stamps)} 个抽查时刻缺失。请检查坐标系或缩短导出区间。")
    else:
        lines.append(f"位姿 / TF · {len(stamps)} 个抽查时刻可用")
    for frame, (first, last) in getattr(source, "cloud_frame_ranges", {}).items():
        left, right = max(first, start), min(last, end)
        if right < left:
            continue
        operations = {left, right, *(stamp for stamp in stamps if left < stamp < right)}
        failures = 0
        for stamp in sorted(operations):
            check_cancel()
            try:
                source.tree.lookup(source.world, frame, stamp)
            except LookupError:
                failures += 1
        if failures:
            blockers.append(f"点云 {frame} → {source.world} · {failures}/{len(operations)} 个抽查时刻缺少 TF")
        else:
            lines.append(f"点云坐标系 · {frame} → {source.world}")
    metadata = getattr(source, "mission", {})
    for name, label in (("plan", "规划轨迹"), ("goal", "目标点"), ("corridor", "安全走廊")):
        if name not in metadata:
            continue
        available = 0
        for stamp in stamps:
            check_cancel()
            available += bool(source.mission_shapes(name, stamp))
        lines.append(f"{label} · {available}/{len(stamps)} 个抽查时刻可绘制")
    lo, hi = (start-source.first)/1e9, (end-source.first)/1e9
    if hi < args.start + args.seconds - 1e-6:
        lines.append(f"录包在 {hi:.3f} s 结束，导出会在此处截断。")
    audio_ids = []
    for layer in project.videos:
        check_cancel()
        info = media_info(layer.path)
        if info.has_audio:
            audio_ids.append(layer.id)
        first, last = layer.edit.video_time(lo), layer.edit.video_time(hi)
        spans = tuple(layer.edit.spans(info.duration))
        if not spans:
            lines.append(f"{layer.path.name} · 当前没有保留的片段")
            continue
        tail = spans[-1][1]
        head = min(hi-lo, max(0., (spans[0][0]-first)/layer.edit.rate))
        end_gap = min(hi-lo, max(0., (last-tail)/layer.edit.rate)) if tail is not None else None
        fps = args.fps
        tail_text = "片尾时长未知" if end_gap is None else f"片尾缺 {ceil(max(0., end_gap*fps-1e-7))} 帧"
        retained = sum(max(0., min(last, b if b is not None else last) - max(first, a)) for a, b in spans)
        deleted = max(0., (hi-lo) - head - (end_gap or 0.) - retained/layer.edit.rate)
        cut_note = f" · 中间留空 {deleted:.2f} s" if deleted > 1/fps and len(spans) > 1 else ""
        lines.append(f"{layer.path.name} · 片头缺 {ceil(max(0., head*fps-1e-7))} 帧 · {tail_text}{cut_note}")
    return tuple(lines), tuple(blockers), tuple(audio_ids)


def tempo_filters(rate):
    if not isfinite(rate) or rate <= 0:
        raise ValueError("音频速度无效。")
    filters = []
    while rate > 2.:
        filters.append("atempo=2")
        rate /= 2.
    while rate < .5:
        filters.append("atempo=0.5")
        rate *= 2.
    if abs(rate-1.) > 1e-9:
        filters.append(f"atempo={rate:.12g}")
    return filters


def mux_audio(destination, layer, elapsed, duration, play_rate, check_cancel=None):
    info = media_info(layer.path)
    if not info.has_audio:
        raise ValueError(f"{layer.path.name} 没有音轨。")
    source_start = layer.edit.video_time(elapsed)
    source_end = layer.edit.video_time(elapsed + duration*play_rate)
    spans = [(max(first, source_start), min(last if last is not None else source_end, source_end))
             for first, last in layer.edit.spans(info.duration)]
    spans = [(first, last) for first, last in spans if last > first]
    rate = layer.edit.rate * play_rate
    with TemporaryDirectory(prefix=".audio-", dir=destination.parent) as tmp:
        staged = Path(tmp) / "mux.mp4"
        command = [get_ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-copyts",
                   "-i", str(destination)]
        if spans:
            lower, upper = spans[0][0], spans[-1][1]
            start, end = lower + info.origin, upper + info.origin
            delay = round(max(0., (lower-source_start)/rate)*1000)
            filters = [f"atrim=start={start:.9f}:end={end:.9f}",
                       f"asetpts=PTS-({start:.9f})/TB", "aresample=48000:async=1:first_pts=0",
                       *tempo_filters(rate)]
            gaps = [(a[1], b[0]) for a, b in zip(spans, spans[1:]) if a[1] < b[0]]
            if gaps:
                # Small audio frames keep the mute near the corresponding video cut.
                enabled = "+".join(f"gte(t,{(a-lower)/rate:.9f})*lt(t,{(b-lower)/rate:.9f})" for a, b in gaps)
                filters += ["asetnsamples=n=128:p=0", f"volume=0:enable='{enabled}'"]
            filters += [f"adelay={delay}:all=1", "apad"]
            command += ["-i", str(layer.path), "-filter_complex", "[1:a:0]" + ",".join(filters) + "[audio]",
                        "-map", "0:v:0", "-map", "[audio]"]
        else:
            command += ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-map", "0:v:0", "-map", "1:a:0"]
        command += ["-c:v", "copy", "-c:a", "aac", "-t", f"{duration:.9f}", "-movflags", "+faststart", str(staged)]
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        with subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=flags) as process:
            try:
                while True:
                    if check_cancel is not None:
                        check_cancel()
                    try:
                        _, error = process.communicate(timeout=.15)
                        break
                    except subprocess.TimeoutExpired:
                        continue
            except BaseException:
                process.kill()
                process.communicate()
                raise
            if process.returncode:
                raise ValueError(f"原声合并失败。{error.decode(errors='replace').strip()}")
        staged.replace(destination)
