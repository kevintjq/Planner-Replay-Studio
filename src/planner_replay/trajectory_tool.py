"""Inline fixed-camera motion-ghost editor."""

from math import isfinite
from pathlib import Path
from tempfile import TemporaryDirectory
import tkinter as tk
from tkinter import filedialog, ttk

import av

from .export_media import publish_output
from .footage import VideoReader
from .trajectory_image import TrajectoryComposer, track_motion
from .video_editor import VideoEditor


class TrajectoryTool:
    def __init__(self, app, path):
        self.app, self.path = app, path
        self.reader = VideoReader(path)
        frame = self.reader.current
        self.frame = self.reader.image(frame)
        self.time = self.reader.time(frame)
        self.result = self.region = self.origin = self.bounds = None
        self.composer = None
        self.excluded = set()
        self.working = False
        self.pending = None

        self.window = ttk.Frame(app.viewer.master, padding=14, style="Workspace.TFrame")
        app.viewer.grid_remove()
        self.window.grid(row=1, column=1, sticky="nsew")
        panel = self.window
        heading = ttk.Frame(panel)
        heading.pack(fill="x")
        ttk.Label(heading, text="固定机位轨迹图", style="PanelTitle.TLabel").pack(side="left")
        ttk.Button(heading, text="返回合成", command=self.close).pack(side="right")
        ttk.Label(panel, text="选择起始帧，在画面上拖框选中无人机。", style="Quiet.TLabel").pack(anchor="w")
        split = ttk.Frame(panel)
        split.pack(fill="both", expand=True, pady=8)
        frame_list = ttk.Frame(split)
        frame_list.pack(side="right", fill="y", padx=(6, 0))
        ttk.Label(frame_list, text="采样帧").pack(anchor="w")
        listing = ttk.Frame(frame_list)
        listing.pack(fill="both", expand=True)
        self.frame_list = ttk.Treeview(listing, columns=("time", "state"), show="headings", selectmode="browse")
        self.frame_list.heading("time", text="时间 (s)")
        self.frame_list.heading("state", text="状态")
        self.frame_list.column("time", width=68, stretch=False)
        self.frame_list.column("state", width=54, stretch=False)
        scrollbar = ttk.Scrollbar(listing, command=self.frame_list.yview)
        self.frame_list.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.frame_list.pack(side="left", fill="both", expand=True)
        ttk.Button(frame_list, text="删除 / 恢复选中帧", command=self.toggle_frame).pack(fill="x", pady=(4, 0))
        ttk.Button(frame_list, text="恢复全部", command=self.restore_frames).pack(fill="x")
        self.frame_list.bind("<Delete>", lambda _: self.remove_frame())
        self.frame_list.bind("<Double-1>", lambda _: self.toggle_frame())
        self.canvas = tk.Canvas(split, bg="#101824", highlightthickness=0)
        self.canvas.pack(side="left", fill="both", expand=True)
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
        self.interval_box.bind("<<ComboboxSelected>>", lambda _: self.schedule_render())
        self.interval_box.bind("<Return>", lambda _: self.schedule_render())
        self.interval_box.bind("<FocusOut>", lambda _: self.schedule_render())
        edit = ttk.Frame(panel)
        edit.pack(fill="x", pady=(8, 0))
        self.opacity = tk.DoubleVar(value=90)
        self.opacity_label = ttk.Label(edit, text="不透明度 90%", width=12)
        self.opacity_label.pack(side="left")
        ttk.Scale(edit, from_=0, to=100, variable=self.opacity, command=self.change_opacity).pack(
            side="left", fill="x", expand=True, padx=6)
        self.requested = None
        self.dirty = False

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
        self.clear_result()
        self.region = None
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
            self.clear_result()
            self.region = None
            self.draw()
        except (ValueError, OSError, av.FFmpegError) as error:
            self.status.set(str(error))

    def clear_result(self):
        if self.pending is not None:
            self.window.after_cancel(self.pending)
            self.pending = None
        self.result = self.composer = self.requested = None
        self.dirty = False
        self.excluded.clear()
        items = self.frame_list.get_children()
        if items:
            self.frame_list.delete(*items)
        self.export_button.state(["disabled"])

    def interval_seconds(self):
        text = self.interval.get().strip()
        if text == "自动":
            return None
        try:
            interval = float(text)
        except ValueError:
            raise ValueError("采样间隔请选择「自动」或输入秒数。") from None
        if not isfinite(interval) or interval <= 0:
            raise ValueError("采样间隔须为大于零的秒数。")
        return interval

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
            options = self.interval_seconds(), self.opacity.get()/100, frozenset()
        except ValueError as error:
            self.status.set(str(error))
            return
        start, region = self.time, self.region
        self.clear_result()
        self.draw()
        self.requested = options
        self.set_working(True)
        self.status.set("正在跟踪无人机…")

        def work():
            track = track_motion(self.path, start, end, region, self.app.check_cancel,
                                 lambda elapsed, total: self.app.events.put(("track_progress", (elapsed, total))))
            composer = TrajectoryComposer(track)
            interval, opacity, excluded = options
            result = composer.render(self.app.check_cancel, sample_interval=interval, opacity=opacity, excluded=excluded, preview=True)
            return "trajectory", (composer, result)

        self.app.run_job(work, "正在从视频生成轨迹…")

    def set_working(self, active):
        self.working = active
        for widget in (self.start_button, self.scale):
            widget.state(["disabled" if active else "!disabled"])
        self.cancel_button.state(["!disabled" if active else "disabled"])
        self.export_button.state(["!disabled" if not active and self.result is not None else "disabled"])

    def change_opacity(self, value):
        self.opacity_label.configure(text=f"不透明度 {float(value):.0f}%")
        self.schedule_render()

    def change_frame(self, *, restore=False):
        selection = self.frame_list.selection()
        if not selection:
            return
        index = int(selection[0])
        if restore and index in self.excluded:
            self.excluded.remove(index)
        else:
            self.excluded.add(index)
        self.schedule_render()

    def toggle_frame(self):
        self.change_frame(restore=True)

    def remove_frame(self):
        self.change_frame()

    def restore_frames(self):
        if self.excluded:
            self.excluded.clear()
            self.schedule_render()

    def edit_options(self):
        return self.interval_seconds(), round(self.opacity.get())/100, frozenset(self.excluded)

    def schedule_render(self):
        if self.composer is None and not self.working:
            return
        if self.composer is not None and not self.working:
            try:
                if self.edit_options() == self.requested and not self.dirty:
                    return
            except ValueError:
                pass
        self.dirty = True
        self.export_button.state(["disabled"])
        if self.pending is not None:
            self.window.after_cancel(self.pending)
        self.pending = self.window.after(100, self.render_edits)

    def render_edits(self):
        self.pending = None
        if self.working:
            return
        if self.composer is None:
            return
        if self.app.busy:
            self.pending = self.window.after(100, self.render_edits)
            return
        try:
            options = self.edit_options()
        except ValueError as error:
            self.status.set(str(error))
            return
        self.dirty = False
        if self.requested == options and self.result is not None:
            self.export_button.state(["!disabled"])
            return
        self.requested = options
        self.set_working(True)
        self.status.set("正在更新拖影…")
        composer = self.composer

        def work():
            interval, opacity, excluded = options
            result = composer.render(self.app.check_cancel, sample_interval=interval, opacity=opacity, excluded=excluded, preview=True)
            return "trajectory", (composer, result)

        self.app.run_job(work, "正在更新拖影…")

    def finish(self, result=None, error=None):
        self.set_working(False)
        if result is None:
            self.requested = None
            self.dirty = False
            self.status.set(error or "任务已取消。")
            self.export_button.state(["disabled"])
            return
        self.composer, self.result = result
        track = self.composer.track
        selection = self.frame_list.selection()
        items = self.frame_list.get_children()
        if items:
            self.frame_list.delete(*items)
        for index, seconds, painted in self.result.frames:
            status = "已删除" if index in self.excluded else "显示" if painted else "未分离"
            self.frame_list.insert("", "end", iid=str(index), values=(f"{seconds:.3f}", status))
        if selection and self.frame_list.exists(selection[0]):
            self.frame_list.selection_set(selection)
        self.export_button.state(["!disabled" if not self.dirty else "disabled"])
        visible = sum(painted for _, _, painted in self.result.frames)
        label = f"{track.start:.2f}–{track.end:.2f} s · {visible} 个拖影"
        if track.lost_at is not None:
            label += f" · {track.lost_at:.2f} s 跟踪中断"
        self.status.set(label)
        self.draw()
        if self.dirty and self.pending is None:
            self.pending = self.window.after(0, self.render_edits)

    def save(self):
        if self.result is None or self.working or self.dirty:
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
        interval, opacity, excluded = self.requested
        composer = self.composer
        try:
            cache = self.app.export_cache(destination)
        except (OSError, ValueError) as error:
            self.status.set(str(error))
            return
        self.set_working(True)
        self.status.set("正在保存原分辨率轨迹图…")

        def work():
            image = composer.render(self.app.check_cancel, sample_interval=interval,
                                    opacity=opacity, excluded=excluded).image
            cache.mkdir(parents=True, exist_ok=True)
            with TemporaryDirectory(prefix=".trajectory-", dir=cache) as folder:
                staged = Path(folder) / destination.name
                image.save(staged, format="PNG")
                self.app.check_cancel()
                publish_output(staged, destination)
            return "trajectory_saved", destination

        self.app.run_job(work, "正在保存原分辨率轨迹图…")

    def finish_save(self, path):
        self.set_working(False)
        self.status.set(f"已保存 {path.name}")
        if self.dirty and self.pending is None:
            self.pending = self.window.after(0, self.render_edits)

    def close(self):
        if self.working:
            self.status.set("请等待当前任务完成，或先取消。")
            return
        if self.pending is not None:
            self.window.after_cancel(self.pending)
        self.reader.close()
        self.window.destroy()
        self.app.viewer.grid()
        self.app.trajectory_tool = None
