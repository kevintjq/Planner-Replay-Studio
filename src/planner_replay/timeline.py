"""Shared bag-time playhead, markers, and nondestructive layer trims."""

from dataclasses import dataclass
import tkinter as tk


TRACK_COLORS = {
    "video": ("#D9E7FD", "#3872CB"),
    "text": ("#F0E5FC", "#8356AD"),
    "image": ("#FFEAD9", "#935D30"),
    "speed": ("#DEF1EC", "#267867"),
}


@dataclass(frozen=True)
class Track:
    key: tuple
    start: float
    end: float
    title: str
    segments: tuple = ()
    media_bounds: tuple | None = None


class Timeline(tk.Canvas):
    gutter = 116
    track_top = 82
    row_height = 25
    visible_tracks = 4

    def __init__(self, parent, seek, apply_interval, font, jump=None, *, select, marker_menu, add_marker, scrub, move_layout, layout_menu, clip_menu):
        super().__init__(parent, height=self.track_top + self.row_height*self.visible_tracks + 3,
                         bg="#FFFFFF", highlightthickness=0)
        self.seek, self.jump = seek, jump or seek
        self.select, self.apply_interval = select, apply_interval
        self.marker_menu, self.add_marker = marker_menu, add_marker
        self.scrub, self.move_layout = scrub, move_layout
        self.layout_menu, self.clip_menu = layout_menu, clip_menu
        self.snap = True
        self.active_segment = None
        self.layout_drag = None
        self.clip_origin = None
        self.face = font
        self.start = self.time = 0.
        self.duration = 6.
        self.fps = 60
        self.events, self.layouts, self.markers, self.tracks = (), (), (), ()
        self.motions = ()
        self.selected = None
        self.top_track = 0
        self.drag = self.draft = None
        self.bind("<Configure>", lambda _: self.draw())
        self.bind("<ButtonPress-1>", self.press)
        self.bind("<B1-Motion>", self.move)
        self.bind("<ButtonRelease-1>", self.release)
        self.bind("<Button-3>", self.context)
        self.bind("<MouseWheel>", lambda event: self.scroll_tracks(-1 if event.delta > 0 else 1))
        self.bind("<Button-4>", lambda _: self.scroll_tracks(-1))
        self.bind("<Button-5>", lambda _: self.scroll_tracks(1))
        self.bind("<Left>", lambda _: self.keyboard(-1))
        self.bind("<Right>", lambda _: self.keyboard(1))
        self.bind("<Key-m>", lambda _: None if self.busy else self.add_marker())
        self.busy = False

    def update_view(self, start, duration, time, tracks, selected, fps, events=(), layouts=(), markers=(), motions=()):
        self.start, self.duration, self.time, self.fps = start, duration, time, fps
        self.tracks, self.selected = tracks, selected
        self.events, self.layouts, self.markers, self.motions = events, layouts, markers, motions
        self.top_track = min(self.top_track, max(0, len(tracks)-self.visible_tracks))
        self.draw()

    def x(self, time):
        return self.gutter + max(1, self.winfo_width()-self.gutter-15) * min(
            1., max(0., (time-self.start)/self.duration))

    def at(self, x):
        fraction = min(1., max(0., (x-self.gutter)/max(1, self.winfo_width()-self.gutter-15)))
        return self.start + fraction*self.duration

    def visible(self, time):
        return self.start <= time <= self.start+self.duration

    def draw(self):
        self.delete("all")
        x0, x1 = self.x(self.start), self.x(self.start+self.duration)
        self.create_text(9, 13, anchor="w", text="合成时间轴", font=(self.face, 9, "bold"), fill="#34445C")
        self.create_text(x1, 13, anchor="e", font=(self.face, 9), fill="#34445C", tags="playhead-time")
        self.create_line(x0, 39, x1, 39, fill="#DCE7F6", width=5)
        for index in range(6):
            value = self.start + self.duration*index/5
            self.create_text(self.x(value), 27, text=f"{value:.2f}", font=(self.face, 8), fill="#61758E")
        self.create_text(9, 52, anchor="w", text="事件 / 布局", font=(self.face, 8), fill="#61758E")
        for time, label in self.events:
            if self.visible(time):
                x = self.x(time)
                color = "#CB7A22" if label == "目标更新" else "#217E79"
                self.create_polygon(x-4, 49, x+4, 49, x, 56, fill=color)
        for cue in self.layouts:
            if self.visible(cue.time):
                x = self.x(self.draft if self.layout_drag == cue else cue.time)
                self.create_polygon(x-4, 58, x, 54, x+4, 58, x, 62, fill="#8866BF")
        for marker in self.markers:
            if self.visible(marker.time):
                x = self.x(marker.time)
                self.create_polygon(x-5, 50, x, 46, x+5, 50, x, 56, fill="#FFB849", outline="#9F580E")
        if self.motions:
            self.create_text(9, 72, anchor="w", text="图层缩窗", font=(self.face, 8), fill="#61758E")
        for motion in self.motions:
            time = self.draft if self.layout_drag == motion else motion.time
            shift = time - motion.time
            start, end = motion.start+shift, motion.end+shift
            if end >= self.start and start <= self.start+self.duration:
                self.create_line(self.x(start), 72, self.x(end), 72, fill="#62CAA9", width=3)
            if self.visible(time):
                x = self.x(time)
                self.create_polygon(x-5, 72, x, 67, x+5, 72, x, 77, fill="#087A60")
        for row, track in enumerate(self.tracks[self.top_track:self.top_track+self.visible_tracks]):
            self.draw_track(track, row, x0, x1)
        if not self.tracks:
            self.create_text(x0, self.track_top + 15, anchor="w", text="导入视频后，拖动片段两端裁剪。",
                             font=(self.face, 9), fill="#61758E")
        elif len(self.tracks) > self.visible_tracks:
            self.create_text(9, self.track_top+self.row_height*self.visible_tracks-1,
                             anchor="sw", text="滚轮查看更多", font=(self.face, 7), fill="#61758E")
        self.create_line(0, 0, 0, 0, fill="#2165D9", width=1, tags="playhead-line")
        self.create_oval(0, 0, 0, 0, fill="#2165D9", outline="#FFFFFF", tags="playhead-dot")
        self.update_playhead(self.time)

    def update_playhead(self, time):
        self.time = time
        play = self.draft if self.drag == "play" and self.draft is not None else time
        x = self.x(play)
        self.itemconfigure("playhead-time", text=f"{play:.3f} s")
        self.coords("playhead-line", x, 36, x, self.winfo_height()-3)
        self.coords("playhead-dot", x-5, 34, x+5, 44)

    def draw_track(self, track, row, x0, x1):
        y = self.track_top + row*self.row_height
        selected = track.key == self.selected
        self.create_rectangle(2, y, x1, y+self.row_height-2, outline="",
                              fill="#F1F5FD" if selected else "#F7F8FB")
        name = track.title if len(track.title) <= 12 else track.title[:11]+"…"
        self.create_text(9, y+11, text=name, anchor="w", font=(self.face, 8), fill="#34445C")
        segments = track.segments or ((track.start, track.end),)
        for i, (first, last) in enumerate(segments):
            active = selected and (self.active_segment == (track.key, i) or len(segments) == 1)
            if active and self.drag in ("left", "right", "clip") and isinstance(self.draft, tuple):
                first, last = self.draft
            left, right = self.x(first), self.x(last)
            if right <= left:
                continue
            fill, stroke = TRACK_COLORS[track.key[0]]
            self.create_rectangle(left, y+3, right, y+20, fill=fill, outline=stroke if active else "",
                                  width=2 if active else 1)
            if right-left > 76:
                self.create_text((left+right)/2, y+11, text=name, font=(self.face, 8), fill="#263950")
            if active:
                for x in (left, right):
                    self.create_line(x, y+6, x, y+17, fill=stroke, width=3)

    def track_at(self, y):
        row = (y-self.track_top)//self.row_height
        index = self.top_track + row
        return self.tracks[index] if 0 <= row < self.visible_tracks and index < len(self.tracks) else None

    def layout_at(self, x, y):
        items = self.motions if 65 <= y < self.track_top else self.layouts if 54 <= y < 65 else ()
        cues = [cue for cue in items if self.visible(cue.time) and abs(self.x(cue.time)-x) <= 8]
        return min(cues, key=lambda cue: abs(self.x(cue.time)-x), default=None)

    def snap_time(self, x, track=None):
        raw = self.at(x)
        return self.snap_value(raw, track)

    def snap_value(self, raw, track=None):
        x = self.x(raw)
        if self.snap:
            points = [self.start, self.start+self.duration, *(time for time, _ in self.events),
                      *(mark.time for mark in self.markers), *(cue.time for cue in (*self.layouts, *self.motions) if cue != self.layout_drag)]
            if self.drag != "play":
                points.append(self.time)
                for item in self.tracks:
                    for i, segment in enumerate(item.segments or ((item.start, item.end),)):
                        if self.active_segment != (item.key, i) or track != item:
                            points.extend(segment)
            nearby = [time for time in points if self.visible(time) and abs(self.x(time)-x) <= 7]
            if nearby:
                return min(nearby, key=lambda time: abs(time-raw))
        return min(self.start+self.duration, self.start + round((raw-self.start)*self.fps)/self.fps)

    def marker_at(self, x):
        close = [m for m in self.markers if self.visible(m.time) and abs(self.x(m.time)-x) <= 8]
        return min(close, key=lambda m: abs(self.x(m.time)-x), default=None)

    def context(self, event):
        if self.busy:
            return
        cue = self.layout_at(event.x, event.y)
        marker = self.marker_at(event.x) if 44 <= event.y < 55 else None
        track = self.track_at(event.y)
        if cue is not None:
            self.layout_menu(cue, event.x_root, event.y_root)
        elif marker is not None:
            self.marker_menu(marker, event.x_root, event.y_root)
        elif track is not None and track.key[0] == "video":
            self.select(track.key)
            time = self.at(event.x)
            self.active_segment = next(((track.key, i) for i, (a, b) in enumerate(track.segments)
                                        if a <= time < b), None)
            self.draw()
            self.clip_menu(time, event.x_root, event.y_root)

    def press(self, event):
        if self.busy:
            return
        self.focus_set()
        cue = self.layout_at(event.x, event.y)
        if cue is not None:
            self.drag, self.draft, self.layout_drag = "layout", cue.time, cue
            return
        if 44 <= event.y < self.track_top:
            marker = self.marker_at(event.x)
            if marker is not None:
                self.jump(marker.time)
                return
            near = [t for t, _ in self.events if self.visible(t) and abs(event.x-self.x(t)) <= 7]
            if near:
                self.jump(min(near, key=lambda t: abs(self.x(t)-event.x)))
                return
        self.drag = None
        track = self.track_at(event.y)
        if track is not None:
            self.select(track.key)
            self.selected = track.key
            self.active_segment = None
            if event.x < self.gutter-7:
                self.draw()
                return
            time = self.at(event.x)
            segments = track.segments or ((track.start, track.end),)
            candidates = [(abs(event.x-self.x(t)), i, side) for i, (a, b) in enumerate(segments)
                          for t, side in ((a, "left"), (b, "right")) if b > a and self.visible(t)]
            if candidates and min(candidates)[0] <= 8:
                _, index, self.drag = min(candidates)
                self.active_segment = track.key, index
            else:
                self.active_segment = next(((track.key, i) for i, (a, b) in enumerate(segments)
                                            if a <= time < b), None)
                if self.active_segment is not None and track.key[0] == "video":
                    self.drag = "clip"
                    self.clip_origin = event.x, time
        self.drag = self.drag or "play"
        self.move(event)

    def move(self, event):
        if self.drag is None:
            return
        if self.drag == "layout":
            self.draft = self.snap_time(event.x)
        elif self.drag == "play":
            value = self.snap_time(event.x)
            if value != self.draft:
                self.draft = value
                self.scrub(value)
            self.update_playhead(self.time)
            return
        else:
            key, index = self.active_segment
            track = next(t for t in self.tracks if t.key == key)
            segments = track.segments or ((track.start, track.end),)
            first, last = segments[index]
            value = self.snap_time(event.x, track)
            minimum = min(1/self.fps, last-first)
            media_start, media_end = track.media_bounds or (0., self.start+self.duration)
            lower = segments[index-1][1] if index else media_start
            upper = segments[index+1][0] if index+1 < len(segments) else media_end
            if self.drag == "clip":
                origin_x, origin_time = self.clip_origin
                if abs(event.x-origin_x) < 4:
                    self.draft = self.snap_time(event.x)
                else:
                    length = last-first
                    proposed = first + self.at(event.x)-origin_time
                    proposed = self.snap_value(proposed, track)
                    earliest, latest = min(first, lower), max(first, upper-length)
                    proposed = min(latest, max(earliest, proposed))
                    self.draft = proposed, proposed+length
            else:
                self.draft = ((max(lower, min(value, last-minimum)), last) if self.drag == "left"
                              else (first, min(upper, max(value, first+minimum))))
        self.draw()

    def release(self, _event):
        if self.drag is None:
            return
        action, value, cue = self.drag, self.draft, self.layout_drag
        self.drag = self.draft = self.layout_drag = self.clip_origin = None
        if action == "play" or (action == "clip" and not isinstance(value, tuple)):
            self.update_playhead(value)
            self.seek(value)
        elif action == "layout":
            if value == cue.time:
                self.jump(value)
            else:
                self.move_layout(cue, value)
        else:
            self.apply_interval(value, self.active_segment[1])

    def scroll_tracks(self, direction):
        self.top_track = min(max(0, len(self.tracks)-self.visible_tracks), max(0, self.top_track+direction))
        self.draw()
        return "break"

    def keyboard(self, direction):
        if not self.busy:
            self.update_playhead(min(self.start+self.duration, max(self.start, self.time+direction/self.fps)))
            self.seek(self.time)
