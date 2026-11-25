"""Desktop controls for offline bag rendering and optional flight footage."""

from argparse import ArgumentParser
from dataclasses import replace
import json
import os
import sys
from math import isfinite
from pathlib import Path
from queue import Empty, Queue
from tempfile import TemporaryDirectory
from threading import Event, Thread
import tkinter as tk
import av
from tkinter import colorchooser, filedialog, messagebox, ttk

from PIL import Image, ImageTk

from .cli import open_source, parser, render, validate
from .footage import FULL_FRAME, VideoEdit, video_rect
from .video_editor import VideoEditor
from .sources import inspect_bag


class FormParser(ArgumentParser):
    def error(self, message):
        raise ValueError(message)


class Cancelled(Exception):
    def __str__(self):
        return "Rendering cancelled"


class RendererApp:
    def __init__(self, root):
        # The POSIX FFmpeg pre-exec callback can deadlock after OpenGL creates threads.
        if os.name != "nt":
            os.environ["IMAGEIO_FFMPEG_NO_PREVENT_SIGINT"] = "1"
        self.root = root
        self.events = Queue()
        self.cancel_event = Event()
        self.busy = False
        self.closing = False
        self.values = {}
        self.position = (.035, .87)
        self.frame = self.video_frame = self.elapsed = None
        self.video_edit = VideoEdit()
        self.video_editor = None
        self.drag_offset = None
        self.layer_bounds = None
        self.view_bounds = None
        self.bag_path = None
        self.demo_selected = False
        self.disabled_controls = []

        root.title("SLAM FPV Renderer")
        root.geometry("1200x750")
        root.minsize(920, 620)
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.configure_style()

        workspace = ttk.Frame(root, padding=18, style="Shell.TFrame")
        workspace.pack(fill="both", expand=True)
        workspace.columnconfigure(1, weight=1)
        workspace.rowconfigure(1, weight=1)
        ttk.Label(workspace, text="SLAM FPV Renderer", style="Title.TLabel").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 14))

        self.form = ttk.Frame(workspace, style="Shell.TFrame")
        self.form.grid(row=1, column=0, sticky="nsew", padx=(0, 16))
        self.make_form()

        viewer = ttk.Frame(workspace, padding=12)
        viewer.grid(row=1, column=1, sticky="nsew")
        self.preview_label = ttk.Label(viewer, text="画面预览", style="PanelTitle.TLabel")
        self.preview_label.pack(anchor="w", pady=(0, 8))
        self.canvas = tk.Canvas(viewer, bg="#0b1321", highlightthickness=0, width=460, height=400)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<Configure>", lambda _: self.draw_preview())
        self.canvas.bind("<ButtonPress-1>", self.start_drag)
        self.canvas.bind("<B1-Motion>", self.move_video)
        self.canvas.bind("<ButtonRelease-1>", lambda _: self.end_drag())

        bar = ttk.Frame(workspace, style="Shell.TFrame")
        bar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        bar.columnconfigure(0, weight=1)
        self.status = tk.StringVar(value="选择 ROS1 bag 开始，或打开模拟预览")
        ttk.Label(bar, textvariable=self.status, style="Shell.TLabel", wraplength=690).grid(
            row=0, column=0, sticky="w")
        self.progress = ttk.Progressbar(bar, length=130, maximum=100)
        self.progress.grid(row=0, column=1, padx=12)
        self.cancel_button = ttk.Button(bar, text="取消", command=self.cancel, state="disabled")
        self.cancel_button.grid(row=0, column=2)
        root.after(60, self.poll)

    def configure_style(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        font = "Microsoft YaHei UI" if sys.platform == "win32" else "Noto Sans CJK SC"
        style.configure(".", font=(font, 10), background="#ffffff", foreground="#18263b")
        style.configure("Shell.TFrame", background="#edf1f6")
        style.configure("Shell.TLabel", background="#edf1f6", foreground="#46566c")
        style.configure("Title.TLabel", background="#edf1f6", font=(font, 20, "bold"))
        style.configure("PanelTitle.TLabel", font=(font, 11, "bold"))
        style.configure("TButton", padding=(9, 6), background="#e7edf6", borderwidth=0)
        style.configure("Primary.TButton", background="#2558a6", foreground="white")
        style.map("Primary.TButton", background=[("disabled", "#8194ad"), ("active", "#1b4689")])
        style.configure("TNotebook", background="#edf1f6", borderwidth=0)
        style.configure("TNotebook.Tab", padding=(10, 7), background="#dce5f0")
        style.map("TNotebook.Tab", background=[("selected", "#ffffff")])
        style.configure("TEntry", padding=4)
        style.configure("TCombobox", padding=4)
        style.configure("TProgressbar", background="#2558a6")

    def make_form(self):
        nav = ttk.Notebook(self.form)
        nav.pack(fill="both", expand=True)
        source, quality, camera, footage = (ttk.Frame(nav, padding=12) for _ in range(4))
        for panel, name in zip((source, quality, camera, footage), ("数据源", "画质与颜色", "相机", "实拍视频")):
            nav.add(panel, text=name)
            panel.columnconfigure(1, weight=1)

        self.bag_name = tk.StringVar(value="未选择 ROS1 bag")
        ttk.Button(source, text="打开 .bag", command=self.select_bag).grid(
            row=0, column=0, sticky="w", pady=(0, 6))
        ttk.Button(source, text="使用模拟数据", command=self.use_demo).grid(
            row=0, column=1, sticky="w", pady=(0, 6))
        ttk.Label(source, textvariable=self.bag_name, wraplength=310).grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(0, 12))
        self.cloud_topics = self.field(source, 2, "点云话题", "cloud_topic", "", ())
        self.cloud_topics.bind("<<ComboboxSelected>>", self.reset_alignment)
        self.field(source, 3, "位姿来源", "pose_source", "Odometry", ("Odometry", "TF"))
        self.odom_topics = self.field(source, 4, "里程计话题", "odom_topic", "", ())
        self.field(source, 5, "世界坐标系", "world", "camera_init")
        self.field(source, 6, "机体坐标系", "body", "body")
        self.field(source, 7, "最大插值间隔 (s)", "max_gap", ".5")
        self.field(source, 8, "点云更新方式", "cloud_mode", "累积扫描", ("累积扫描", "替换整图"))
        self.file_row(source, 9, "外参 JSON", "extrinsics", (("JSON", "*.json"),), "可选")
        self.field(source, 10, "导出起点 (s)", "start", "0")
        self.field(source, 11, "导出时长 (s)", "seconds", "6")
        self.field(source, 12, "片段内预览位置 (s)", "preview_offset", "1")
        self.values["pose_source"].trace_add("write", lambda *_: self.sync_odometry())

        self.field(quality, 0, "分辨率", "resolution", "1920 × 1080",
                   ("1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160"))
        self.field(quality, 1, "帧率", "fps", "60", ("60", "30", "24"))
        self.field(quality, 2, "超采样", "supersample", "2", ("1", "2"))
        self.field(quality, 3, "MP4 质量 (CRF/CQ)", "crf", "15", ("12", "15", "18", "23"))
        self.field(quality, 4, "体素大小 (m)", "voxel", ".05")
        self.field(quality, 5, "点数上限", "max_points", "400000")
        self.field(quality, 6, "点大小 (px)", "point_size", "2.6")
        self.field(quality, 7, "点云着色", "color", "原始 RGB", ("原始 RGB", "按高度", "自选纯色"))
        self.color_field(quality, 8, "点云纯色", "cloud_color", "#70d9ff")
        self.color_field(quality, 9, "轨迹颜色", "trajectory_color", "#ffba2e")
        self.field(quality, 10, "轨迹线宽 (px)", "trajectory_width", "3")
        self.edl = tk.BooleanVar(value=True)
        ttk.Checkbutton(quality, text="启用 EDL 深度明暗", variable=self.edl).grid(
            row=11, column=0, columnspan=2, sticky="w", pady=6)
        self.field(quality, 12, "MP4 编码器", "encoder", "自动", ("自动", "CPU", "NVIDIA NVENC"))

        for row, (label, key, value) in enumerate((
            ("跟随距离 (m)", "distance", "3.8"),
            ("主视角高度 (m)", "camera_height", "1.35"),
            ("主视角横向偏移 (m)", "side", "0"),
            ("垂直视场角 (°)", "fov", "65"),
            ("高位斜视距离 (m)", "inset_distance", "10"),
            ("高位斜视高度 (m)", "inset_height", "6.7"),
            ("三轴长度 (m)", "axis", ".48"),
            ("三轴最小显示 (px)", "axis_pixels", "28"),
        )):
            self.field(camera, row, label, key, value)

        self.file_row(footage, 0, "实拍视频", "video",
                      (("视频", "*.mp4 *.mov *.mkv *.avi *.m4v"), ("所有文件", "*")), "可选")
        self.field(footage, 1, "视频起点偏移 (s)", "video_offset", "0")
        self.field(footage, 2, "画中画宽度 (%)", "video_width", "30")
        self.values["video_width"].trace_add("write", lambda *_: self.draw_preview())
        ttk.Button(footage, text="裁剪与时间对齐", command=self.open_video_editor,
                   style="Primary.TButton").grid(row=3, column=0, columnspan=2, sticky="ew", pady=(14, 6))
        self.edit_summary = tk.StringVar(value="全片 · 原画幅 · 手动偏移")
        ttk.Label(footage, textvariable=self.edit_summary, wraplength=300).grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(0, 8))
        ttk.Button(footage, text="重置视频位置", command=self.reset_position).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=5)
        ttk.Label(footage, text="生成预览后，可直接拖动视频小窗。", wraplength=290).grid(
            row=6, column=0, columnspan=2, sticky="w")

        actions = ttk.Frame(self.form, style="Shell.TFrame")
        actions.pack(fill="x", pady=(10, 0))
        ttk.Button(actions, text="生成预览", command=lambda: self.start_render(True)).pack(side="left")
        ttk.Button(actions, text="导出 MP4 / PNG", style="Primary.TButton",
                   command=lambda: self.start_render(False)).pack(side="right")
        self.sync_odometry()

    def field(self, panel, row, label, name, value, choices=None):
        variable = self.values[name] = tk.StringVar(value=value)
        ttk.Label(panel, text=label).grid(row=row, column=0, sticky="w", padx=(0, 7), pady=3)
        widget = (ttk.Entry(panel, textvariable=variable, width=18) if choices is None else
                  ttk.Combobox(panel, textvariable=variable, values=choices, width=18, state="readonly"))
        widget.grid(row=row, column=1, sticky="ew", pady=3)
        return widget

    def file_row(self, panel, row, label, name, filetypes, placeholder):
        self.values[name] = tk.StringVar()
        display = tk.StringVar(value=placeholder)

        def set_path(path):
            self.values[name].set(path)
            display.set(Path(path).name if path else placeholder)
            if name == "video":
                self.apply_video_edit(VideoEdit())
                self.values["video_offset"].set("0")
                if path:
                    self.status.set("已导入视频，可裁剪并与 SLAM 对齐")

        def browse():
            path = filedialog.askopenfilename(parent=self.root, title=label, filetypes=filetypes)
            if path:
                set_path(path)

        ttk.Button(panel, text=label, command=browse).grid(row=row, column=0, sticky="w", pady=4)
        group = ttk.Frame(panel)
        group.grid(row=row, column=1, sticky="ew")
        ttk.Label(group, textvariable=display, width=13).pack(side="left", fill="x", expand=True)
        ttk.Button(group, text="清除", command=lambda: set_path("")).pack(side="right")

    def color_field(self, panel, row, label, name, default):
        variable = self.values[name] = tk.StringVar(value=default)
        ttk.Label(panel, text=label).grid(row=row, column=0, sticky="w", pady=4)
        group = ttk.Frame(panel)
        group.grid(row=row, column=1, sticky="ew")
        swatch = tk.Label(group, width=3, bg=default)
        swatch.pack(side="left", padx=(0, 6))

        def pick():
            _, color = colorchooser.askcolor(variable.get(), parent=self.root, title=label)
            if color:
                variable.set(color)
                swatch.configure(bg=color)
                if name == "cloud_color":
                    self.values["color"].set("自选纯色")

        ttk.Button(group, textvariable=variable, command=pick).pack(side="left")

    def select_bag(self):
        path = filedialog.askopenfilename(parent=self.root, filetypes=(("ROS1 bag", "*.bag"),))
        if path:
            path = Path(path)
            self.run_job(lambda: ("topics", (path, inspect_bag(path))), "正在读取 bag 话题…")

    def use_demo(self):
        if not self.demo_selected:
            self.reset_alignment()
        self.bag_path = None
        self.demo_selected = True
        self.bag_name.set("模拟点云与轨迹")
        self.start_render(True)

    def sync_odometry(self):
        state = "readonly" if self.values["pose_source"].get() == "Odometry" else "disabled"
        self.odom_topics.configure(state=state)

    def set_topics(self, path, data):
        self.reset_alignment()
        self.bag_path = path
        self.demo_selected = False
        self.bag_name.set(path.name)
        topics = data["topics"]
        clouds = sorted({row["topic"] for row in topics if row["type"] == "sensor_msgs/msg/PointCloud2"})
        odometry = sorted({row["topic"] for row in topics if row["type"] == "nav_msgs/msg/Odometry"})
        for widget, choices, fallback in ((self.cloud_topics, clouds, "/cloud_registered"),
                                          (self.odom_topics, odometry, "/Odometry")):
            widget.configure(values=choices)
            widget.set(fallback if fallback in choices else (choices[0] if choices else ""))
        self.values["pose_source"].set("Odometry" if odometry else "TF")
        self.status.set(f"已读取 {len(clouds)} 个点云话题，{len(odometry)} 个里程计话题")

    def reset_alignment(self, _event=None):
        self.frame = self.elapsed = None
        self.preview_label.configure(text="画面预览")
        self.apply_video_edit(replace(self.video_edit, anchors=()))

    def apply_video_edit(self, edit):
        self.video_edit = edit
        clip = "全片"
        if edit.trim_in or edit.trim_out is not None:
            end = "片尾" if edit.trim_out is None else f"{edit.trim_out:.3f} s"
            clip = f"{edit.trim_in:.3f} s – {end}"
        crop = "原画幅" if edit.crop == FULL_FRAME else "已裁剪画幅"
        align = f"{len(edit.anchors)} 个锚点" if edit.anchors else "手动偏移"
        self.edit_summary.set(f"{clip}  ·  {crop}  ·  {align}")
        self.video_frame = None
        self.draw_preview()
        self.status.set("视频设置已更新，生成预览确认效果")

    def open_video_editor(self):
        path = self.values["video"].get().strip()
        if not path:
            messagebox.showinfo("实拍视频", "请先打开实拍视频文件。", parent=self.root)
            return
        try:
            edit = replace(self.video_edit, offset=float(self.values["video_offset"].get()))
            self.video_editor = VideoEditor(self.root, path, edit, self.frame, self.elapsed,
                                            self.request_alignment_frame, self.apply_video_edit)
        except (ValueError, OSError, av.FFmpegError) as error:
            messagebox.showerror("无法打开视频", str(error), parent=self.root)

    def request_alignment_frame(self, seconds):
        if self.busy:
            raise ValueError("请等待当前预览完成")
        args = self.build_args(Path("preview.png"))
        args.video = None
        args.start = 0
        args.seconds = max(args.seconds, seconds + 1 / args.fps)
        args.width, args.height, args.supersample = 960, 540, 1

        def work():
            result = []
            source = open_source(args, self.check_cancel)
            render(args, source, preview=lambda *frame: result.append(frame),
                   preview_offset=seconds, check_cancel=self.check_cancel)
            return "alignment", result[0]

        self.run_job(work, "正在定位 SLAM 帧…")

    def build_args(self, destination):
        if self.bag_path is None and not self.demo_selected:
            raise ValueError("请先打开 ROS1 bag 或选择使用模拟数据。")
        values = {key: variable.get().strip() for key, variable in self.values.items()}
        width, height = values["resolution"].split(" × ")
        args = ["demo"] if self.bag_path is None else ["bag", str(self.bag_path)]
        args += ["--output", str(destination), "--width", width, "--height", height]
        keys = ("start", "seconds", "fps", "supersample", "crf", "voxel", "max_points", "point_size",
                "cloud_color", "trajectory_color", "trajectory_width", "distance", "camera_height",
                "side", "fov", "inset_distance", "inset_height", "axis", "axis_pixels", "video_offset")
        if self.bag_path is not None:
            if not values["cloud_topic"] or not values["world"] or not values["body"]:
                raise ValueError("请选择点云话题并填写世界、机体坐标系。")
            if values["pose_source"] == "Odometry" and not values["odom_topic"]:
                raise ValueError("请选择里程计话题，或将位姿来源改为 TF。")
            keys += ("cloud_topic", "odom_topic", "world", "body", "max_gap")
            args += ["--pose-source", "odom" if values["pose_source"] == "Odometry" else "tf"]
            if values["extrinsics"]:
                keys += ("extrinsics",)
        if values["video"]:
            keys += ("video",)
        for key in keys:
            args += ["--" + key.replace("_", "-") + "=" + values[key]]
        color = {"原始 RGB": "rgb", "按高度": "height", "自选纯色": "solid"}[values["color"]]
        mode = "accumulate" if values["cloud_mode"] == "累积扫描" else "replace"
        encoder = {"自动": "auto", "CPU": "cpu", "NVIDIA NVENC": "nvenc"}[values["encoder"]]
        args += ["--color", color, "--cloud-mode", mode, "--encoder", encoder]
        if values["video"]:
            edit = self.video_edit
            args += ["--video-in", str(edit.trim_in), "--video-crop", *map(str, edit.crop)]
            if edit.trim_out is not None:
                args += ["--video-out", str(edit.trim_out)]
            for pair in edit.anchors:
                args += ["--sync-anchor", *map(str, pair)]
        args += ["--video-width", str(float(values["video_width"]) / 100),
                 "--video-position", *map(str, self.position)]
        if not self.edl.get():
            args.append("--no-edl")
        settings = parser(FormParser).parse_args(args)
        validate(settings)
        return settings

    def start_render(self, preview):
        destination = Path("preview.png")
        if not preview:
            path = filedialog.asksaveasfilename(parent=self.root, defaultextension=".mp4", initialfile="slam-fpv",
                                               filetypes=(("MP4 视频", "*.mp4"), ("PNG 图片", "*.png")))
            if not path:
                return
            destination = Path(path)
            report = destination.with_suffix(".json")
            if report.exists() and not messagebox.askyesno("覆盖报告", f"覆盖现有报告 {report.name}？", parent=self.root):
                return
        try:
            args = self.build_args(destination)
            offset = float(self.values["preview_offset"].get()) if preview else 0
            if not isfinite(offset) or offset < 0:
                raise ValueError("片段内预览位置须为非负有限数值。")
        except ValueError as error:
            messagebox.showerror("检查输入", str(error), parent=self.root)
            return

        def work():
            source = open_source(args, self.check_cancel)
            if preview:
                result = []
                render(args, source, preview=lambda *frame: result.append(frame),
                       preview_offset=offset, check_cancel=self.check_cancel)
                return "preview", result[0]
            destination.parent.mkdir(parents=True, exist_ok=True)
            with TemporaryDirectory(prefix=".slam-fpv-", dir=destination.parent) as staging:
                temporary_output = Path(staging) / destination.name
                render(args, source, staging=temporary_output,
                       progress=lambda done, total: self.events.put(("progress", (done, total))),
                       check_cancel=self.check_cancel)
                self.check_cancel()
                if not temporary_output.is_file() or not temporary_output.stat().st_size:
                    raise ValueError("编码器未写入成品，请检查 FFmpeg 或改用较低分辨率。")
                report = temporary_output.with_suffix(".json")
                summary = json.loads(report.read_text(encoding="utf-8"))
                codec = summary["encoder"]
                if codec is not None:
                    with av.open(str(temporary_output)) as video:
                        if not video.streams.video or video.streams.video[0].frames != summary["frames"]:
                            raise ValueError("导出视频的帧数与渲染记录不符。")
                temporary_output.replace(destination)
                report.replace(destination.with_suffix(".json"))
            return "saved", (destination, codec)

        self.run_job(work, "正在准备预览…" if preview else "正在准备导出…")

    def run_job(self, action, text):
        self.busy = True
        self.cancel_event.clear()
        self.disable_form(self.form)
        self.cancel_button.state(["!disabled"])
        self.status.set(text)
        self.progress.configure(mode="indeterminate")
        self.progress.start(12)

        def run():
            try:
                result = action()
                if result[0] != "saved":
                    self.check_cancel()
                self.events.put(result)
            except Cancelled:
                self.events.put(("cancelled", None))
            except Exception as error:
                self.events.put(("error", str(error)))

        Thread(target=run, daemon=True).start()

    def disable_form(self, parent):
        for child in parent.winfo_children():
            if isinstance(child, (ttk.Button, ttk.Entry, ttk.Checkbutton)) and not child.instate(["disabled"]):
                child.state(["disabled"])
                self.disabled_controls.append(child)
            self.disable_form(child)

    def check_cancel(self):
        if self.cancel_event.is_set():
            raise Cancelled()

    def cancel(self):
        self.cancel_event.set()
        self.cancel_button.state(["disabled"])
        self.status.set("正在取消…")

    def close(self):
        if not self.busy:
            self.root.destroy()
        elif messagebox.askyesno("退出", "取消当前任务并退出？", parent=self.root):
            self.closing = True
            self.cancel()

    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "progress":
                    done, total = value
                    if self.progress["mode"] == "indeterminate":
                        self.progress.stop()
                        self.progress.configure(mode="determinate", maximum=total)
                    self.progress.configure(value=done)
                    self.status.set(f"正在导出 {done} / {total} 帧")
                    continue
                self.busy = False
                if self.closing:
                    self.root.destroy()
                    return
                self.progress.stop()
                self.progress.configure(mode="determinate", value=0)
                self.cancel_button.state(["disabled"])
                for widget in self.disabled_controls:
                    widget.state(["!disabled"])
                self.disabled_controls.clear()
                editor = self.video_editor
                if editor is not None and not editor.window.winfo_exists():
                    self.video_editor = editor = None
                if editor is not None:
                    editor.set_slam(editor.slam_frame, editor.slam_time)
                if kind == "alignment":
                    frame, _, elapsed = value
                    if editor is not None:
                        editor.set_slam(Image.fromarray(frame), elapsed)
                    self.status.set(f"已定位 SLAM {elapsed:.3f} s")
                elif kind == "topics":
                    self.set_topics(*value)
                elif kind == "preview":
                    frame, video, elapsed = value
                    self.frame = Image.fromarray(frame)
                    self.elapsed = elapsed
                    self.video_frame = None if video is None else Image.fromarray(video)
                    self.preview_label.configure(text=f"画面预览  ·  {elapsed:.3f} s")
                    text = "预览已更新"
                    if video is not None:
                        text += "，拖动实拍画面可调整位置"
                    elif self.values["video"].get():
                        text += "，此时刻没有实拍画面，请检查偏移和预览位置"
                    self.status.set(text)
                    self.draw_preview()
                elif kind == "saved":
                    destination, codec = value
                    name = {"h264_nvenc": "NVIDIA NVENC", "libx264": "CPU H.264", None: "PNG"}[codec]
                    self.status.set(f"已导出 {destination.name}  ·  {name}")
                elif kind == "error":
                    self.status.set("任务失败")
                    messagebox.showerror("任务失败", value, parent=editor.window if editor else self.root)
                else:
                    self.status.set("已取消")
        except Empty:
            pass
        self.root.after(60, self.poll)

    def draw_preview(self):
        self.canvas.delete("all")
        self.layer_bounds = None
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        if self.frame is None:
            self.canvas.create_text(width / 2, height / 2, text="选择 bag 后生成预览",
                                    fill="#a6b5c9", font=("Segoe UI", 13))
            return
        scale = min(width / self.frame.width, height / self.frame.height)
        size = (max(1, round(self.frame.width * scale)), max(1, round(self.frame.height * scale)))
        x, y = (width - size[0]) // 2, (height - size[1]) // 2
        self.view_bounds = x, y, *size
        self.base_photo = ImageTk.PhotoImage(self.frame.resize(size, Image.Resampling.LANCZOS))
        self.canvas.create_image(x, y, image=self.base_photo, anchor="nw")
        self.draw_layer()

    def draw_layer(self):
        self.canvas.delete("footage")
        self.layer_bounds = None
        if self.video_frame is None or self.view_bounds is None:
            return
        try:
            fraction = float(self.values["video_width"].get()) / 100
        except ValueError:
            return
        if not .05 <= fraction <= .90:
            return
        x, y, fw, fh = self.view_bounds
        left, top, w, h = video_rect((fw, fh), self.video_frame.size, fraction, self.position)
        self.layer_bounds = x + left, y + top, w, h
        self.layer_photo = ImageTk.PhotoImage(self.video_frame.resize((w, h), Image.Resampling.LANCZOS))
        self.canvas.create_image(x + left, y + top, image=self.layer_photo, anchor="nw", tags="footage")
        self.canvas.create_rectangle(x + left, y + top, x + left + w - 1, y + top + h - 1,
                                     outline="#8cc9fa", tags="footage")

    def start_drag(self, event):
        if self.busy or self.layer_bounds is None:
            return
        left, top, w, h = self.layer_bounds
        dx, dy = event.x - left, event.y - top
        if 0 <= dx < w and 0 <= dy < h:
            self.drag_offset = dx, dy
            self.canvas.configure(cursor="fleur")

    def move_video(self, event):
        if self.drag_offset is None or self.layer_bounds is None:
            return
        x, y, fw, fh = self.view_bounds
        _, _, w, h = self.layer_bounds
        dx, dy = self.drag_offset
        px = min(1, max(0, (event.x - dx - x) / max(1, fw - w)))
        py = min(1, max(0, (event.y - dy - y) / max(1, fh - h)))
        self.position = px, py
        self.draw_layer()

    def end_drag(self):
        self.drag_offset = None
        self.canvas.configure(cursor="")

    def reset_position(self):
        self.position = (.035, .87)
        self.draw_layer()


def main():
    root = tk.Tk()
    RendererApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
