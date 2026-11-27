"""Independent fixed-camera tracking window."""

from math import isfinite
from pathlib import Path
from tempfile import TemporaryDirectory
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import av

from .export_media import publish_output
from .footage import VideoReader
from .trajectory_image import track_trajectory
from .video_editor import VideoEditor


class TrajectoryTool:
    def __init__(self, app, path):
        self.app, self.path = app, path
        self.reader = VideoReader(path)
        frame = self.reader.current
        self.frame = self.reader.image(frame)
        self.time = self.reader.time(frame)
        self.result = self.region = self.origin = self.bounds = None
        self.working = False

        self.window = tk.Toplevel(app.root)
        self.window.title(f"固定机位轨迹图 · {path.name}")
        self.window.geometry("980x690")
        self.window.minsize(650, 460)
        self.window.transient(app.root)
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        panel = ttk.Frame(self.window, padding=14)
        panel.pack(fill="both", expand=True)
        ttk.Label(panel, text="选择起始帧，在画面上拖框选中无人机。", style="PanelTitle.TLabel").pack(anchor="w")
        self.canvas = tk.Canvas(panel, bg="#101824", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, pady=8)
        self.canvas.bind("<Configure>", lambda _: self.draw())
        self.canvas.bind("<ButtonPress-1>", self.press)
        self.canvas.bind("<B1-Motion>", self.drag)
        self.canvas.bind("<ButtonRelease-1>", self.release)

        row = ttk.Frame(panel)
        row.pack(fill="x")
        self.seek_time = tk.DoubleVar(value=self.time)
        self.end = tk.StringVar()
        self.seconds = tk.StringVar(value=f"{self.time:.3f}")
        ttk.Label(row, text="起始帧").pack(side="left")
        self.scale = ttk.Scale(row, from_=0., to=max(0., (self.reader.duration or 0.)-self.reader.tick),
                               variable=self.seek_time)
        self.scale.pack(side="left", fill="x", expand=True, padx=6)
        self.scale.bind("<ButtonRelease-1>", lambda _: self.seek(self.seek_time.get()))
        self.scale.bind("<KeyRelease>", lambda _: self.seek(self.seek_time.get()))
        entry = ttk.Entry(row, textvariable=self.seconds, width=9)
        entry.pack(side="left")
        entry.bind("<Return>", lambda _: self.seek())
        ttk.Label(row, text="结束秒（留空到片尾）").pack(side="left", padx=(14, 5))
        ttk.Entry(row, textvariable=self.end, width=8).pack(side="left")

        footer = ttk.Frame(panel)
        footer.pack(fill="x", pady=(10, 0))
        self.interval = tk.StringVar(value="自动")
        self.status = tk.StringVar(value="适用于固定机位。采样间隔越短，拖影越密。")
        ttk.Label(panel, textvariable=self.status, wraplength=710).pack(anchor="w", pady=(5,0))
        self.start_button = ttk.Button(footer, text="生成轨迹", style="Primary.TButton", command=self.start)
        self.start_button.pack(side="left")
        self.export_button = ttk.Button(footer, text="保存轨迹 PNG", state="disabled", command=self.save)
        self.export_button.pack(side="left", padx=7)
        self.cancel_button = ttk.Button(footer, text="取消跟踪", state="disabled", command=app.cancel)
        self.cancel_button.pack(side="right")
        ttk.Label(footer, text="采样间隔 (s)").pack(side="left", padx=(12, 5))
        self.interval_box = ttk.Combobox(footer, textvariable=self.interval, width=7,
                                         values=("自动", "0.25", "0.5", "1", "2", "3"))
        self.interval_box.pack(side="left")
        self.window.grab_set()

    def draw(self):
        image = self.frame if self.result is None else self.result.image
        self.bounds = VideoEditor.draw_image(self.canvas, image, "")
        self.draw_region(self.region)

    def draw_region(self, region):
        self.canvas.delete("region")
        if self.result is None and self.bounds and region:
            x, y, w, h = self.bounds
            a, b, c, d = region
            self.canvas.create_rectangle(x+a*w, y+b*h, x+c*w, y+d*h, outline="#70ECF1", width=2, tags="region")

    def point(self, event):
        x, y, w, h = self.bounds
        return min(1., max(0., (event.x-x)/w)), min(1., max(0., (event.y-y)/h))

    def current_region(self, event):
        x, y = self.point(event)
        ox, oy = self.origin
        return min(x, ox), min(y, oy), max(x, ox), max(y, oy)

    def press(self, event):
        if self.working or not self.bounds:
            return
        x, y, w, h = self.bounds
        if not (x <= event.x < x+w and y <= event.y < y+h):
            return
        self.result = self.region = None
        self.export_button.state(["disabled"])
        self.origin = self.point(event)
        self.draw()

    def drag(self, event):
        if self.origin is not None:
            self.draw_region(self.current_region(event))

    def release(self, event):
        if self.origin is None:
            return
        a, b, c, d = self.current_region(event)
        self.origin = None
        self.region = (a, b, c, d) if (c-a)*self.frame.width >= 6 and (d-b)*self.frame.height >= 6 else None
        self.draw()

    def seek(self, seconds=None):
        if self.working:
            return
        try:
            time = float(self.seconds.get()) if seconds is None else seconds
            if not isfinite(time) or time < 0:
                raise ValueError("起始时间须为非负数。")
            frame = self.reader.select(time)
            if frame is None:
                raise ValueError("该时间没有视频帧。")
            self.time = self.reader.time(frame)
            self.frame = self.reader.image(frame)
            self.seek_time.set(self.time)
            self.seconds.set(f"{self.time:.3f}")
            self.result = self.region = None
            self.export_button.state(["disabled"])
            self.draw()
        except (ValueError, OSError, av.FFmpegError) as error:
            self.status.set(str(error))

    def start(self):
        if self.working or self.app.busy:
            return
        if self.region is None:
            self.status.set("先在画面上框选无人机。")
            return
        try:
            end = float(self.end.get()) if self.end.get().strip() else None
            if end is not None and (not isfinite(end) or end <= self.time):
                raise ValueError("结束时间须晚于当前起始帧。")
            interval_value = self.interval.get().strip()
            try:
                interval = None if interval_value == "自动" else float(interval_value)
            except ValueError:
                raise ValueError("采样间隔请选择「自动」或输入秒数。") from None
            if interval is not None and (not isfinite(interval) or interval <= 0):
                raise ValueError("采样间隔须为大于零的秒数。")
        except ValueError as error:
            self.status.set(str(error))
            return
        start, region = self.time, self.region
        self.result = None
        self.draw()
        self.working = True
        self.start_button.state(["disabled"])
        self.export_button.state(["disabled"])
        self.cancel_button.state(["!disabled"])
        self.scale.state(["disabled"])
        self.interval_box.state(["disabled"])
        self.status.set("正在跟踪无人机…")

        def work():
            result = track_trajectory(self.path, start, end, region, self.app.check_cancel,
                                      lambda elapsed, total: self.app.events.put(("track_progress", (elapsed, total))),
                                      sample_interval=interval)
            return "trajectory", result

        self.app.run_job(work, "正在从视频生成轨迹…")

    def finish(self, result=None):
        self.working = False
        self.start_button.state(["!disabled"])
        self.cancel_button.state(["disabled"])
        self.scale.state(["!disabled"])
        self.interval_box.state(["!disabled"])
        if result is None:
            self.status.set("跟踪已停止，可重新选帧和框选。")
            return
        self.result = result
        self.export_button.state(["!disabled"])
        label = f"轨迹已生成 · {result.start:.2f}–{result.end:.2f} s"
        if result.lost_at is not None:
            label += f" · {result.lost_at:.2f} s 跟踪中断，图中保留中断前的轨迹"
        self.status.set(label)
        self.draw()

    def save(self):
        if self.result is None or self.working:
            return
        path = filedialog.asksaveasfilename(parent=self.window, defaultextension=".png", filetypes=(("PNG 图片", "*.png"),),
                                            initialfile=f"{self.path.stem}-trajectory.png", initialdir=self.app.export_directory())
        if not path:
            return
        destination = Path(path)
        output = destination.resolve()
        if (destination.suffix.lower() != ".png" or self.path.resolve() == output
                or not self.app.export_paths_safe({output})):
            self.status.set("请选择独立的 PNG 文件，保留源素材。")
            return
        try:
            cache = self.app.export_cache(destination)
            cache.mkdir(parents=True, exist_ok=True)
            with TemporaryDirectory(prefix=".trajectory-", dir=cache) as folder:
                staged = Path(folder) / destination.name
                self.result.image.save(staged, format="PNG")
                publish_output(staged, destination)
        except (ValueError, OSError) as error:
            self.status.set(str(error))
            return
        self.status.set(f"已保存 {destination.name}")

    def close(self):
        if self.working:
            self.app.cancel()
        self.reader.close()
        self.window.destroy()
        self.app.trajectory_tool = None
