"""Side-by-side timestamp alignment and non-destructive footage trimming."""

from dataclasses import replace
from math import isfinite

import av
import tkinter as tk
from tkinter import messagebox, ttk

from PIL import Image, ImageTk

from .footage import FULL_FRAME, VideoEdit, VideoReader


class VideoEditor:
    def __init__(self, root, path, edit, slam_frame, slam_time, request_slam, apply, *, duration, fps, request_loop, request_candidates, on_close):
        self.reader = VideoReader(path)
        self.edit = edit
        self.request_slam = request_slam
        self.request_loop, self.request_candidates = request_loop, request_candidates
        self.fps = fps
        self.loop_job = None
        self.loop_origin = None
        self.apply = apply
        self.on_close = on_close
        self.slam_frame, self.slam_time = slam_frame, slam_time
        self.clip_frame = None
        self.clip_time = 0.
        self.aligning = False
        self.drag_start = None
        self.clip_bounds = None

        self.window = tk.Toplevel(root)
        self.window.title(f"裁剪与时间对齐  ·  {path.name}")
        self.window.geometry("1060x680")
        self.window.minsize(880, 550)
        self.window.transient(root)
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        self.window.grab_set()

        content = ttk.Frame(self.window, padding=14, style="Shell.TFrame")
        content.pack(fill="both", expand=True)
        content.columnconfigure((0, 1), weight=1, uniform="view")
        content.rowconfigure(1, weight=1)
        for column, label in enumerate(("SLAM 画面  ·  从首帧点云计时", f"实拍画面  ·  {path.name[:42]}")):
            ttk.Label(content, text=label, style="Shell.TLabel").grid(row=0, column=column, sticky="w", pady=(0, 6))
        self.slam_canvas = self.make_canvas(content, 0)
        self.clip_canvas = self.make_canvas(content, 1)
        self.clip_canvas.bind("<ButtonPress-1>", self.start_crop)
        self.clip_canvas.bind("<B1-Motion>", self.move_crop)
        self.clip_canvas.bind("<ButtonRelease-1>", self.end_crop)

        transport = ttk.Frame(content, padding=(8, 6))
        transport.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        ttk.Label(transport, text="共同时间轴 · SLAM 秒").pack(side="left", padx=(0, 10))
        self.common_time = tk.DoubleVar(value=slam_time or 0.)
        self.common_slider = ttk.Scale(transport, from_=0., to=max(.001, duration), variable=self.common_time)
        self.common_slider.pack(side="left", fill="x", expand=True)
        self.common_slider.bind("<ButtonRelease-1>", lambda _: self.seek_slam(self.common_time.get()))
        self.common_slider.bind("<KeyRelease>", lambda _: self.seek_slam(self.common_time.get()))
        self.linked = tk.BooleanVar(value=True)
        ttk.Checkbutton(transport, text="实拍跟随", variable=self.linked,
                        command=self.follow_slam).pack(side="left", padx=(12, 0))

        left = ttk.Frame(content, padding=8)
        left.grid(row=3, column=0, padx=(0, 5), sticky="nsew")
        self.bag_entry = tk.StringVar(value=f"{slam_time or 0:.3f}")
        ttk.Label(left, text="SLAM 秒").pack(side="left")
        bag_entry = ttk.Entry(left, textvariable=self.bag_entry, width=9)
        bag_entry.pack(side="left", padx=5)
        bag_entry.bind("<Return>", lambda _: self.seek_slam())
        self.render_button = ttk.Button(left, text="定位", command=self.seek_slam)
        self.render_button.pack(side="left")
        for label, direction in (("上一帧", -1), ("下一帧", 1)):
            ttk.Button(left, text=label, width=6, command=lambda d=direction: self.step_slam(d)).pack(side="left", padx=2)
        self.slam_label = tk.StringVar()
        ttk.Label(left, textvariable=self.slam_label).pack(side="right")

        right = ttk.Frame(content, padding=8)
        right.grid(row=3, column=1, padx=(5, 0), sticky="nsew")
        self.clip_entry = tk.StringVar(value="0.000")
        self.slider_time = tk.DoubleVar()
        self.slider = ttk.Scale(right, from_=0, to=self.reader.duration or 1, variable=self.slider_time)
        self.slider.pack(fill="x", pady=(0, 5))
        self.slider.bind("<ButtonRelease-1>", lambda _: self.manual_clip_seek(self.slider_time.get()))
        self.slider.bind("<KeyRelease>", lambda _: self.manual_clip_seek(self.slider_time.get()))
        nav = ttk.Frame(right)
        nav.pack(fill="x")
        for label, direction in (("上一帧", -1), ("下一帧", 1)):
            ttk.Button(nav, text=label, command=lambda step=direction: self.step(step)).pack(side="left", padx=(0, 4))
        entry = ttk.Entry(nav, textvariable=self.clip_entry, width=9)
        entry.pack(side="left", padx=4)
        entry.bind("<Return>", lambda _: self.manual_clip_seek())
        ttk.Button(nav, text="定位", command=self.manual_clip_seek).pack(side="left")

        trim = ttk.Frame(content, padding=8)
        trim.grid(row=4, column=1, sticky="ew", padx=(5, 0), pady=(6, 0))
        self.trim_in = tk.StringVar(value=f"{edit.trim_in:.6f}")
        self.trim_out = tk.StringVar(value="" if edit.trim_out is None else f"{edit.trim_out:.6f}")
        for label, variable, command in (("设入点", self.trim_in, self.set_in),
                                          ("设出点", self.trim_out, self.set_out)):
            ttk.Button(trim, text=label, width=6, command=command).pack(side="left", padx=(0, 4))
            ttk.Entry(trim, textvariable=variable, width=9).pack(side="left", padx=(0, 6))
        ttk.Button(trim, text="还原画幅", width=8, command=self.reset_crop).pack(side="right")

        self.advanced = ttk.Frame(content, padding=(8, 4))
        self.advanced.grid(row=6, column=0, columnspan=2, sticky="ew")
        self.advanced.grid_remove()
        align = ttk.Frame(self.advanced)
        align.pack(fill="x", pady=(0, 6))
        for index in range(2):
            ttk.Button(align, text=f"记录锚点 {index + 1}", width=9,
                       command=lambda i=index: self.mark(i)).pack(side="left", padx=(0, 4))
        ttk.Button(align, text="清除锚点", width=8, command=self.clear_anchors).pack(side="left")
        self.mid_button = ttk.Button(align, text="检查中段", width=8, command=self.seek_middle)
        self.mid_button.pack(side="left", padx=(4, 0))
        self.anchor_label = tk.StringVar()
        ttk.Label(content, textvariable=self.anchor_label, style="Shell.TLabel", wraplength=520).grid(
            row=5, column=0, sticky="w", pady=7)
        self.sync_label = tk.StringVar()
        ttk.Label(content, textvariable=self.sync_label,
                  style="Shell.TLabel").grid(row=5, column=1, sticky="w", padx=5)
        review = ttk.Frame(self.advanced)
        review.pack(fill="x")
        self.loop_button = ttk.Button(review, text="循环核对 ±0.6 s", command=self.toggle_loop)
        self.loop_button.pack(side="left", padx=(0, 8))
        ttk.Button(review, text="找对齐候选", command=self.find_candidates).pack(side="left", padx=(0, 8))
        self.candidate = tk.StringVar(value="选择候选动作")
        self.candidate_times = {}
        self.candidate_box = ttk.Combobox(review, width=27, textvariable=self.candidate, state="disabled")
        self.candidate_box.pack(side="left", padx=(0, 8))
        self.candidate_box.bind("<<ComboboxSelected>>", lambda _: self.choose_candidate())
        self.review_status = tk.StringVar()
        ttk.Label(review, textvariable=self.review_status, style="Quiet.TLabel").pack(side="left")
        footer = ttk.Frame(content, style="Shell.TFrame")
        footer.grid(row=7, column=0, columnspan=2, sticky="ew")
        self.show_advanced = tk.BooleanVar(value=False)
        ttk.Checkbutton(footer, text="精调", variable=self.show_advanced, command=self.toggle_advanced).pack(side="left")
        ttk.Label(footer, text="左右选中同一个动作，再对齐这两帧。", style="Shell.TLabel").pack(side="left", padx=8)
        ttk.Button(footer, text="对齐这两帧", style="Primary.TButton", command=self.confirm_pair).pack(side="right")
        ttk.Button(footer, text="应用", command=self.save).pack(side="right", padx=5)
        ttk.Button(footer, text="关闭", command=self.close).pack(side="right", padx=5)
        self.refresh_anchors()
        self.set_slam(slam_frame, slam_time)
        if self.clip_frame is None:
            self.seek_clip(edit.trim_in)

    def toggle_advanced(self):
        if self.show_advanced.get():
            self.advanced.grid()
        else:
            self.stop_loop()
            self.advanced.grid_remove()

    def confirm_pair(self):
        if self.mark(0):
            self.save()

    def make_canvas(self, content, column):
        canvas = tk.Canvas(content, bg="#0b1321", highlightthickness=0, width=320, height=240)
        canvas.grid(row=1, column=column, sticky="nsew", padx=(0, 5) if column == 0 else (5, 0))
        canvas.bind("<Configure>", lambda _: self.draw())
        return canvas

    @staticmethod
    def draw_image(canvas, image, placeholder):
        canvas.delete("all")
        width, height = canvas.winfo_width(), canvas.winfo_height()
        if image is None:
            canvas.create_text(width / 2, height / 2, text=placeholder, fill="#a6b5c9")
            return None
        scale = min(width / image.width, height / image.height)
        w, h = max(1, round(image.width * scale)), max(1, round(image.height * scale))
        x, y = (width - w) // 2, (height - h) // 2
        canvas.photo = ImageTk.PhotoImage(image.resize((w, h), Image.Resampling.LANCZOS))
        canvas.create_image(x, y, image=canvas.photo, anchor="nw")
        return x, y, w, h

    def draw(self):
        self.draw_image(self.slam_canvas, self.slam_frame, "输入 SLAM 时间后定位")
        self.clip_bounds = self.draw_image(self.clip_canvas, self.clip_frame, "实拍视频")
        self.draw_crop(self.edit.crop)

    def seek_slam(self, seconds=None):
        if self.aligning:
            return
        self.stop_loop()
        try:
            seconds = float(self.bag_entry.get()) if seconds is None else seconds
            if not isfinite(seconds) or seconds < 0:
                raise ValueError("SLAM 时间须为非负有限数值")
            self.request_slam(seconds)
            self.aligning = True
            self.render_button.state(["disabled"])
            self.common_slider.state(["disabled"])
            self.slam_label.set("定位中…")
        except ValueError as error:
            messagebox.showerror("检查时间", str(error), parent=self.window)

    def finish_seek(self):
        self.aligning = False
        self.render_button.state(["!disabled"])
        self.common_slider.state(["!disabled"])
        self.slam_label.set("" if self.slam_time is None else f"当前 {self.slam_time:.3f} s")

    def set_slam(self, frame, elapsed, duration=None):
        self.slam_frame, self.slam_time = frame, elapsed
        if duration is not None:
            self.common_slider.configure(to=max(.001, duration))
        if elapsed is not None:
            self.common_time.set(elapsed)
            self.bag_entry.set(f"{elapsed:.6f}")
        self.finish_seek()
        self.follow_slam()
        self.draw()

    def follow_slam(self):
        if self.linked.get() and self.slam_time is not None:
            target = self.edit.video_time(self.slam_time)
            if target < 0 or (self.reader.duration is not None and target >= self.reader.duration):
                self.clip_frame = None
                self.sync_label.set("共同时间轴上的该时刻超出视频范围")
                self.draw()
            else:
                self.seek_clip(target)

    def seek_middle(self):
        if len(self.edit.anchors) == 2:
            self.linked.set(True)
            self.seek_slam(sum(bag for bag, _ in self.edit.anchors) / 2)

    def step_slam(self, direction):
        if self.slam_time is None:
            self.seek_slam(0.)
        else:
            cap = float(self.common_slider.cget("to"))
            self.seek_slam(max(0., min(cap, self.slam_time + direction/self.fps)))

    def find_candidates(self):
        self.stop_loop()
        try:
            self.request_candidates()
        except ValueError as error:
            self.review_status.set(str(error))
        else:
            self.review_status.set("正在扫描 SLAM 位姿…")

    def show_candidates(self, candidates):
        self.candidate_times = {f"{time:.3f} s  ·  {label}": time for time, label in candidates}
        self.candidate_box.configure(values=tuple(self.candidate_times),
                                     state="readonly" if candidates else "disabled")
        self.candidate.set("选择候选动作")
        self.review_status.set("请在实拍中确认同一动作" if candidates else "所选区间暂无明显动作")

    def choose_candidate(self):
        time = self.candidate_times.get(self.candidate.get())
        if time is not None:
            self.linked.set(True)
            self.seek_slam(time)

    def toggle_loop(self):
        if self.loop_job is not None:
            self.stop_loop()
            return
        if self.aligning or self.slam_time is None:
            self.review_status.set("先定位一帧 SLAM 画面")
            return
        start = max(0., self.slam_time-.6)
        end = min(float(self.common_slider.cget("to")), self.slam_time+.6)
        try:
            self.request_loop(start, end)
        except ValueError as error:
            self.review_status.set(str(error))
        else:
            self.review_status.set("正在准备短循环…")

    def loop_ready(self, frames):
        self.loop_origin = self.slam_frame, self.slam_time, self.clip_time, self.linked.get()
        self.linked.set(True)
        self.loop_button.configure(text="停止循环")
        self.review_status.set("同步循环预览")
        pictures = [(Image.fromarray(frame), time) for frame, time in frames]

        def advance(i):
            self.set_slam(*pictures[i])
            self.loop_job = self.window.after(100, lambda: advance((i+1) % len(pictures)))

        advance(0)

    def stop_loop(self):
        if self.loop_job is None:
            return
        self.window.after_cancel(self.loop_job)
        self.loop_job = None
        self.loop_button.configure(text="循环核对 ±0.6 s")
        frame, time, clip_time, linked = self.loop_origin
        self.linked.set(linked)
        self.set_slam(frame, time)
        if not linked:
            self.seek_clip(clip_time)
        self.review_status.set("")

    def manual_clip_seek(self, seconds=None):
        self.stop_loop()
        self.linked.set(False)
        self.seek_clip(seconds)

    def seek_clip(self, seconds=None):
        try:
            value = float(self.clip_entry.get()) if seconds is None else seconds
            if not isfinite(value) or value < 0:
                raise ValueError("视频时间须为非负有限数值")
            if self.reader.duration is not None:
                value = min(value, max(0., self.reader.duration - self.reader.tick))
            frame = self.reader.select(value)
            if frame is None:
                raise ValueError("该时刻没有视频帧")
            self.clip_time = self.reader.time(frame)
            self.clip_entry.set(f"{self.clip_time:.6f}")
            self.slider_time.set(self.clip_time)
            self.clip_frame = self.reader.image(frame)
            self.refresh_sync()
            self.draw()
        except (ValueError, OSError, av.FFmpegError) as error:
            messagebox.showerror("无法定位视频", str(error), parent=self.window)

    def refresh_sync(self):
        if self.slam_time is not None and self.clip_frame is not None:
            mapped = self.edit.video_time(self.slam_time)
            self.sync_label.set(f"对齐位置 {mapped:.3f} s  ·  当前视频帧偏差 {(self.clip_time-mapped)*1000:+.0f} ms")

    def step(self, direction):
        self.stop_loop()
        self.linked.set(False)
        if self.clip_frame is None:
            self.seek_clip(self.edit.trim_in)
        elif direction > 0:
            if self.reader.following is not None:
                self.seek_clip(self.reader.time(self.reader.following))
        elif self.clip_time > 0:
            self.seek_clip(max(0., self.clip_time - self.reader.tick * .75))

    def set_in(self):
        self.trim_in.set(f"{self.clip_time:.6f}")

    def set_out(self):
        end = self.reader.time(self.reader.following) if self.reader.following is not None else self.reader.duration
        self.trim_out.set("" if end is None else f"{end:.6f}")

    def mark(self, index):
        self.stop_loop()
        if self.render_button.instate(["disabled"]):
            return
        if self.slam_time is None or self.clip_frame is None:
            messagebox.showinfo("需要两侧画面", "先定位两侧画面，再记录时间。", parent=self.window)
            return
        try:
            anchors = list(self.edit.anchors)
            pair = (self.slam_time, self.clip_time)
            if index < len(anchors):
                anchors[index] = pair
            elif index == len(anchors):
                anchors.append(pair)
            else:
                raise ValueError("请先记录第一个锚点")
            self.edit = replace(self.edit, anchors=tuple(anchors))
            self.refresh_anchors()
            return True
        except ValueError as error:
            messagebox.showerror("检查锚点", str(error), parent=self.window)

    def refresh_anchors(self):
        pairs = [f"{i}  {bag:.3f} s ↔ {video:.3f} s" for i, (bag, video) in enumerate(self.edit.anchors, 1)]
        if len(pairs) == 2:
            pairs.append(f"速度系数 {self.edit.rate:.6f}×")
        self.anchor_label.set("  ·  ".join(pairs) if pairs else "手动偏移模式  ·  尚无时间锚点")
        self.mid_button.state(["!disabled" if len(self.edit.anchors) == 2 else "disabled"])
        self.refresh_sync()

    def clear_anchors(self):
        self.edit = replace(self.edit, anchors=())
        self.refresh_anchors()

    def normalized(self, event):
        x, y, w, h = self.clip_bounds
        return min(1., max(0., (event.x - x) / w)), min(1., max(0., (event.y - y) / h))

    def start_crop(self, event):
        self.stop_loop()
        if self.clip_bounds is not None:
            self.drag_start = self.normalized(event)

    def crop_box(self, event):
        x0, y0 = self.drag_start
        x1, y1 = self.normalized(event)
        return min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)

    def move_crop(self, event):
        if self.drag_start is not None:
            self.draw_crop(self.crop_box(event))

    def end_crop(self, event):
        if self.drag_start is not None:
            x0, y0, x1, y1 = self.crop_box(event)
            self.drag_start = None
            if x1 - x0 >= .01 and y1 - y0 >= .01:
                self.edit = replace(self.edit, crop=(x0, y0, x1, y1))
            self.draw_crop(self.edit.crop)

    def draw_crop(self, box):
        self.clip_canvas.delete("crop")
        if self.clip_bounds is None or box == FULL_FRAME:
            return
        x, y, w, h = self.clip_bounds
        left, top, right, bottom = box
        self.clip_canvas.create_rectangle(x + left*w, y + top*h, x + right*w, y + bottom*h,
                                          width=2, outline="#62e3f4", tags="crop")

    def reset_crop(self):
        self.edit = replace(self.edit, crop=FULL_FRAME)
        self.draw_crop(FULL_FRAME)

    def save(self):
        try:
            edit = replace(self.edit, trim_in=float(self.trim_in.get()),
                           trim_out=float(self.trim_out.get()) if self.trim_out.get().strip() else None)
        except ValueError as error:
            messagebox.showerror("检查裁剪范围", str(error), parent=self.window)
            return
        self.apply(edit)
        self.close()

    def close(self):
        if self.loop_job is not None:
            self.window.after_cancel(self.loop_job)
        self.reader.close()
        self.window.destroy()
        self.on_close()
