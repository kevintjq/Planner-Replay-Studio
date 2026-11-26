"""Buffered proxy playback and latest-request timeline seeking."""

from collections import OrderedDict
from pathlib import Path
from queue import Empty, Full, Queue
from time import monotonic

from .cli import open_source, render


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
        cache_key = settings, project
        if cache_key != self.cache_key:
            self.cache.clear()
            self.cache_key = cache_key
        tick = round((time-start) * args.fps)
        time = min(end, start + tick/args.fps)
        if not self.playing and (cached := self.cache.get(tick)) is not None:
            self.cache.move_to_end(tick)
            app.accept_preview(cached)
            return

        playing = self.playing
        limit = 640
        if args.width > limit:
            args.height = max(8, round(args.height*limit/args.width/2)*2)
            args.width = limit
        args.supersample = 1
        if playing:
            args.fps = min(args.fps, 20)
        frames = Queue(maxsize=8) if playing else None
        self.buffer, self.working, self.requested = frames, True, time
        self.working_fps = settings["fps"]
        self.working_start = start
        self.period = 1 / args.fps
        self.next_frame = monotonic()
        if playing:
            app.root.after(15, lambda: self.advance(frames))

        def work():
            if self.source_key != settings:
                source = open_source(args, app.check_cancel, metadata)
                self.source, self.source_key = source, settings
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
        self.app.root.after(12, lambda: self.advance(frames))

    def remember_frame(self, tick, payload):
        self.cache[tick] = payload
        self.cache.move_to_end(tick)
        limit = max(4, 42 // (1 + len(self.app.project.videos)*2 + len(self.app.project.views)))
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
