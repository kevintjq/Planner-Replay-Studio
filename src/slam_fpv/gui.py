"""Desktop controls for offline bag rendering and optional flight footage."""

from argparse import ArgumentParser
from dataclasses import replace
import json
import os
from math import ceil, isfinite
from pathlib import Path
from queue import Empty, Queue
from tempfile import TemporaryDirectory
from threading import Event, Thread
import tkinter as tk
import av
from tkinter import colorchooser, filedialog, messagebox, ttk

from PIL import Image, ImageDraw, ImageTk

from .cli import extrinsics, open_source, parser, render, validate
from .composition import (
    LAYOUT_NAMES, MAX_CAMERA_ZOOM, MIN_CAMERA_ZOOM, Composition, LayoutCue, compose, placement_axis, rounded_mask,
)
from .desktop_style import INK, desktop_style
from .timeline import Timeline
from .layer_controls import LayerControls
from .video_editor import VideoEditor
from .sources import inspect_bag
from .project_file import ProjectFile, load_project, save_project
from .export_media import inspect_export
from .spatial import CORRIDOR_TYPES, EGO_SPLINES, GOAL_TYPES, PLAN_TYPES
from .sensor_topics import SCAN_TYPES, camera_choices, connected_frames, pose_frames, suggested_topic


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
        self.frame = self.elapsed = self.speed = self.source_duration = self.checkerboard = None
        self.footages = ()
        self.cloud_views = {}
        self.timeline_events = ()
        self.selection = None
        self.drag_offset = None
        self.resize_drag = None
        self.resize_handle = None
        self.base_cache = None
        self.video_editor = None
        self.layer_bounds = {}
        self.view_bounds = None
        self.zoom_job = None
        self.bag_path = None
        self.bag_metadata = None
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
        ttk.Button(header, text="线条与标记", command=self.edit_render_marks,
                   style="Shell.TButton").pack(side="right", padx=(6, 0))
        ttk.Button(header, text="保存项目", command=self.save_workspace, style="Shell.TButton").pack(side="right", padx=6)
        ttk.Button(header, text="打开项目", command=self.open_workspace, style="Shell.TButton").pack(side="right")

        self.form = ttk.Frame(workspace, style="Shell.TFrame")
        self.form.grid(row=1, column=0, sticky="nsew", padx=(0, 16))
        self.make_form()

        viewer = ttk.Frame(workspace, padding=14, style="Workspace.TFrame")
        viewer.grid(row=1, column=1, sticky="nsew")
        preview_header = ttk.Frame(viewer)
        preview_header.pack(fill="x", pady=(0, 8))
        self.preview_label = ttk.Label(preview_header, text="画面预览", style="PanelTitle.TLabel")
        self.preview_label.pack(side="left")
        ttk.Label(preview_header, text="点云画面上滚轮缩放", style="Quiet.TLabel").pack(side="left", padx=10)
        self.transparent_canvas = tk.BooleanVar(value=False)
        ttk.Checkbutton(preview_header, text="透明画布", variable=self.transparent_canvas,
                        command=self.toggle_canvas).pack(side="right")
        self.overlay_status = tk.StringVar()
        self.overlay_label = ttk.Label(viewer, textvariable=self.overlay_status, style="Quiet.TLabel",
                                       justify="left", wraplength=700)
        self.overlay_label.pack(fill="x", pady=(0, 4))
        self.canvas = tk.Canvas(viewer, bg="#F8F9FB", highlightthickness=0, width=460, height=365)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<Configure>", self.resize_preview)
        self.canvas.bind("<ButtonPress-1>", self.start_drag)
        self.canvas.bind("<B1-Motion>", self.move_layer)
        self.canvas.bind("<ButtonRelease-1>", lambda _: self.end_drag())
        self.canvas.bind("<MouseWheel>", self.zoom_preview)
        self.canvas.bind("<Button-4>", lambda event: self.zoom_preview(event, 1))
        self.canvas.bind("<Button-5>", lambda event: self.zoom_preview(event, -1))

        self.timeline = Timeline(viewer, self.seek_timeline, self.apply_interval, self.font, self.jump_cue)
        self.timeline.pack(fill="x", pady=(7, 0))
        transport = ttk.Frame(viewer)
        transport.pack(fill="x")
        self.event_choice = tk.StringVar(value="事件跳转")
        self.event_picker = ttk.Combobox(transport, width=20, state="readonly", textvariable=self.event_choice)
        self.event_picker.pack(side="left", padx=(2, 8))
        self.event_picker.bind("<<ComboboxSelected>>", self.choose_event)
        self.compare_plans = tk.BooleanVar(value=False)
        ttk.Checkbutton(transport, text="对比上一条规划", variable=self.compare_plans,
                        command=self.toggle_plan_comparison).pack(side="left")
        self.layout_button = ttk.Menubutton(transport, text="添加布局标记")
        self.layout_menu = tk.Menu(self.layout_button, tearoff=False, postcommand=self.make_layout_menu)
        self.layout_video_menu = tk.Menu(self.layout_menu, tearoff=False)
        self.layout_button.configure(menu=self.layout_menu)
        self.layout_button.pack(side="right")
        self.layout_status = tk.StringVar(value="当前合成")
        ttk.Label(transport, textvariable=self.layout_status, style="Quiet.TLabel").pack(side="right", padx=8)
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
        self.cloud_topics.bind("<<ComboboxSelected>>", self.change_source_frames)
        self.field(source, 3, "位姿来源", "pose_source", "Odometry", ("Odometry", "TF"))
        self.odom_topics = self.field(source, 4, "里程计话题", "odom_topic", "", ())
        self.odom_topics.bind("<<ComboboxSelected>>", self.change_source_frames)
        self.world_frames = self.field(source, 5, "世界坐标系", "world", "", ())
        self.body_frames = self.field(source, 6, "机体坐标系", "body", "", ())
        self.field(source, 7, "最大插值间隔 (s)", "max_gap", ".5")
        self.field(source, 8, "点云更新方式", "cloud_mode", "累积扫描", ("累积扫描", "替换整图"))
        self.file_row(source, 9, "外参 JSON", "extrinsics", (("JSON", "*.json"),), "可选")
        self.values["extrinsics"].trace_add("write", lambda *_: self.sync_spatial())
        self.field(source, 10, "导出起点 (s)", "start", "0")
        self.field(source, 11, "导出时长 (s)", "seconds", "6")
        self.field(source, 12, "片段内预览位置 (s)", "preview_offset", "1")
        self.values["pose_source"].trace_add("write", self.change_source_frames)
        for widget in (self.world_frames, self.body_frames):
            widget.bind("<<ComboboxSelected>>", lambda _: self.sync_spatial())

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
        self.field(quality, 10, "已飞轨迹线宽 (px)", "trajectory_width", "5")
        self.field(quality, 11, "规划轨迹线宽 (px)", "plan_width", "6")
        self.values["axis_width"] = tk.StringVar(value="5")
        self.values["goal_size"] = tk.StringVar(value="24")
        self.field(quality, 12, "高度范围 (m)", "height_range", "")
        self.edl = tk.BooleanVar(value=True)
        self.slam_view = tk.BooleanVar(value=True)
        render_styles = ttk.Frame(quality)
        render_styles.grid(row=13, column=0, columnspan=2, sticky="w", pady=4)
        ttk.Checkbutton(render_styles, text="SLAM 原貌", variable=self.slam_view).pack(side="left", padx=(0, 8))
        edl_toggle = ttk.Checkbutton(render_styles, text="EDL", variable=self.edl)
        edl_toggle.pack(side="left")
        def update_edl(*_):
            edl_toggle.state(["disabled" if self.slam_view.get() else "!disabled"])
        self.slam_view.trace_add("write", update_edl)
        update_edl()
        self.field(quality, 14, "MP4 编码器", "encoder", "自动", ("自动", "CPU", "NVIDIA NVENC"))

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
        self.spatial_details = {}
        for index, (name, label, color) in enumerate((
            ("plan", "规划轨迹", "#33EB80"), ("goal", "目标点", "#FFCB63"),
            ("corridor", "飞行安全走廊", "#4DB3FF"), ("scan", "雷达当前帧高亮", "#76F6FF"),
            ("camera", "相机 FOV", "#C7A6F0"),
        )):
            row = index * 3
            flag = self.spatial_toggles[name] = tk.BooleanVar(value=False)
            ttk.Checkbutton(panel, text=label, variable=flag, command=self.sync_spatial).grid(
                row=row, column=0, sticky="w", pady=(9, 3))
            tk.Label(panel, width=3, height=1, bg=color).grid(row=row, column=1, sticky="e")
            widget = self.field(panel, row+1, "话题来源", name + "_topic", "自动识别", ("自动识别",))
            widget.bind("<<ComboboxSelected>>", lambda _: self.sync_spatial())
            self.spatial_topics[name] = widget
            detail = self.spatial_details[name] = tk.StringVar()
            ttk.Label(panel, textvariable=detail, style="Quiet.TLabel", wraplength=300).grid(
                row=row+2, column=0, columnspan=2, sticky="w", pady=(1, 3))
        self.ego_options = ttk.Frame(panel)
        self.ego_options.grid(row=15, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        self.ego_options.columnconfigure(1, weight=1)
        self.field(self.ego_options, 0, "EGO 轨迹坐标系", "plan_frame", "world")
        self.spatial_hint = tk.StringVar()
        ttk.Label(panel, textvariable=self.spatial_hint, style="Quiet.TLabel", wraplength=300).grid(
            row=16, column=0, columnspan=2, sticky="w", pady=(12, 2))
        self.sync_spatial()

    def spatial_source(self, name):
        if not self.bag_metadata or self.demo_selected:
            return "", None
        topics = self.bag_metadata["topics"]
        world, body = self.values["world"].get(), self.values["body"].get()
        picked = self.values[name+"_topic"].get()
        topic = "" if picked == "自动识别" else picked
        if name == "camera":
            candidates = camera_choices(topics)
            pairs = list(self.bag_metadata["tf_pairs"])
            if self.values["pose_source"].get() == "Odometry":
                odom = self.values["odom_topic"].get()
                pairs.extend((row["frame"], row["child"]) for row in topics
                             if row["topic"] == odom and row.get("child"))
            path = self.values["extrinsics"].get()
            if path:
                try:
                    pairs.extend((parent.strip("/"), child.strip("/")) for parent, child, _, _ in extrinsics(Path(path)))
                except (OSError, ValueError, TypeError, KeyError, AttributeError):
                    pass
            frames = connected_frames(world, pairs)
            row = next((row for row in candidates if (not topic or row["topic"] == topic)
                        and row["view"].frame in frames), None)
            return (row["topic"], row) if row else ("", None)
        topic = topic or suggested_topic(topics, name, world, body)
        return topic, next((row for row in topics if row["topic"] == topic), None)

    def sync_spatial(self):
        for name, widget in self.spatial_topics.items():
            enabled = self.bag_path is not None and self.spatial_toggles[name].get()
            widget.configure(state="readonly" if enabled else "disabled")
            topic, row = self.spatial_source(name)
            if self.demo_selected:
                detail = "内置模拟数据"
            elif self.bag_path is None:
                detail = "打开 ROS1 bag 后识别"
            elif row and name == "camera":
                view = row["view"]
                detail = f"{topic} · {view.frame} · {view.angles[0]:.0f}° × {view.angles[1]:.0f}°"
            elif topic:
                frame = row.get("frame") if row else None
                detail = f"{topic} · {frame}" if frame else topic
            elif name == "scan":
                detail = "使用主点云的最新一帧"
            else:
                detail = "没有可用话题或 TF，此图层将跳过" if name == "camera" else "未匹配话题，可从列表选择"
            self.spatial_details[name].set(detail)
        _, plan = self.spatial_source("plan")
        if self.spatial_toggles["plan"].get() and plan and plan["type"] in EGO_SPLINES:
            self.ego_options.grid()
        else:
            self.ego_options.grid_remove()
        self.spatial_hint.set("勾选图层后重新生成预览。传感器坐标系读取自消息和 TF。")

    def add_spatial_args(self, args, values):
        for name, enabled in self.spatial_toggles.items():
            if not enabled.get():
                continue
            topic, _ = self.spatial_source(name)
            if name == "camera":
                args.append("--show-camera-fov")
                if self.bag_path and values["camera_topic"] != "自动识别":
                    args.extend(("--camera-info-topic", values["camera_topic"]))
            elif topic:
                args.extend(("--" + name + "-topic", topic))
                if name == "plan":
                    args.extend(("--plan-frame", values["plan_frame"] or "world"))
            elif self.bag_path is None or name == "scan":
                args.append("--show-" + name)

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

        def display_path(*_):
            path = self.values[name].get()
            display.set(Path(path).name if path else placeholder)

        self.values[name].trace_add("write", display_path)

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
        variable.trace_add("write", lambda *_: swatch.configure(bg=variable.get()))

        def pick():
            _, color = colorchooser.askcolor(variable.get(), parent=self.root, title=label)
            if color:
                variable.set(color)
                if name == "cloud_color":
                    self.values["color"].set("自选纯色")

        ttk.Button(group, textvariable=variable, command=pick).pack(side="left")

    def save_workspace(self):
        if self.busy:
            return
        path = filedialog.asksaveasfilename(parent=self.root, title="保存项目", defaultextension=".slamfpv",
                                            filetypes=(("SLAM FPV 项目", "*.slamfpv"),))
        if not path:
            return
        values = {name: value.get() for name, value in self.values.items() if name != "extrinsics"}
        enabled = {name: flag.get() for name, flag in self.spatial_toggles.items()}
        enabled.update(edl=self.edl.get(), slam_view=self.slam_view.get())
        source = "bag" if self.bag_path else "demo" if self.demo_selected else "none"
        external = self.values["extrinsics"].get()
        project = ProjectFile(self.project, values, enabled, source, self.bag_path, Path(external) if external else None)
        try:
            save_project(Path(path), project)
        except (ValueError, OSError) as error:
            messagebox.showerror("无法保存项目", str(error), parent=self.root)
            return
        self.status.set(f"已保存项目 {Path(path).name}")

    def open_workspace(self):
        if self.busy:
            return
        path = filedialog.askopenfilename(parent=self.root, title="打开项目",
                                          filetypes=(("SLAM FPV 项目", "*.slamfpv"),))
        if not path:
            return
        try:
            project = load_project(Path(path))
        except (ValueError, OSError) as error:
            messagebox.showerror("无法打开项目", str(error), parent=self.root)
            return
        replacements = {}
        for media in project.media():
            if not media.is_file():
                linked = filedialog.askopenfilename(parent=self.root, title=f"重新定位 {media.name}",
                                                     initialfile=media.name)
                if not linked:
                    self.status.set("已取消打开项目")
                    return
                replacements[media] = Path(linked).resolve()
        project = project.relink(replacements)
        if project.bag is None:
            self.restore_workspace(Path(path), project, None)
        else:
            def work():
                metadata = inspect_bag(project.bag, self.check_cancel)
                return "project", (Path(path), project, metadata)
            self.run_job(work, "正在读取项目的话题…")

    def restore_workspace(self, path, saved, metadata):
        if saved.bag:
            self.set_topics(saved.bag, metadata)
        else:
            self.bag_path = self.bag_metadata = None
            self.demo_selected = saved.source == "demo"
            self.bag_name.set("模拟点云与轨迹" if self.demo_selected else "未选择 ROS1 bag")
        # Restore source choices first; changing the pose source refreshes frame defaults.
        for name in ("cloud_topic", "odom_topic", "pose_source"):
            if name in saved.values:
                self.values[name].set(saved.values[name])
        for name, value in saved.values.items():
            if name in self.values and name not in ("cloud_topic", "odom_topic", "pose_source", "extrinsics"):
                self.values[name].set(value)
        self.values["extrinsics"].set(str(saved.extrinsics or ""))
        for name, flag in {**self.spatial_toggles, "edl": self.edl, "slam_view": self.slam_view}.items():
            if name in saved.enabled:
                flag.set(saved.enabled[name])
        self.selection = None
        self.reset_alignment()
        self.project = saved.composition
        self.transparent_canvas.set(self.project.transparent_canvas)
        self.compare_plans.set(self.project.compare_plans)
        self.footages = (None,) * len(self.project.videos)
        self.refresh_layers()
        self.sync_spatial()
        self.update_timeline()
        self.status.set(f"已打开项目 {path.name}")
        if self.bag_path or self.demo_selected:
            self.root.after_idle(lambda: self.start_render(True))

    def update_event_choices(self, start, duration):
        labels = {f"{stamp:.4f} s · {label}": stamp for stamp, label in self.timeline_events
                  if start <= stamp <= start + duration}
        self.event_times = labels
        self.event_picker.configure(values=tuple(labels), state="readonly" if labels else "disabled")
        if self.event_choice.get() not in labels:
            self.event_choice.set("事件跳转" if labels else "暂无规划事件")
        cue = self.project.layout_at(self.elapsed if self.elapsed is not None else start)
        self.layout_status.set(LAYOUT_NAMES[cue.mode] if cue else LAYOUT_NAMES["mix"])

    def choose_event(self, _event):
        stamp = self.event_times.get(self.event_choice.get())
        if stamp is not None:
            self.jump_cue(stamp)

    def jump_cue(self, stamp):
        if self.busy:
            return
        start, _ = self.timeline_range()
        fps = int(self.values["fps"].get())
        self.seek_timeline(start + max(0, ceil((stamp-start)*fps - 1e-7))/fps)

    def toggle_plan_comparison(self):
        if self.busy:
            self.compare_plans.set(self.project.compare_plans)
            return
        self.project = replace(self.project, compare_plans=self.compare_plans.get())
        if self.frame is not None:
            self.start_render(True)

    def make_layout_menu(self):
        self.layout_menu.delete(0, "end")
        state = "normal" if self.elapsed is not None and not self.busy else "disabled"
        for mode in ("mix", "cloud"):
            self.layout_menu.add_command(label=LAYOUT_NAMES[mode], state=state,
                                         command=lambda key=mode: self.set_layout(key))
        if self.project.videos:
            self.layout_video_menu.delete(0, "end")
            for i, layer in enumerate(self.project.videos, 1):
                self.layout_video_menu.add_command(label=f"{i}. {layer.path.name}",
                                                   command=lambda id=layer.id: self.set_layout("video", id))
            self.layout_menu.add_cascade(label=LAYOUT_NAMES["video"], menu=self.layout_video_menu, state=state)
        self.layout_menu.add_separator()
        self.layout_menu.add_command(label="移除当前时刻的标记", state=state, command=lambda: self.set_layout(None))

    def set_layout(self, mode, video_id=None):
        if self.busy or self.elapsed is None:
            return
        epsilon = 1. / int(self.values["fps"].get()) + 1e-7
        current = min(self.project.layouts, key=lambda cue: abs(cue.time-self.elapsed), default=None)
        match = current if current and abs(current.time-self.elapsed) <= epsilon else None
        cues = [cue for cue in self.project.layouts if cue != match]
        if mode is not None:
            cues.append(LayoutCue(match.time if match else self.elapsed, mode, video_id))
        self.project = replace(self.project, layouts=tuple(sorted(cues, key=lambda cue: cue.time)))
        self.draw_preview()
        self.update_timeline()
        self.status.set("已更新布局标记。紫色标记可跳转。")

    def resize_preview(self, event):
        self.overlay_label.configure(wraplength=max(200, event.width - 12))
        self.draw_preview()

    def toggle_canvas(self):
        if self.busy:
            self.transparent_canvas.set(self.project.transparent_canvas)
            return
        self.project = replace(self.project, transparent_canvas=self.transparent_canvas.get())
        self.draw_preview()

    def select_bag(self):
        path = filedialog.askopenfilename(parent=self.root, filetypes=(("ROS1 bag", "*.bag"),))
        if path:
            path = Path(path)
            self.run_job(lambda: ("topics", (path, inspect_bag(path, self.check_cancel))), "正在读取 bag 话题…")

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
        self.bag_metadata = data
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
        for name, types in (("plan", PLAN_TYPES), ("goal", GOAL_TYPES), ("corridor", CORRIDOR_TYPES),
                            ("scan", SCAN_TYPES)):
            choices = sorted({row["topic"] for row in topics if row["type"] in types})
            self.spatial_topics[name].configure(values=("自动识别", *choices))
            self.spatial_topics[name].set("自动识别")
        choices = tuple(dict.fromkeys(row["topic"] for row in camera_choices(topics)))
        self.spatial_topics["camera"].configure(values=("自动识别", *choices))
        self.spatial_topics["camera"].set("自动识别")
        self.sync_spatial()
        self.status.set(f"已读取 {len(clouds)} 个点云话题，{len(odometry)} 个里程计话题")

    def change_source_frames(self, *_):
        self.sync_odometry()
        if self.bag_path is None:
            return
        data = self.bag_metadata
        odometry = self.values["odom_topic"].get() if self.values["pose_source"].get() == "Odometry" else ""
        world, body = pose_frames(data["topics"], data["tf_pairs"], odometry, self.values["cloud_topic"].get())
        frames = {frame for pair in data["tf_pairs"] for frame in pair}
        frames.update(row[key] for row in data["topics"] for key in ("frame", "child") if row.get(key))
        for widget, frame in ((self.world_frames, world), (self.body_frames, body)):
            widget.configure(values=tuple(sorted(frames)))
            widget.set(frame)
        self.reset_alignment()
        self.sync_spatial()

    def reset_alignment(self, _event=None):
        self.frame = self.elapsed = self.speed = self.base_cache = self.source_duration = None
        self.overlay_status.set("")
        self.timeline_events = ()
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
            start, duration = self.timeline_range()
            end = start + duration
            metadata = self.bag_metadata
            approximate = (metadata["storage_end_ns"] - metadata["storage_start_ns"])/1e9 if metadata else end
            self.video_editor = VideoEditor(self.root, layer.path, layer.edit, self.frame, self.elapsed,
                                            self.request_alignment_frame, apply,
                                            duration=self.source_duration or max(end, approximate))
        except (ValueError, OSError, av.FFmpegError) as error:
            messagebox.showerror("无法打开视频", str(error), parent=self.root)

    def request_alignment_frame(self, seconds):
        if self.busy:
            raise ValueError("请等待当前预览完成")
        args = self.build_args(Path("preview.png"))
        args.start = 0
        args.seconds = max(args.seconds, seconds + 1 / args.fps)
        args.width, args.height, args.supersample = 960, 540, 1

        project = replace(self.project, videos=(), layouts=(), audio_id=None, views=(), texts=(), show_speed=False)
        metadata = self.bag_metadata

        def work():
            result = []
            source = open_source(args, self.check_cancel, metadata)
            render(args, source, preview=lambda *frame: result.append(frame),
                   preview_offset=seconds, check_cancel=self.check_cancel, project=project)
            return "alignment", (result[0], (source.last - source.first)/1e9)

        self.run_job(work, "正在定位 SLAM 帧…")

    def build_args(self, destination):
        if self.bag_path is None and not self.demo_selected:
            raise ValueError("请先打开 ROS1 bag 或选择使用模拟数据。")
        values = {key: variable.get().strip() for key, variable in self.values.items()}
        width, height = values["resolution"].split(" × ")
        args = ["demo", "--demo-scene", "trail"] if self.bag_path is None else ["bag", str(self.bag_path)]
        args += ["--output", str(destination), "--width", width, "--height", height]
        keys = ("start", "seconds", "fps", "supersample", "crf", "voxel", "max_points", "point_size",
                "cloud_color", "trajectory_color", "trajectory_width", "plan_width", "distance", "camera_height",
                "side", "fov", "inset_distance", "inset_height", "axis", "axis_pixels", "axis_width", "goal_size")
        if self.bag_path is not None:
            if not values["cloud_topic"] or not values["world"] or not values["body"]:
                raise ValueError("缺少点云话题或无法自动确定坐标系。请检查 bag，或从列表选择世界、机体坐标系。")
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
            outputs = {destination.resolve(), destination.with_suffix(".json").resolve()}
            if not preview and any(layer.path.resolve() in outputs for layer in project.videos):
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

        metadata = self.bag_metadata

        def work():
            source = open_source(args, self.check_cancel, metadata)
            if preview:
                result, data, events = [], [], []
                render(args, source, preview=lambda *frame: result.append(frame),
                       preview_data=lambda *status: data.append(status), event_data=events.extend,
                       preview_offset=offset, check_cancel=self.check_cancel, project=project)
                return "preview", (result[0], *data[0], tuple(events))
            checks, blocked, audio_ids = inspect_export(args, source, project, self.check_cancel)
            return "export_check", (args, source, project, destination, checks, blocked, audio_ids)

        self.run_job(work, "正在准备预览…" if preview else "正在检查导出区间…")

    def export_check(self, args, source, project, destination, checks, blocked, audio_ids):
        modal = tk.Toplevel(self.root)
        modal.title("导出检查")
        modal.transient(self.root)
        box = ttk.Frame(modal, padding=18)
        box.pack(fill="both", expand=True)
        ttk.Label(box, text=f"{args.width} × {args.height}  ·  {args.fps} fps",
                  style="PanelTitle.TLabel").pack(anchor="w")
        details = tk.Text(box, width=66, height=min(13, max(4, len(checks)+len(blocked)+1)),
                          font=(self.font, 10), wrap="word", relief="flat", padx=8, pady=8)
        details.pack(fill="both", expand=True, pady=8)
        details.insert("end", "\n".join((*blocked, *checks)))
        details.configure(state="disabled")
        video = destination.suffix.lower() == ".mp4"
        options = {"静音": None}
        options.update((f"{i}. {layer.path.name}", layer.id) for i, layer in enumerate(project.videos, 1)
                       if layer.id in audio_ids)
        initial = next((label for label, ident in options.items() if ident == project.audio_id), "静音")
        audio = tk.StringVar(value=initial)
        rates = {"1× 原速": 1., "0.5× 慢放": .5, "0.25× 慢放": .25}
        pace = tk.StringVar(value=next(label for label, rate in rates.items() if rate == project.play_rate))
        if video:
            row = ttk.Frame(box)
            row.pack(fill="x", pady=(0, 8))
            ttk.Label(row, text="原声").pack(side="left", padx=(0, 5))
            ttk.Combobox(row, textvariable=audio, values=tuple(options), state="readonly", width=25).pack(side="left")
            ttk.Label(row, text="速度").pack(side="left", padx=(12, 5))
            ttk.Combobox(row, textvariable=pace, values=tuple(rates), state="readonly", width=11).pack(side="left")
            ttk.Label(box, text="缺帧统计按原速计算。", style="Quiet.TLabel").pack(anchor="w")

        def proceed():
            prepared = replace(project, audio_id=options[audio.get()], play_rate=rates[pace.get()]) if video else project
            self.project = prepared
            modal.destroy()
            self.execute_export(args, source, prepared, destination)

        buttons = ttk.Frame(box)
        buttons.pack(fill="x", pady=(8, 0))
        ttk.Button(buttons, text="返回", command=modal.destroy).pack(side="right")
        ttk.Button(buttons, text="开始导出", command=proceed, style="Primary.TButton",
                   state="disabled" if blocked else "normal").pack(side="right", padx=6)
        modal.wait_visibility()
        modal.grab_set()

    def execute_export(self, args, source, project, destination):
        def work():
            destination.parent.mkdir(parents=True, exist_ok=True)
            with TemporaryDirectory(prefix=".slam-fpv-", dir=destination.parent) as staging:
                temporary_output = Path(staging) / destination.name
                render(args, source, staging=temporary_output, project=project,
                       progress=lambda done, total: self.events.put(("progress", (done, total))),
                       check_cancel=self.check_cancel)
                self.check_cancel()
                if not temporary_output.is_file() or not temporary_output.stat().st_size:
                    raise ValueError("编码器未写入成品，请检查 FFmpeg 或降低分辨率。")
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
        self.run_job(work, "正在准备导出…")

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
                    editor.finish_seek()
                if kind == "alignment":
                    (frame, _, elapsed, *_), duration = value
                    if editor is not None:
                        editor.set_slam(Image.fromarray(frame), elapsed, duration)
                    self.status.set(f"已定位 SLAM {elapsed:.3f} s")
                elif kind == "topics":
                    self.set_topics(*value)
                elif kind == "project":
                    self.restore_workspace(*value)
                elif kind == "export_check":
                    self.status.set("导出检查完成")
                    self.export_check(*value)
                elif kind == "preview":
                    (frame, videos, elapsed, self.speed, self.cloud_views), self.source_duration, diagnostics, self.timeline_events = value
                    self.overlay_status.set("\n".join(diagnostics))
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
        if image.mode == "RGBA":
            if self.checkerboard is None or self.checkerboard.size != size:
                self.checkerboard = Image.new("RGBA", size, "#E9EBEF")
                draw = ImageDraw.Draw(self.checkerboard)
                for top in range(0, size[1], 14):
                    for left in range(14 * ((top // 14) % 2), size[0], 28):
                        draw.rectangle((left, top, left+13, top+13), fill="#D8DBE3")
            image = Image.alpha_composite(self.checkerboard, image)
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

    def edit_render_marks(self):
        if self.busy:
            return
        settings = (("trajectory_width", "已飞轨迹线宽 (px)"), ("plan_width", "规划轨迹线宽 (px)"),
                    ("axis_width", "TF 三轴线宽 (px)"), ("goal_size", "目标点直径 (px)"))

        def apply(values):
            for key, label in settings:
                amount = float(values[key])
                if not isfinite(amount) or amount <= 0:
                    raise ValueError(f"{label}须大于 0。")
            for key, _ in settings:
                self.values[key].set(values[key])
            if self.frame is not None:
                self.root.after_idle(lambda: self.start_render(True))

        self.layer_dialog("线条与标记",
                          tuple((key, label, self.values[key].get()) for key, label in settings), apply)

    def zoom_preview(self, event, steps=None):
        if self.busy or self.frame is None:
            return
        if steps is None:
            steps = max(-5., min(5., event.delta / 120))
        if not steps:
            return
        # Choose the topmost point-cloud viewport under the cursor. HUDs may
        # sit over a viewport and should not prevent camera navigation.
        for key, (left, top, width, height) in reversed(self.layer_bounds.items()):
            if not (left <= event.x < left+width and top <= event.y < top+height):
                continue
            if key[0] == "video":
                return "break"
            if key[0] not in ("main", "view"):
                continue
            kind, index = key
            view = self.layers(kind)[index]
            zoom = min(MAX_CAMERA_ZOOM, max(MIN_CAMERA_ZOOM, view.zoom * 1.18**steps))
            if zoom != view.zoom:
                self.replace_layer(kind, index, replace(view, zoom=zoom), redraw=False)
                if self.zoom_job is not None:
                    self.root.after_cancel(self.zoom_job)
                self.zoom_job = self.root.after(150, self.render_zoom)
                self.status.set(f"{'主视角' if kind == 'main' else '点云小窗'}缩放 {zoom:.0%}")
            return "break"

    def render_zoom(self):
        self.zoom_job = None
        if not self.busy and self.frame is not None:
            self.start_render(True)

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
