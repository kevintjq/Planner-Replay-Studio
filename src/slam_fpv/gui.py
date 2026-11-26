"""Desktop controls for offline bag rendering and optional flight footage."""

from argparse import ArgumentParser
from dataclasses import replace
import json
import os
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
from .composition import Composition, compose, placement_axis, rounded_mask
from .desktop_style import INK, desktop_style
from .timeline import Timeline
from .layer_controls import LayerControls
from .video_editor import VideoEditor
from .sources import inspect_bag
from .spatial import CLOUD, CORRIDOR_TYPES, PLAN_TYPES


class FormParser(ArgumentParser):
    def error(self, message):
        raise ValueError(message)


class Cancelled(Exception):
    def __str__(self):
        return "Rendering cancelled"


class RendererApp(LayerControls):
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
        self.project = Composition()
        self.frame = self.elapsed = self.speed = None
        self.footages = ()
        self.cloud_views = {}
        self.selection = None
        self.drag_offset = None
        self.resize_drag = None
        self.resize_handle = None
        self.base_cache = None
        self.video_editor = None
        self.layer_bounds = {}
        self.view_bounds = None
        self.bag_path = None
        self.demo_selected = False
        self.disabled_controls = []

        root.title("SLAM FPV Renderer")
        root.geometry("1310x850")
        root.minsize(1000, 720)
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.font, self.style_photos = desktop_style(root)

        workspace = ttk.Frame(root, padding=18, style="Shell.TFrame")
        workspace.pack(fill="both", expand=True)
        workspace.columnconfigure(1, weight=1)
        workspace.rowconfigure(1, weight=1)
        header = ttk.Frame(workspace, style="Shell.TFrame")
        header.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 15))
        ttk.Label(header, text="SLAM FPV", style="Title.TLabel").pack(side="left")
        ttk.Label(header, text="合成工作台", style="Subtitle.TLabel").pack(side="left", padx=13)
        ttk.Button(header, text="主视角", command=lambda: self.edit_cloud_view("main", 0),
                   style="Shell.TButton").pack(side="right")

        self.form = ttk.Frame(workspace, style="Shell.TFrame")
        self.form.grid(row=1, column=0, sticky="nsew", padx=(0, 16))
        self.make_form()

        viewer = ttk.Frame(workspace, padding=14, style="Workspace.TFrame")
        viewer.grid(row=1, column=1, sticky="nsew")
        self.preview_label = ttk.Label(viewer, text="画面预览", style="PanelTitle.TLabel")
        self.preview_label.pack(anchor="w", pady=(0, 8))
        self.canvas = tk.Canvas(viewer, bg="#F8F9FB", highlightthickness=0, width=460, height=365)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<Configure>", lambda _: self.draw_preview())
        self.canvas.bind("<ButtonPress-1>", self.start_drag)
        self.canvas.bind("<B1-Motion>", self.move_layer)
        self.canvas.bind("<ButtonRelease-1>", lambda _: self.end_drag())

        self.timeline = Timeline(viewer, self.seek_timeline, self.apply_interval, self.font)
        self.timeline.pack(fill="x", pady=(7, 0))
        for key in ("start", "seconds"):
            self.values[key].trace_add("write", lambda *_: self.update_timeline())

        bar = ttk.Frame(workspace, style="Shell.TFrame")
        bar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        bar.columnconfigure(0, weight=1)
        self.status = tk.StringVar(value="选择 ROS1 bag 开始，或打开模拟预览")
        ttk.Label(bar, textvariable=self.status, style="Shell.TLabel", wraplength=690).grid(
            row=0, column=0, sticky="w")
        self.progress = ttk.Progressbar(bar, length=130, maximum=100)
        self.progress.grid(row=0, column=1, padx=12)
        self.cancel_button = ttk.Button(bar, text="取消", command=self.cancel, state="disabled", style="Shell.TButton")
        self.cancel_button.grid(row=0, column=2)
        self.refresh_layers()
        root.after(60, self.poll)

    def make_form(self):
        nav = ttk.Notebook(self.form)
        nav.pack(fill="both", expand=True)
        self.nav = nav
        source, quality, camera, footage, spatial = (ttk.Frame(nav, padding=12) for _ in range(5))
        for panel, name in zip((source, quality, camera, footage, spatial), ("数据", "画质", "相机", "图层", "空间叠加")):
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
        self.field(quality, 4, "体素 (m，0 为原始)", "voxel", ".015")
        self.field(quality, 5, "点数上限", "max_points", "850000")
        self.field(quality, 6, "点大小 (px)", "point_size", "2.3")
        self.field(quality, 7, "点云着色", "color", "SLAM 高度彩虹", ("SLAM 高度彩虹", "原始 RGB", "按高度", "自选纯色"))
        self.color_field(quality, 8, "点云纯色", "cloud_color", "#70d9ff")
        self.color_field(quality, 9, "轨迹颜色", "trajectory_color", "#f9d74b")
        self.field(quality, 10, "轨迹线宽 (px)", "trajectory_width", "3")
        self.field(quality, 11, "高度范围 (m)", "height_range", "")
        self.edl = tk.BooleanVar(value=True)
        self.slam_view = tk.BooleanVar(value=True)
        render_styles = ttk.Frame(quality)
        render_styles.grid(row=12, column=0, columnspan=2, sticky="w", pady=4)
        ttk.Checkbutton(render_styles, text="SLAM 原貌", variable=self.slam_view).pack(side="left", padx=(0, 8))
        edl_toggle = ttk.Checkbutton(render_styles, text="EDL", variable=self.edl)
        edl_toggle.pack(side="left")
        def update_edl(*_):
            edl_toggle.state(["disabled" if self.slam_view.get() else "!disabled"])
        self.slam_view.trace_add("write", update_edl)
        update_edl()
        self.field(quality, 13, "MP4 编码器", "encoder", "自动", ("自动", "CPU", "NVIDIA NVENC"))

        for row, (label, key, value) in enumerate((
            ("跟随距离 (m)", "distance", "8.5"),
            ("主视角高度 (m)", "camera_height", "4.8"),
            ("主视角横向偏移 (m)", "side", "0"),
            ("垂直视场角 (°)", "fov", "65"),
            ("高位斜视距离 (m)", "inset_distance", "10"),
            ("高位斜视高度 (m)", "inset_height", "6.7"),
            ("三轴长度 (m)", "axis", ".48"),
            ("三轴最小显示 (px)", "axis_pixels", "28"),
        )):
            self.field(camera, row, label, key, value)

        self.make_layer_panel(footage)
        self.make_spatial_panel(spatial)

        actions = ttk.Frame(self.form, style="Shell.TFrame")
        actions.pack(fill="x", pady=(10, 0))
        ttk.Button(actions, text="生成预览", style="Shell.TButton", command=lambda: self.start_render(True)).pack(side="left")
        ttk.Button(actions, text="导出 MP4 / PNG", style="Shell.Primary.TButton",
                   command=lambda: self.start_render(False)).pack(side="right")
        self.sync_odometry()

    def make_spatial_panel(self, panel):
        self.spatial_panel = panel
        self.spatial_toggles = {}
        self.spatial_topics = {}
        self.spatial_demo_labels = {}

        def toggle(row, name, label, color):
            flag = self.spatial_toggles[name] = tk.BooleanVar(value=False)
            ttk.Checkbutton(panel, text=label, variable=flag, command=self.sync_spatial).grid(
                row=row, column=0, sticky="w", pady=5)
            group = ttk.Frame(panel)
            group.grid(row=row, column=1, sticky="e")
            tk.Label(group, width=3, height=1, bg=color).pack(side="right", padx=3)

        for row, name, label, color in ((0, "plan", "规划轨迹", "#33EB80"),
                                         (2, "corridor", "飞行安全走廊", "#4DB3FF"),
                                         (4, "scan", "当前扫描点云", "#76F6FF")):
            toggle(row, name, label, color)
            self.spatial_topics[name] = self.field(panel, row+1, "ROS 话题", name + "_topic", "", ("",))
            demo = ttk.Label(panel, text="内置模拟数据", style="Quiet.TLabel")
            demo.grid(row=row+1, column=1, sticky="w")
            self.spatial_demo_labels[name] = demo
        toggle(6, "camera", "相机 FOV", "#C787FF")
        self.field(panel, 7, "光学坐标系", "camera_frame", "camera_optical_frame")
        self.field(panel, 8, "水平° 垂直° 深度m", "camera_fov", "82 53 3.5")
        toggle(9, "lidar", "雷达 FOV", "#FA9942")
        self.field(panel, 10, "雷达坐标系", "lidar_frame", "lidar")
        self.field(panel, 11, "水平° 垂直° 半径m 仰角°", "lidar_fov", "360 40 5 0")
        self.spatial_hint = tk.StringVar()
        ttk.Label(panel, textvariable=self.spatial_hint, style="Quiet.TLabel", wraplength=295).grid(
            row=12, column=0, columnspan=2, sticky="w", pady=(12, 2))
        self.sync_spatial()

    def sync_spatial(self):
        for name, widget in self.spatial_topics.items():
            demo = self.spatial_demo_labels[name]
            if self.demo_selected:
                widget.grid_remove()
                demo.grid()
            else:
                demo.grid_remove()
                widget.grid()
                enabled = self.bag_path is not None and self.spatial_toggles[name].get()
                widget.configure(state="readonly" if enabled else "disabled")
        if self.demo_selected:
            self.spatial_hint.set("模拟模式使用内置规划与传感器数据。勾选图层后重新生成预览。")
        else:
            self.spatial_hint.set("配置后重新生成预览。当前扫描留空时使用主点云话题的最新一帧。")

    def add_spatial_args(self, args, values):
        for name in ("plan", "corridor", "scan"):
            if not self.spatial_toggles[name].get():
                continue
            topic = values[name + "_topic"]
            if self.bag_path is None or (name == "scan" and not topic):
                args.append("--show-" + name)
            elif topic:
                args.extend(("--" + name + "-topic", topic))
            else:
                label = "规划轨迹" if name == "plan" else "飞行安全走廊"
                raise ValueError(f"请选择{label}话题，或取消勾选。")
        for name, total in (("camera", 3), ("lidar", 4)):
            if not self.spatial_toggles[name].get():
                continue
            angles = values[name+"_fov"].replace("，", " ").replace(",", " ").split()
            frame = values[name+"_frame"]
            if len(angles) != total or not frame:
                raise ValueError("请填写传感器坐标系及对应数量的 FOV 参数。")
            args.extend(("--" + name + "-fov", frame, *angles))

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
        self.sync_spatial()
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
        for name, types in (("plan", PLAN_TYPES), ("corridor", CORRIDOR_TYPES), ("scan", (CLOUD,))):
            widget = self.spatial_topics[name]
            choices = sorted({row["topic"] for row in topics if row["type"] in types})
            widget.configure(values=("", *choices))
            if self.values[name + "_topic"].get() not in choices:
                widget.set("")
        self.sync_spatial()
        self.values["pose_source"].set("Odometry" if odometry else "TF")
        self.status.set(f"已读取 {len(clouds)} 个点云话题，{len(odometry)} 个里程计话题")

    def reset_alignment(self, _event=None):
        self.frame = self.elapsed = self.speed = self.base_cache = None
        self.footages = (None,) * len(self.project.videos)
        self.cloud_views = {}
        videos = tuple(replace(layer, edit=replace(layer.edit, anchors=())) for layer in self.project.videos)
        self.project = replace(self.project, videos=videos)
        self.preview_label.configure(text="画面预览")
        self.draw_preview()

    def open_video_editor(self):
        if self.selection is None or self.selection[0] != "video":
            return
        index = self.selection[1]
        layer = self.project.videos[index]

        def apply(edit):
            self.replace_layer("video", index, replace(layer, edit=edit))
            self.invalidate_video(index)
            if self.frame is not None and not self.busy:
                self.root.after_idle(lambda: self.start_render(True))

        try:
            self.video_editor = VideoEditor(self.root, layer.path, layer.edit, self.frame, self.elapsed,
                                            self.request_alignment_frame, apply)
        except (ValueError, OSError, av.FFmpegError) as error:
            messagebox.showerror("无法打开视频", str(error), parent=self.root)

    def request_alignment_frame(self, seconds):
        if self.busy:
            raise ValueError("请等待当前预览完成")
        args = self.build_args(Path("preview.png"))
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
        args = ["demo", "--demo-scene", "trail"] if self.bag_path is None else ["bag", str(self.bag_path)]
        args += ["--output", str(destination), "--width", width, "--height", height]
        keys = ("start", "seconds", "fps", "supersample", "crf", "voxel", "max_points", "point_size",
                "cloud_color", "trajectory_color", "trajectory_width", "distance", "camera_height",
                "side", "fov", "inset_distance", "inset_height", "axis", "axis_pixels")
        if self.bag_path is not None:
            if not values["cloud_topic"] or not values["world"] or not values["body"]:
                raise ValueError("请选择点云话题并填写世界、机体坐标系。")
            if values["pose_source"] == "Odometry" and not values["odom_topic"]:
                raise ValueError("请选择里程计话题，或将位姿来源改为 TF。")
            keys += ("cloud_topic", "odom_topic", "world", "body", "max_gap")
            args += ["--pose-source", "odom" if values["pose_source"] == "Odometry" else "tf"]
            if values["extrinsics"]:
                keys += ("extrinsics",)
        for key in keys:
            args += ["--" + key.replace("_", "-") + "=" + values[key]]
        color = {"SLAM 高度彩虹": "scan", "原始 RGB": "rgb", "按高度": "height", "自选纯色": "solid"}[values["color"]]
        if values["height_range"]:
            limits = values["height_range"].replace("，", " ").replace(",", " ").split()
            args += ["--height-range", *limits]
        if self.slam_view.get():
            args.append("--slam-view")
        mode = "accumulate" if values["cloud_mode"] == "累积扫描" else "replace"
        encoder = {"自动": "auto", "CPU": "cpu", "NVIDIA NVENC": "nvenc"}[values["encoder"]]
        args += ["--color", color, "--cloud-mode", mode, "--encoder", encoder]
        if not self.edl.get():
            args.append("--no-edl")
        self.add_spatial_args(args, values)
        settings = parser(FormParser).parse_args(args)
        validate(settings)
        return settings

    def start_render(self, preview):
        if self.busy:
            return
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
            project = self.project
            if any(layer.path.resolve() == destination.resolve() for layer in project.videos) and not preview:
                raise ValueError("导出位置不能覆盖已导入的素材。")
            if preview and args.width > 1440:
                args.height = round(args.height * 1440 / args.width / 2) * 2
                args.width, args.supersample = 1440, 1
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
                       preview_offset=offset, check_cancel=self.check_cancel, project=project)
                return "preview", result[0]
            destination.parent.mkdir(parents=True, exist_ok=True)
            with TemporaryDirectory(prefix=".slam-fpv-", dir=destination.parent) as staging:
                temporary_output = Path(staging) / destination.name
                render(args, source, staging=temporary_output, project=project,
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
        self.timeline.busy = True
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
                self.timeline.busy = False
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
                    frame, videos, elapsed, self.speed, self.cloud_views = value
                    self.frame = Image.fromarray(frame)
                    self.elapsed = elapsed
                    self.base_cache = None
                    self.footages = tuple(None if video is None else Image.fromarray(video) for video in videos)
                    self.preview_label.configure(text=f"画面预览    {elapsed:.3f} s")
                    self.status.set("预览已更新。拖动画面中的图层可调整位置。")
                    self.draw_preview()
                    self.update_timeline()
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
        self.layer_bounds = {}
        self.resize_handle = None
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        if self.frame is None:
            self.canvas.create_rectangle(14, 18, width-14, height-18, fill="#F3F6FB", width=0)
            self.canvas.create_text(width / 2, height / 2, text="打开 bag 或使用模拟数据",
                                    fill=INK, font=(self.font, 12))
            return
        scale = min(width / self.frame.width, height / self.frame.height)
        size = (max(1, round(self.frame.width * scale)), max(1, round(self.frame.height * scale)))
        x, y = (width - size[0]) // 2, (height - size[1]) // 2
        self.view_bounds = x, y, *size
        if self.base_cache is None or self.base_cache.size != size:
            self.base_cache = self.frame.convert("RGB").resize(size, Image.Resampling.LANCZOS)
            if self.frame.mode == "RGBA":
                self.base_cache.putalpha(self.frame.getchannel("A").resize(size, Image.Resampling.LANCZOS))
        image, bounds = compose(self.base_cache, self.project, self.footages, self.elapsed, self.speed, self.cloud_views)
        image.putalpha(rounded_mask(image.size, 16))
        self.base_photo = ImageTk.PhotoImage(image)
        self.canvas.create_image(x, y, image=self.base_photo, anchor="nw")
        self.layer_bounds = {key: (x+left, y+top, w, h) for key, (left, top, w, h) in bounds.items()}
        selected = self.layer_bounds.get(self.selection)
        if selected is not None:
            x, y, w, h = selected
            vx, vy, vw, vh = self.view_bounds
            left, top = max(x, vx), max(y, vy)
            right, bottom = min(x+w, vx+vw)-2, min(y+h, vy+vh)-2
            if right > left and bottom > top:
                self.canvas.create_rectangle(left, top, right, bottom, outline="#0A84FF", width=1)
                self.resize_handle = right-5, bottom-5
                hx, hy = self.resize_handle
                self.canvas.create_oval(hx-4, hy-4, hx+4, hy+4, fill="#FFFFFF", outline="#0A84FF", width=2)

    def start_drag(self, event):
        if self.busy:
            return
        if self.resize_handle is not None:
            hx, hy = self.resize_handle
            if abs(event.x-hx) <= 9 and abs(event.y-hy) <= 9:
                _, _, w, h = self.layer_bounds[self.selection]
                self.resize_drag = event.x, event.y, w, h, self.layer_scale()
                self.canvas.configure(cursor="sizing")
                return
        keys = list(reversed(self.layer_bounds))
        if (self.selection in self.layer_bounds and self.selection[0] == "video"
                and self.project.videos[self.selection[1]].backdrop):
            # An explicitly selected bottom layer can be dragged through the layers above it.
            keys.remove(self.selection)
            keys.insert(0, self.selection)
        for key in keys:
            left, top, w, h = self.layer_bounds[key]
            dx, dy = event.x-left, event.y-top
            if 0 <= dx < w and 0 <= dy < h:
                self.select_layer(key)
                self.drag_offset = dx, dy
                self.canvas.configure(cursor="fleur")
                break

    def layer_scale(self):
        kind, index = self.selection
        if kind == "speed":
            return self.project.speed_scale
        layer = self.layers(kind)[index]
        if kind == "text":
            return layer.size
        return layer.backdrop_scale if kind == "video" and layer.backdrop else layer.width

    def resize_selected(self, factor):
        kind, index = self.selection
        clamp = lambda value, low, high: min(high, max(low, value))
        if kind == "speed":
            self.project = replace(self.project, speed_scale=clamp(factor, .35, 2.5))
        else:
            layer = self.layers(kind)[index]
            if kind == "text":
                layer = replace(layer, size=clamp(round(factor), 12, 120))
            elif kind == "video" and layer.backdrop:
                layer = replace(layer, backdrop_scale=clamp(factor, .2, 2.))
            else:
                minimum, maximum = (.05, .9) if kind == "video" else (.12, 1.5)
                layer = replace(layer, width=clamp(factor, minimum, maximum))
            self.replace_layer(kind, index, layer, redraw=False)

    def move_layer(self, event):
        if self.resize_drag is not None:
            x, y, w, h, scale = self.resize_drag
            factor = 1 + ((event.x-x)*w + (event.y-y)*h)/(w*w + h*h)
            self.resize_selected(scale * factor)
            self.draw_preview()
            return
        if self.drag_offset is None or self.selection not in self.layer_bounds:
            return
        x, y, fw, fh = self.view_bounds
        _, _, w, h = self.layer_bounds[self.selection]
        dx, dy = self.drag_offset
        kind, index = self.selection
        layer = None if kind == "speed" else self.layers(kind)[index]
        can_pan = kind in {"main", "view"} or (kind == "video" and layer.backdrop)
        position = []
        for p, space, size in ((event.x-dx-x, fw, w), (event.y-dy-y, fh, h)):
            offset, travel = placement_axis(space, size) if can_pan else (0, max(1, space-size))
            position.append(min(1, max(0, (p-offset)/travel)))
        position = tuple(position)
        if kind == "speed":
            self.project = replace(self.project, speed_position=position)
        elif kind == "video" and layer.backdrop:
            self.replace_layer(kind, index, replace(layer, backdrop_position=position), redraw=False)
        else:
            self.replace_layer(kind, index, replace(layer, position=position), redraw=False)
        self.draw_preview()

    def end_drag(self):
        changed = self.drag_offset is not None or self.resize_drag is not None
        self.drag_offset = self.resize_drag = None
        self.canvas.configure(cursor="")
        if changed:
            self.show_layer()


def main():
    root = tk.Tk()
    RendererApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
