import subprocess

import av
from imageio_ffmpeg import get_ffmpeg_exe
import numpy as np

from slam_fpv.composition import VideoLayer
from slam_fpv.export_media import mux_audio
from slam_fpv.footage import VideoEdit


def test_mux_preserves_aligned_audio_after_silence_and_keeps_video(tmp_path):
    ffmpeg = get_ffmpeg_exe()
    source, output = tmp_path / "clip.mp4", tmp_path / "output.mp4"
    inputs = [ffmpeg, "-v", "error", "-f", "lavfi", "-i", "color=s=64x48:r=24"]
    subprocess.run([*inputs, "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000",
                    "-t", "1", "-c:v", "libx264", "-c:a", "aac", str(source)], check=True)
    subprocess.run([*inputs, "-frames:v", "15", "-c:v", "libx264", str(output)], check=True)
    layer = VideoLayer(source, VideoEdit(offset=.12, trim_out=.7))
    mux_audio(output, layer, elapsed=0., duration=15/24, play_rate=.5)
    with av.open(str(output)) as mp4:
        assert mp4.streams.video[0].frames == 15
        assert mp4.streams.audio[0].codec_context.name == "aac"
        audio_frames = [(frame.time, np.max(np.abs(frame.to_ndarray()))) for frame in mp4.decode(audio=0)]
    assert audio_frames and max(peak for time, peak in audio_frames if time < .12) < .01
    onset = next(time for time, peak in audio_frames if peak > .035)
    assert .22 <= onset <= .26


def test_mux_mutes_deleted_middle_segment_without_ripple(tmp_path):
    ffmpeg = get_ffmpeg_exe()
    source, output = tmp_path / "clip.mp4", tmp_path / "output.mp4"
    inputs = [ffmpeg, "-v", "error", "-f", "lavfi", "-i", "color=s=64x48:r=24"]
    subprocess.run([*inputs, "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000",
                    "-t", "1", "-c:v", "libx264", "-c:a", "aac", str(source)], check=True)
    subprocess.run([*inputs, "-t", "1", "-c:v", "libx264", str(output)], check=True)
    edit = VideoEdit(segments=((0., .3), (.65, 1.)))
    mux_audio(output, VideoLayer(source, edit), elapsed=0., duration=1., play_rate=1.)
    with av.open(str(output)) as mp4:
        assert mp4.streams.video[0].frames == 24
        samples = [(frame.time, np.max(np.abs(frame.to_ndarray()))) for frame in mp4.decode(audio=0)]
    assert max(peak for time, peak in samples if .08 < time < .22) > .06
    assert max(peak for time, peak in samples if .37 < time < .57) < .01
    assert max(peak for time, peak in samples if .73 < time < .92) > .06
