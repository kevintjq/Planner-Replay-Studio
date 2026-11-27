"""Check NVIDIA H.264 hardware encoding with the FFmpeg used for export."""

import subprocess

from imageio_ffmpeg import get_ffmpeg_exe


def h264_encoder(mode, width, height, fps):
    if mode == "cpu":
        return "libx264"
    command = [get_ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-f", "lavfi",
               "-i", f"color=c=black:s={width}x{height}:r={fps}", "-frames:v", "1",
               "-pix_fmt", "yuv420p", "-c:v", "h264_nvenc", "-f", "null", "-"]
    try:
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        result = subprocess.run(command, capture_output=True, text=True, timeout=12, creationflags=flags)
        if result.returncode == 0:
            return "h264_nvenc"
        detail = result.stderr.strip().splitlines()
    except (OSError, subprocess.TimeoutExpired) as error:
        detail = [str(error)]
    if mode == "nvenc":
        message = detail[0] if detail else "FFmpeg encoder initialization failed"
        raise RuntimeError(f"NVENC 不可用，改选自动或 CPU。{message}")
    return "libx264"
