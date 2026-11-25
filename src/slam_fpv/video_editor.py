"""Side-by-side timestamp alignment and non-destructive footage trimming."""

from dataclasses import replace
from math import isfinite

import av
import tkinter as tk
from tkinter import messagebox, ttk

from PIL import Image, ImageTk

from .footage import FULL_FRAME, VideoEdit, VideoReader


class VideoEditor:
    def __init__(self, root, path, edit, slam_frame, slam_time, request_slam, apply):
        self.reader = VideoReader(path)
        self.edit = edit
        self.request_slam = request_slam
        self.apply = apply
        self.slam_frame, self.slam_time = slam_frame, slam_time
        self.clip_frame = None
        self.clip_time = 0.
        self.drag_start = None
        self.clip_bounds = None

        self.window = tk.Toplevel(root)
        self.window.title("实拍视频  ·  裁剪与时间对齐")
        self.window.geometry("1060x680")
        self.window.minsize(880, 550)
        self.window.transient(root)
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        self.window.grab_set()

        content = ttk.Frame(self.window, padding=14, style="Shell.TFrame")
        content.pack(fill="both", expand=True)
        content.columnconfigure((0, 1), weight=1, uniform="view")
        content.rowconfigure(1, weight=1)
        for column, label in enumerate(("SLAM 画面  ·  从首帧点云计时", "实拍画面  ·  视频时间戳")):
            ttk.Label(content, text=label, style="Shell.TLabel").grid(row=0, column=column, sticky="w", pady=(0, 6))
        self.slam_canvas = self.make_canvas(content, 0)
        self.clip_canvas = self.make_canvas(content, 1)
        self.clip_canvas.bind("<ButtonPress-1>", self.start_crop)
        self.clip_canvas.bind("<B1-Motion>", self.move_crop)
        self.clip_canvas.bind("<ButtonRelease-1>", self.end_crop)

        left = ttk.Frame(content, padding=8)
        left.grid(row=2, column=0, padx=(0, 5), sticky="nsew")
        self.bag_entry = tk.StringVar(value=f"{slam_time or 0:.3f}")
        ttk.Label(left, text="SLAM 秒").pack(side="left")
        ttk.Entry(left, textvariable=self.bag_entry, width=9).pack(side="left", padx=5)
        self.render_button = ttk.Button(left, text="定位", command=self.seek_slam)
        self.render_button.pack(side="left")
        self.slam_label = tk.StringVar()
        ttk.Label(left, textvariable=self.slam_label).pack(side="right")

        right = ttk.Frame(content, padding=8)
        right.grid(row=2, column=1, padx=(5, 0), sticky="nsew")
        self.clip_entry = tk.StringVar(value="0.000")
        self.slider_time = tk.DoubleVar()
        self.slider = ttk.Scale(right, from_=0, to=self.reader.duration or 1, variable=self.slider_time)
        self.slider.pack(fill="x", pady=(0, 5))
        self.slider.bind("<ButtonRelease-1>", lambda _: self.seek_clip(self.slider_time.get()))
        self.slider.bind("<KeyRelease>", lambda _: self.seek_clip(self.slider_time.get()))
        nav = ttk.Frame(right)
        nav.pack(fill="x")
        for label, direction in (("上一帧", -1), ("下一帧", 1)):
            ttk.Button(nav, text=label, command=lambda step=direction: self.step(step)).pack(side="left", padx=(0, 4))
        entry = ttk.Entry(nav, textvariable=self.clip_entry, width=9)
        entry.pack(side="left", padx=4)
        entry.bind("<Return>", lambda _: self.seek_clip())
        ttk.Button(nav, text="定位", command=self.seek_clip).pack(side="left")

        trim = ttk.Frame(content, padding=8)
        trim.grid(row=3, column=1, sticky="ew", padx=(5, 0), pady=(6, 0))
        self.trim_in = tk.StringVar(value=f"{edit.trim_in:.6f}")
        self.trim_out = tk.StringVar(value="" if edit.trim_out is None else f"{edit.trim_out:.6f}")
        for label, variable, command in (("设入点", self.trim_in, self.set_in),
                                          ("设出点", self.trim_out, self.set_out)):
            ttk.Button(trim, text=label, command=command).pack(side="left", padx=(0, 4))
            ttk.Entry(trim, textvariable=variable, width=9).pack(side="left", padx=(0, 8))
        ttk.Button(trim, text="还原画幅", command=self.reset_crop).pack(side="right")

        align = ttk.Frame(content, padding=8)
        align.grid(row=3, column=0, sticky="ew", padx=(0, 5), pady=(6, 0))
        for index in range(2):
            ttk.Button(align, text=f"记录锚点 {index + 1}", command=lambda i=index: self.mark(i)).pack(side="left", padx=(0, 4))
        ttk.Button(align, text="清除锚点", command=self.clear_anchors).pack(side="left")
        self.anchor_label = tk.StringVar()
        ttk.Label(content, textvariable=self.anchor_label, style="Shell.TLabel", wraplength=520).grid(
            row=4, column=0, sticky="w", pady=7)
        ttk.Label(content, text="拖框保留画面区域  ·  入点和出点只控制出现区间",
                  style="Shell.TLabel").grid(row=4, column=1, sticky="w", padx=5)
        footer = ttk.Frame(content, style="Shell.TFrame")
        footer.grid(row=5, column=0, columnspan=2, sticky="ew")
        ttk.Label(footer, text="把两侧定位到同一动作，各记录一次。第二个锚点用于校正长片时钟漂移。",
                  style="Shell.TLabel").pack(side="left")
        ttk.Button(footer, text="应用", style="Primary.TButton", command=self.save).pack(side="right")
        ttk.Button(footer, text="关闭", command=self.close).pack(side="right", padx=5)
        self.refresh_anchors()
        self.set_slam(slam_frame, slam_time)
        self.seek_clip(max(0., edit.video_time(slam_time or 0)))

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

    def seek_slam(self):
        try:
            seconds = float(self.bag_entry.get())
            if not isfinite(seconds) or seconds < 0:
                raise ValueError("SLAM 时间须为非负有限数值")
            self.request_slam(seconds)
            self.render_button.state(["disabled"])
            self.slam_label.set("定位中…")
        except ValueError as error:
            messagebox.showerror("检查时间", str(error), parent=self.window)

    def set_slam(self, frame, elapsed):
        self.slam_frame, self.slam_time = frame, elapsed
        self.slam_label.set("" if elapsed is None else f"当前 {elapsed:.3f} s")
        self.render_button.state(["!disabled"])
        self.draw()

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
            self.draw()
        except (ValueError, OSError, av.FFmpegError) as error:
            messagebox.showerror("无法定位视频", str(error), parent=self.window)

    def step(self, direction):
        if direction > 0:
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
        if self.render_button.instate(["disabled"]):
            return
        if self.slam_time is None:
            messagebox.showinfo("需要 SLAM 帧", "先定位 SLAM 画面，再记录两侧时间。", parent=self.window)
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
        except ValueError as error:
            messagebox.showerror("检查锚点", str(error), parent=self.window)

    def refresh_anchors(self):
        pairs = [f"{i}  {bag:.3f} s ↔ {video:.3f} s" for i, (bag, video) in enumerate(self.edit.anchors, 1)]
        if len(pairs) == 2:
            pairs.append(f"速度系数 {self.edit.rate:.6f}×")
        self.anchor_label.set("  ·  ".join(pairs) if pairs else "手动偏移模式  ·  尚无时间锚点")

    def clear_anchors(self):
        self.edit = replace(self.edit, anchors=())
        self.refresh_anchors()

    def normalized(self, event):
        x, y, w, h = self.clip_bounds
        return min(1., max(0., (event.x - x) / w)), min(1., max(0., (event.y - y) / h))

    def start_crop(self, event):
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
        self.reader.close()
        self.window.destroy()
