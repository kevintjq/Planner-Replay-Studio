"""Buffered proxy playback and latest-request timeline seeking."""

from collections import OrderedDict
from pathlib import Path
from queue import Empty, Full, Queue
from time import monotonic

from .cli import open_source, render


def configure_preview(args, *, playing, viewport=None):
    scale = min(1., 1920 / args.width, 1080 / args.height)
    if viewport and min(viewport) > 1:
        display_scale = min(viewport[0] / args.width, viewport[1] / args.height)
        scale = min(scale, max(640 / args.width, 1.25 * display_scale))
    if scale < 1:
        args.width = max(8, round(args.width * scale / 2) * 2)
        args.height = max(8, round(args.height * scale / 2) * 2)
    args.supersample = 1
    if playing:
        args.fps = min(args.fps, 30)
    return args.width


class PreviewTransport:
    def __init__(self, app):
        self.app = app
        self.pending = self.timer = self.buffer = self.requested = None
        self.playing = self.working = False
        self.cache = OrderedDict()
        self.source = self.source_key = self.cache_key = None

    def clear(self):
        self.cache.clear()
        self.source = self.source_key = self.cache_key = None

    def request(self, seconds, *, delay=0):
        if self.app.busy and not self.working:
            return
        self.set_playing(False)
        self.pending = seconds
        if self.working and (self.buffer is not None or (delay == 0 and abs(self.requested-seconds) > 1e-6)):
            self.app.cancel_event.set()
        if delay == 0 or self.timer is None:
            self.schedule(delay)

    def schedule(self, delay=0):
        if self.timer is not None:
            self.app.root.after_cancel(self.timer)
        self.timer = self.app.root.after(delay, self.start_pending)

    def set_playing(self, playing):
        self.playing = playing
        self.app.play_button.configure(text="暂停" if playing else "播放")

    def toggle(self):
        if self.playing:
            self.stop()
            return
        if self.app.busy and not self.working:
            return
        start, length = self.app.timeline_range()
        time = self.app.elapsed if self.app.elapsed is not None else start
        end = min(start+length, self.app.source_duration or start+length)
        self.pending = time if start <= time < end-1e-4 else start
        self.set_playing(True)
        if self.working:
            self.app.cancel_event.set()
        self.schedule()

    def stop(self):
        self.pending = None
        self.set_playing(False)
        if self.working:
            self.app.cancel_event.set()

    def start_pending(self):
        self.timer = None
        if self.pending is None or self.app.busy:
            return
        app = self.app
        time, self.pending = self.pending, None
        start, duration = app.timeline_range()
        end = min(start+duration, app.source_duration or start+duration)
        time = max(start, min(time, end))
        try:
            args = app.build_args(Path("preview.png"))
        except ValueError as error:
            app.status.set(str(error))
            self.set_playing(False)
            return
        project, metadata = app.project, app.bag_metadata
        settings = vars(args).copy()
        playing = self.playing
        viewport = app.canvas.winfo_width(), app.canvas.winfo_height()
        limit = configure_preview(args, playing=playing, viewport=viewport)
        cache_key = settings, project, args.width, args.height
        if cache_key != self.cache_key:
            self.cache.clear()
            self.cache_key = cache_key
        tick = round((time-start) * settings["fps"])
        time = min(end, start + tick/settings["fps"])
        if not self.playing and (cached := self.cache.get(tick)) is not None:
            self.cache.move_to_end(tick)
            app.accept_preview(cached)
            return

        source_fields = (("demo_scene", "start", "seconds") if args.command == "demo" else
                         ("bag", "cloud_topic", "odom_topic", "world", "body", "pose_source", "max_gap",
                          "extrinsics", "plan_topic", "corridor_topic", "scan_topic", "goal_topic", "plan_frame",
                          "speed_topic", "camera_info_topic", "show_camera_fov", "camera_fov", "show_scan", "topic_displays"))
        source_key = (args.command, *(settings[name] for name in source_fields))
        frames = Queue(maxsize=8) if playing else None
        self.buffer, self.working, self.requested = frames, True, time
        self.working_fps = settings["fps"]
        self.working_start = start
        self.period = 1 / args.fps
        self.next_frame = monotonic()
        if playing:
            app.root.after(15, lambda: self.advance(frames))

        def work():
            if self.source_key != source_key:
                source = open_source(args, app.check_cancel, metadata)
                self.source, self.source_key = source, source_key
            source = self.source
            data, events, durations, result = (), [], [], []

            def preview_info(*status):
                nonlocal data
                data = status

            def frame_ready(*frame):
                payload = (frame, *data, tuple(events),
                           dict(zip((layer.id for layer in project.videos), durations)))
                if frames is None:
                    result.append(payload)
                    return
                while True:
                    app.check_cancel()
                    try:
                        frames.put(payload, timeout=.05)
                        break
                    except Full:
                        continue

            render(args, source, preview=frame_ready, preview_stream=playing, preview_media_width=limit,
                   preview_offset=time-start, preview_data=preview_info,
                   event_data=events.extend, media_data=durations.extend, check_cancel=app.check_cancel,
                   project=project)
            if frames is not None:
                while not frames.empty():
                    if app.cancel_event.wait(.03):
                        app.check_cancel()
                return "transport_done", None
            return "transport_frame", (tick, result[0])

        app.run_job(work, "正在缓冲预览…" if playing else "正在定位预览…", interactive=True)

    def advance(self, frames):
        if frames is not self.buffer or not self.playing:
            return
        now = monotonic()
        if now >= self.next_frame:
            try:
                payload = frames.get_nowait()
            except Empty:
                pass
            else:
                stamp = (payload[0][2] - self.working_start) * self.working_fps
                tick = round(stamp)
                if abs(stamp-tick) < 1e-5:
                    self.remember_frame(tick, payload)
                self.app.accept_preview(payload, playing=True)
                self.next_frame = max(self.next_frame + self.period, now)
        self.app.root.after(8, lambda: self.advance(frames))

    def remember_frame(self, tick, payload):
        self.cache[tick] = payload
        self.cache.move_to_end(tick)
        frame, videos, _, _, views = payload[0]
        bytes_per_frame = frame.nbytes + sum(video.nbytes for video in videos if video is not None)
        bytes_per_frame += sum(view.width * view.height * 4 for view in views.values())
        limit = max(1, min(42, 64 * 1024**2 // bytes_per_frame))
        while len(self.cache) > limit:
            self.cache.popitem(last=False)

    def finish(self, kind, value):
        self.buffer, self.requested = None, None
        self.working = False
        if kind == "transport_frame":
            tick, payload = value
            self.remember_frame(tick, payload)
            if self.pending is None or self.app.timeline.drag == "play":
                self.app.accept_preview(payload)
        elif self.pending is None:
            self.set_playing(False)
        if self.pending is not None:
            self.schedule()
        if kind == "transport_done":
            self.app.status.set("已播放到区间末尾。")
