"""Shared bag-time playhead, markers, and nondestructive layer trims."""

from dataclasses import dataclass
import tkinter as tk


TRACK_COLORS = {
    "video": ("#D9E7FD", "#3872CB"),
    "text": ("#F0E5FC", "#8356AD"),
    "speed": ("#DEF1EC", "#267867"),
}


@dataclass(frozen=True)
class Track:
    key: tuple
    start: float
    end: float
    title: str


class Timeline(tk.Canvas):
    gutter = 116
    track_top = 66
    row_height = 25
    visible_tracks = 4

    def __init__(self, parent, seek, apply_interval, font, jump=None, *, select, marker_menu, add_marker):
        super().__init__(parent, height=self.track_top + self.row_height*self.visible_tracks + 3,
                         bg="#FFFFFF", highlightthickness=0)
        self.seek, self.jump = seek, jump or seek
        self.select, self.apply_interval = select, apply_interval
        self.marker_menu, self.add_marker = marker_menu, add_marker
        self.face = font
        self.start = self.time = 0.
        self.duration = 6.
        self.fps = 60
        self.events, self.layouts, self.markers, self.tracks = (), (), (), ()
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

    def update_view(self, start, duration, time, tracks, selected, fps, events=(), layouts=(), markers=()):
        self.start, self.duration, self.time, self.fps = start, duration, time, fps
        self.tracks, self.selected = tracks, selected
        self.events, self.layouts, self.markers = events, layouts, markers
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
        play = self.draft if self.drag == "play" else self.time
        self.create_text(9, 13, anchor="w", text="合成时间轴", font=(self.face, 9, "bold"), fill="#34445C")
        self.create_text(x1, 13, anchor="e", text=f"{play:.3f} s", font=(self.face, 9), fill="#34445C")
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
                x = self.x(cue.time)
                self.create_polygon(x-4, 58, x, 54, x+4, 58, x, 62, fill="#8866BF")
        for marker in self.markers:
            if self.visible(marker.time):
                x = self.x(marker.time)
                self.create_polygon(x-5, 50, x, 46, x+5, 50, x, 56, fill="#FFB849", outline="#9F580E")
        for row, track in enumerate(self.tracks[self.top_track:self.top_track+self.visible_tracks]):
            self.draw_track(track, row, x0, x1)
        if not self.tracks:
            self.create_text(x0, self.track_top + 15, anchor="w", text="导入视频后，拖动片段两端裁剪。",
                             font=(self.face, 9), fill="#61758E")
        elif len(self.tracks) > self.visible_tracks:
            self.create_text(9, self.track_top+self.row_height*self.visible_tracks-1,
                             anchor="sw", text="滚轮查看更多", font=(self.face, 7), fill="#61758E")
        x = self.x(play)
        self.create_line(x, 36, x, self.winfo_height()-3, fill="#2165D9", width=1)
        self.create_oval(x-5, 34, x+5, 44, fill="#2165D9", outline="#FFFFFF")

    def draw_track(self, track, row, x0, x1):
        y = self.track_top + row*self.row_height
        selected = track.key == self.selected
        self.create_rectangle(2, y, x1, y+self.row_height-2, outline="",
                              fill="#F1F5FD" if selected else "#F7F8FB")
        name = track.title if len(track.title) <= 12 else track.title[:11]+"…"
        self.create_text(9, y+11, text=name, anchor="w", font=(self.face, 8), fill="#34445C")
        first, last = track.start, track.end
        if selected and self.drag in ("left", "right"):
            first, last = self.draft
        left, right = self.x(first), self.x(last)
        if right <= left:
            return
        fill, stroke = TRACK_COLORS[track.key[0]]
        self.create_rectangle(left, y+3, right, y+20, fill=fill, outline=stroke if selected else "",
                              width=2 if selected else 1)
        if right-left > 76:
            self.create_text((left+right)/2, y+11, text=name, font=(self.face, 8), fill="#263950")
        if selected:
            for x in (left, right):
                self.create_line(x, y+6, x, y+17, fill=stroke, width=3)

    def marker_at(self, x):
        close = [m for m in self.markers if self.visible(m.time) and abs(self.x(m.time)-x) <= 8]
        return min(close, key=lambda m: abs(self.x(m.time)-x), default=None)

    def context(self, event):
        if not self.busy and 44 <= event.y < self.track_top:
            marker = self.marker_at(event.x)
            if marker is not None:
                self.marker_menu(marker, event.x_root, event.y_root)

    def press(self, event):
        if self.busy:
            return
        self.focus_set()
        if 44 <= event.y < self.track_top:
            marker = self.marker_at(event.x)
            if marker is not None:
                self.jump(marker.time)
                return
            marks = [t for t, _ in self.events] + [cue.time for cue in self.layouts]
            near = [t for t in marks if self.visible(t) and abs(event.x-self.x(t)) <= 7]
            if near:
                self.jump(min(near, key=lambda t: abs(event.x-self.x(t))))
                return
        row = (event.y-self.track_top)//self.row_height
        self.drag = None
        if 0 <= row < self.visible_tracks and row+self.top_track < len(self.tracks):
            track = self.tracks[row+self.top_track]
            self.select(track.key)
            self.selected = track.key
            if event.x < self.gutter-7:
                self.drag = None
                self.draw()
                return
            edges = [(abs(event.x-self.x(t)), side) for t, side in
                     ((track.start, "left"), (track.end, "right")) if track.end > track.start and self.visible(t)]
            if edges and min(edges)[0] <= 8:
                self.drag = min(edges)[1]
        self.drag = self.drag or "play"
        self.move(event)

    def move(self, event):
        if self.drag is None:
            return
        value = self.at(event.x)
        if self.drag == "play":
            self.draft = value
        else:
            value = self.start + round((value-self.start)*self.fps)/self.fps
            track = next(track for track in self.tracks if track.key == self.selected)
            minimum = min(1/self.fps, track.end-track.start)
            self.draft = ((min(value, track.end-minimum), track.end) if self.drag == "left"
                          else (track.start, max(value, track.start+minimum)))
        self.draw()

    def release(self, _event):
        if self.drag is None:
            return
        action, value = self.drag, self.draft
        self.drag = None
        if action == "play":
            self.time = value
            self.draw()
            self.seek(value)
        else:
            self.apply_interval(value)

    def scroll_tracks(self, direction):
        self.top_track = min(max(0, len(self.tracks)-self.visible_tracks), max(0, self.top_track+direction))
        self.draw()
        return "break"

    def keyboard(self, direction):
        if not self.busy:
            self.time = min(self.start+self.duration, max(self.start, self.time+direction/self.fps))
            self.draw()
            self.seek(self.time)
