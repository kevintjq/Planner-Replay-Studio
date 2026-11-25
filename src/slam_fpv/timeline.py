"""Scrubbable preview timeline and the selected annotation's editable interval."""

import tkinter as tk


class Timeline(tk.Canvas):
    def __init__(self, parent, seek, apply_interval, font):
        super().__init__(parent, height=91, bg="#FFFFFF", highlightthickness=0)
        self.seek = seek
        self.apply_interval = apply_interval
        self.face = font
        self.start = self.time = 0.
        self.duration = 6.
        self.fps = 60
        self.clip = None
        self.drag = None
        self.draft = None
        self.bind("<Configure>", lambda _: self.draw())
        self.bind("<ButtonPress-1>", self.press)
        self.bind("<B1-Motion>", self.move)
        self.bind("<ButtonRelease-1>", self.release)
        self.bind("<Left>", lambda _: self.keyboard(-1))
        self.bind("<Right>", lambda _: self.keyboard(1))
        self.busy = False

    def update_view(self, start, duration, time, clip, fps):
        self.start, self.duration, self.time, self.clip, self.fps = start, duration, time, clip, fps
        self.draw()

    def x(self, time):
        return 20 + max(1, self.winfo_width() - 40) * min(1., max(0., (time-self.start)/self.duration))

    def at(self, x):
        span = min(1., max(0., (x-20)/max(1, self.winfo_width()-40)))
        return self.start + span*self.duration

    def draw(self):
        self.delete("all")
        x0, x1 = self.x(self.start), self.x(self.start + self.duration)
        self.create_text(x0, 12, anchor="w", text="合成时间轴", font=(self.face, 9, "bold"), fill="#34445C")
        play = self.time if self.drag != "play" else self.draft
        self.create_text(x1, 12, anchor="e", text=f"{play:.3f} s", font=(self.face, 9), fill="#34445C")
        self.create_line(x0, 44, x1, 44, fill="#DCE7F6", width=8, capstyle="round")
        self.create_line(x0, 44, self.x(play), 44, fill="#6697EB", width=8, capstyle="round")
        for i in range(6):
            value = self.start + self.duration * i / 5
            self.create_text(self.x(value), 30, text=f"{value:.2f}", font=(self.face, 8), fill="#61758E")
        cx = self.x(play)
        self.create_oval(cx - 6, 38, cx + 6, 50, fill="#2165D9", outline="#FFFFFF", width=2)
        if self.clip is not None:
            start, end, label = self.clip
            if self.drag in ("start", "end"):
                start, end = self.draft
            left, right = self.x(start), self.x(end)
            self.create_line(x0, 72, x1, 72, fill="#EEF1F7", width=18, capstyle="round")
            if right > left:
                self.create_line(left, 72, right, 72, fill="#A8C8FD", width=18, capstyle="round")
                for x in (left, right):
                    self.create_line(x, 66, x, 78, fill="#2E62AC", width=2, capstyle="round")
                if right - left > 95:
                    short = label if len(label) < 20 else label[:18] + "…"
                    self.create_text((left + right) / 2, 72, text=short, font=(self.face, 8), fill="#173B72")
        else:
            self.create_text(x0, 72, text="选择文字或速度图层，拖动两端可调整显示区间",
                             font=(self.face, 8), fill="#61758E", anchor="w")

    def press(self, event):
        if self.busy:
            return
        self.focus_set()
        self.drag = "play"
        if self.clip is not None and 59 <= event.y <= 84:
            start, end, _ = self.clip
            distances = abs(event.x-self.x(start)), abs(event.x-self.x(end))
            edge = 0 if distances[0] <= distances[1] else 1
            if distances[edge] <= 14:
                self.drag = ("start","end")[edge]
        self.move(event)

    def move(self, event):
        if self.drag is None:
            return
        value = self.at(event.x)
        if self.drag == "play":
            self.draft = value
        else:
            start, end, _ = self.clip
            self.draft = ((min(value, end-.001), end) if self.drag == "start"
                          else (start, max(value, start+.001)))
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

    def keyboard(self, direction):
        if not self.busy:
            self.time = min(self.start+self.duration, max(self.start, self.time+direction/self.fps))
            self.draw()
            self.seek(self.time)
