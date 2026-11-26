"""Layer inspector for the desktop compositor."""

from dataclasses import replace
from math import isfinite
from pathlib import Path
import tkinter as tk
from tkinter import colorchooser, filedialog, messagebox, ttk

from .composition import POINT_CLOUD_VIEWS, VIEW_OVERLAYS, PointCloudView, TextLayer, VideoLayer, view_overlays
from .footage import VideoEdit
from .timeline import Track


MATTE_OPTIONS = ("透明底", "深色底")


def background_field(enabled):
    return ("background", "图层底色", MATTE_OPTIONS[0 if enabled else 1], MATTE_OPTIONS)


VIDEO_FILES = (("视频", "*.mp4 *.mov *.mkv *.avi *.m4v"), ("所有文件", "*"))


class LayerControls:
    def make_layer_panel(self, parent):
        self.layer_panel = parent
        parent.columnconfigure(0, weight=1)
        ttk.Label(parent, text="叠加图层", style="PanelTitle.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
        toolbar = ttk.Frame(parent)
        toolbar.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(8, 7))
        ttk.Button(toolbar, text="+ 视频", command=self.add_videos).pack(side="left")
        ttk.Button(toolbar, text="+ 文字", command=self.edit_text).pack(side="left", padx=4)
        ttk.Button(toolbar, text="+ 速度表", command=self.edit_speed).pack(side="left")
        self.add_view_button = ttk.Button(toolbar, text="+ 视角", command=self.choose_view)
        self.add_view_button.pack(side="left", padx=(4,0))
        table = ttk.Frame(parent)
        table.grid(row=2, column=0, columnspan=2, sticky="ew")
        self.layer_list = ttk.Treeview(table, show="tree", height=7, selectmode="browse", takefocus=True)
        self.layer_list.column("#0", width=270, stretch=True)
        self.layer_list.pack(side="left", fill="both", expand=True)
        self.layer_scroll = ttk.Scrollbar(table, orient="vertical", command=self.layer_list.yview)
        self.layer_list.configure(yscrollcommand=self.layer_scroll.set)
        self.layer_list.bind("<<TreeviewSelect>>", self.read_selection)
        self.layer_list.bind("<Double-1>", lambda _: self.edit_selected())
        self.layer_list.tag_configure("primary", foreground="#0066CC")
        self.layer_info = tk.StringVar(value="选择一个图层，或在画面里直接拖动。")
        ttk.Label(parent, textvariable=self.layer_info, style="Quiet.TLabel", wraplength=275).grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(9, 7))
        tools = ttk.Frame(parent)
        tools.grid(row=4, column=0, columnspan=2, sticky="w")
        self.layer_settings = ttk.Button(tools, text="设置", command=self.edit_selected)
        self.layer_settings.pack(side="left")
        self.layer_align = ttk.Button(tools, text="裁剪 / 对齐", command=self.open_video_editor)
        self.layer_align.pack(side="left", padx=4)
        self.layer_remove = ttk.Button(tools, text="移除", command=self.remove_layer)
        self.layer_remove.pack(side="left")
        self.layer_matte = ttk.Button(tools, text="设为深色底", command=self.toggle_layer_background)
        self.layer_matte.pack(side="left", padx=(5, 0))
        video_tools = ttk.Frame(parent)
        video_tools.grid(row=5, column=0, columnspan=2, sticky="w", pady=(5,0))
        self.layer_replace = ttk.Button(video_tools, text="替换视频", command=self.replace_video)
        self.layer_replace.pack(side="left")
        self.layer_backdrop = ttk.Button(video_tools, text="设为全屏底片", command=self.toggle_backdrop)
        self.layer_backdrop.pack(side="left", padx=4)
        self.layer_down = ttk.Button(video_tools, text="置后", command=lambda: self.move_video_layer(-1))
        self.layer_up = ttk.Button(video_tools, text="置前", command=lambda: self.move_video_layer(1))
        self.layer_down.pack(side="left")
        self.layer_up.pack(side="left", padx=(3,0))

    @staticmethod
    def layer_id(key):
        return f"{key[0]}-{key[1]}"

    def layers(self, kind):
        if kind == "main":
            return (self.project.main_view,)
        return getattr(self.project, {"video":"videos", "text":"texts", "view":"views"}[kind])

    def replace_layer(self, kind, index, layer, *, redraw=True):
        if kind == "main":
            self.project = replace(self.project, main_view=layer)
        else:
            values = list(self.layers(kind))
            values[index] = layer
            name = {"video":"videos", "text":"texts", "view":"views"}[kind]
            self.project = replace(self.project, **{name:tuple(values)})
        if redraw:
            self.refresh_layers()

    def invalidate_video(self, index):
        frames = list(self.footages)
        frames[index] = None
        self.footages = tuple(frames)
        self.draw_preview()

    def refresh_layers(self):
        listing = self.layer_list
        listing.delete(*listing.get_children())
        listing.insert("", "end", iid="main-0",
                       text=f"主视角   {POINT_CLOUD_VIEWS[self.project.main_view.kind][0]}", tags=("primary",))
        for i, layer in enumerate(self.project.videos):
            role = "全屏底片" if layer.backdrop else f"视频 {i+1}"
            listing.insert("", "end", iid=f"video-{i}", text=f"{role}   {layer.path.name}")
        for i, view in enumerate(self.project.views):
            listing.insert("", "end", iid=f"view-{i}", text=f"点云视角   {POINT_CLOUD_VIEWS[view.kind][0]}")
        self.add_view_button.state(["disabled" if len(self.project.views) == len(POINT_CLOUD_VIEWS) else "!disabled"])
        for i, layer in enumerate(self.project.texts):
            name = " ".join(layer.text.split())
            listing.insert("", "end", iid=f"text-{i}", text=f"文字 {i+1}   {name}")
        if self.project.show_speed:
            listing.insert("", "end", iid="speed-0", text="速度表   m/s")
        if len(listing.get_children()) > 7:
            self.layer_scroll.pack(side="right", fill="y")
        else:
            self.layer_scroll.pack_forget()
        if self.selection is not None and listing.exists(self.layer_id(self.selection)):
            listing.selection_set(self.layer_id(self.selection))
            listing.see(self.layer_id(self.selection))
        else:
            self.selection = None
        self.show_layer()

    def select_layer(self, key):
        self.selection = key
        self.layer_list.selection_set(self.layer_id(key))
        self.layer_list.see(self.layer_id(key))
        self.nav.select(self.layer_panel)
        self.show_layer()

    def read_selection(self, _event):
        if self.busy:
            return
        selected = self.layer_list.selection()
        if selected:
            kind, index = selected[0].split("-")
            self.selection = kind, int(index)
            self.show_layer()

    def show_layer(self):
        selection = self.selection
        kind, i = selection or ("", 0)
        self.layer_settings.state(["!disabled" if selection else "disabled"])
        self.layer_align.state(["!disabled" if kind == "video" else "disabled"])
        self.layer_remove.state(["!disabled" if selection and kind != "main" else "disabled"])
        self.layer_matte.state(["!disabled" if selection else "disabled"])
        if selection:
            transparent = self.project.speed_transparent if kind == "speed" else self.layers(kind)[i].transparent
            self.layer_matte.configure(text="设为深色底" if transparent else "设为透明底")
        video_selected = kind == "video"
        for control in (self.layer_replace, self.layer_backdrop):
            control.state(["!disabled" if video_selected else "disabled"])
        self.layer_down.state(["!disabled" if video_selected and i > 0 else "disabled"])
        self.layer_up.state(["!disabled" if video_selected and i+1 < len(self.project.videos) else "disabled"])
        backdrop = video_selected and self.project.videos[i].backdrop
        self.layer_backdrop.configure(text="恢复画中画" if backdrop else "设为全屏底片")
        message = "选择一个图层，或直接拖动画面中的图层。"
        if kind == "video":
            layer = self.project.videos[i]
            n = len(layer.edit.anchors)
            placement = f"底片 {layer.backdrop_scale:.0%}" if layer.backdrop else f"宽 {layer.width:.0%}"
            message = f"{placement}  ·  不透明度 {layer.opacity:.0%}  ·  " + (
                f"{n} 个对齐锚点" if n else f"偏移 {layer.edit.offset:+.3f} s")
        elif kind in {"view", "main"}:
            view = self.layers(kind)[i]
            style = MATTE_OPTIONS[0 if view.transparent else 1]
            message = f"{POINT_CLOUD_VIEWS[view.kind][0]}  ·  宽 {view.width:.0%}  ·  {style}"
        elif kind == "text":
            layer = self.project.texts[i]
            message = f"{layer.start:.3f} – {layer.end:.3f} s  ·  字号 {layer.size}"
        elif kind == "speed":
            message = "当前、最高、平均速度从导出起点累计，数据来自世界坐标系位姿。"
        self.layer_info.set(message)
        self.draw_preview()
        self.update_timeline()

    def toggle_layer_background(self):
        if not self.selection:
            return
        kind, index = self.selection
        if kind == "speed":
            self.project = replace(self.project, speed_transparent=not self.project.speed_transparent)
            self.refresh_layers()
        else:
            layer = self.layers(kind)[index]
            self.replace_layer(kind, index, replace(layer, transparent=not layer.transparent))

    def choose_view(self):
        selected = {view.kind for view in self.project.views}
        available = {title: kind for kind, (title, _) in POINT_CLOUD_VIEWS.items() if kind not in selected}
        if not available:
            return
        options = tuple(available)
        self.layer_dialog("添加点云视角", (("view", "视角", options[0], options),),
                          lambda values: self.add_view(available[values["view"]]))

    def add_view(self, kind):
        order = len(self.project.views)
        layer = PointCloudView(kind=kind, position=POINT_CLOUD_VIEWS[kind][1], width=.26)
        self.project = replace(self.project, views=(*self.project.views, layer))
        self.selection = "view", order
        self.refresh_layers()
        if self.frame is not None and kind not in self.cloud_views:
            self.root.after_idle(lambda: self.start_render(True))

    def add_videos(self):
        paths = filedialog.askopenfilenames(parent=self.root, title="导入多路视频",
                                            filetypes=VIDEO_FILES)
        if not paths:
            return
        spots = ((.05,.82), (.50,.82), (.95,.82), (.96,.44), (.05,.44))
        start = len(self.project.videos)
        layers = tuple(VideoLayer(Path(path), width=.27, position=spots[(start+i) % len(spots)])
                       for i, path in enumerate(paths))
        self.project = replace(self.project, videos=(*self.project.videos,*layers))
        self.footages = (*self.footages, *(None for _ in layers))
        self.selection = "video", start
        self.refresh_layers()
        if self.frame is not None:
            self.start_render(True)

    def replace_video(self):
        if self.busy or not self.selection or self.selection[0] != "video":
            return
        index = self.selection[1]
        layer = self.project.videos[index]
        file = filedialog.askopenfilename(parent=self.root, title="替换视频", filetypes=VIDEO_FILES)
        if file and Path(file) != layer.path:
            self.invalidate_video(index)
            self.replace_layer("video", index, replace(layer, path=Path(file), edit=VideoEdit()))
            if self.frame is not None:
                self.start_render(True)

    def toggle_backdrop(self):
        if self.busy or not self.selection or self.selection[0] != "video":
            return
        index = self.selection[1]
        enabled = not self.project.videos[index].backdrop
        videos = tuple(replace(layer, backdrop=enabled and i == index)
                       for i, layer in enumerate(self.project.videos))
        self.project = replace(self.project, videos=videos)
        self.refresh_layers()

    def move_video_layer(self, direction):
        if self.busy or not self.selection or self.selection[0] != "video":
            return
        index = self.selection[1]
        target = index + direction
        if not 0 <= target < len(self.project.videos):
            return
        videos = list(self.project.videos)
        frames = list(self.footages)
        videos[index], videos[target] = videos[target], videos[index]
        frames[index], frames[target] = frames[target], frames[index]
        self.project = replace(self.project, videos=tuple(videos))
        self.footages = tuple(frames)
        self.selection = "video", target
        self.refresh_layers()

    def remove_layer(self):
        if not self.selection or self.selection[0] == "main":
            return
        kind, index = self.selection
        self.selection = None
        if kind == "speed":
            self.project = replace(self.project, show_speed=False)
        elif kind == "video":
            removed = self.project.videos[index].id
            self.project = replace(self.project, videos=self.project.videos[:index]+self.project.videos[index+1:],
                                   layouts=tuple(cue for cue in self.project.layouts if cue.video_id != removed),
                                   audio_id=None if self.project.audio_id == removed else self.project.audio_id)
            self.footages = self.footages[:index]+self.footages[index+1:]
        elif kind == "view":
            self.project = replace(self.project, views=self.project.views[:index]+self.project.views[index+1:])
        else:
            self.project = replace(self.project, texts=self.project.texts[:index]+self.project.texts[index+1:])
        self.refresh_layers()

    def edit_selected(self):
        if not self.selection:
            return
        kind, index = self.selection
        if kind == "text":
            self.edit_text(index)
        elif kind == "speed":
            self.edit_speed()
        elif kind in {"view", "main"}:
            self.edit_cloud_view(kind, index)
        else:
            layer = self.project.videos[index]
            def save(values):
                edit = replace(layer.edit, offset=float(values["offset"]))
                result = replace(layer, width=float(values.get("width", layer.width*100))/100,
                                 opacity=float(values["opacity"])/100, edit=edit,
                                 backdrop_scale=float(values.get("scale", layer.backdrop_scale*100))/100,
                                 transparent=values["background"] == MATTE_OPTIONS[0])
                self.replace_layer(kind, index, result)
                if edit != layer.edit:
                    self.invalidate_video(index)
                    if self.frame is not None:
                        self.root.after_idle(lambda: self.start_render(True))
            size_field = (("scale", "底片缩放 (%)", f"{layer.backdrop_scale*100:g}") if layer.backdrop
                          else ("width", "画面宽度 (%)", f"{layer.width*100:g}"))
            self.layer_dialog("视频图层设置", (size_field, background_field(layer.transparent),
                ("opacity", "不透明度 (%)", f"{layer.opacity*100:g}"),
                ("offset", "手动偏移 (s)", str(layer.edit.offset)),
            ), save, note="有对齐锚点时优先使用锚点。")

    def edit_cloud_view(self, kind, index):
        if self.busy:
            return
        layer = self.layers(kind)[index]
        occupied = {view.kind for i, view in enumerate(self.project.views) if i != index} if kind == "view" else set()
        choices = {title: mode for mode, (title, _) in POINT_CLOUD_VIEWS.items() if mode not in occupied}

        visible = view_overlays(layer.kind, layer.overlays)
        switches = {key: (label, key in visible) for key, label in VIEW_OVERLAYS.items()}

        def save(values):
            mode = choices[values["camera"]]
            selected = tuple(key for key in VIEW_OVERLAYS if values[key])
            overlays = layer.overlays if set(selected) == visible else selected
            change = replace(layer, kind=mode, width=float(values["width"])/100, overlays=overlays,
                             zoom=float(values["zoom"])/100, transparent=values["background"] == MATTE_OPTIONS[0])
            self.replace_layer(kind, index, change)
            if self.frame is not None and (mode != layer.kind or overlays != layer.overlays or change.zoom != layer.zoom):
                self.root.after_idle(lambda: self.start_render(True))

        self.layer_dialog("主视角设置" if kind == "main" else "点云小窗设置", (
            ("camera", "机位", POINT_CLOUD_VIEWS[layer.kind][0], tuple(choices)),
            ("width", "图层宽度 (%)", f"{layer.width*100:g}"),
            ("zoom", "相机缩放 (%)", f"{layer.zoom*100:g}"),
            background_field(layer.transparent),
        ), save, switches=switches, note="预览中也可用滚轮缩放。叠加项由本视角与「空间叠加」共同控制。")

    def edit_text(self, index=None):
        if index is None:
            start, duration = self.timeline_range()
            begin = self.elapsed if self.elapsed is not None else start
            layer = TextLayer("实验场景", begin, max(begin+.1, min(begin+3, start+duration)))
        else:
            layer = self.project.texts[index]

        def save(values):
            new = replace(layer, text=values["text"], start=float(values["start"]), end=float(values["end"]),
                          size=int(values["size"]), color=values["color"],
                          transparent=values["background"] == MATTE_OPTIONS[0])
            if index is None:
                self.selection = "text", len(self.project.texts)
                self.project = replace(self.project, texts=(*self.project.texts, new))
                self.refresh_layers()
            else:
                self.replace_layer("text", index, new)

        self.layer_dialog("文字图层", (
            ("text", "文字", "" if index is None else layer.text),
            ("start", "开始时间 (s)", f"{layer.start:.3f}"),
            ("end", "结束时间 (s)", f"{layer.end:.3f}"),
            ("size", "字号", str(layer.size)),
            ("color", "颜色", layer.color),
            background_field(layer.transparent),
        ), save)

    def edit_speed(self):
        p = self.project
        def save(values):
            self.project = replace(p, show_speed=True, speed_limit=float(values["limit"]),
                                   speed_start=float(values["start"]),
                                   speed_scale=float(values["scale"])/100,
                                   speed_transparent=values["background"] == MATTE_OPTIONS[0],
                                   speed_end=float(values["end"]) if values["end"] else None)
            self.selection = "speed", 0
            self.refresh_layers()
        self.layer_dialog("飞行速度表", (
            ("limit", "表盘上限 (m/s)", f"{p.speed_limit:g}"),
            ("scale", "缩放 (%)", f"{p.speed_scale*100:g}"),
            background_field(p.speed_transparent),
            ("start", "开始时间 (s)", f"{p.speed_start:g}"),
            ("end", "结束时间 (s)", "" if p.speed_end is None else f"{p.speed_end:g}"),
        ), save, note="结束时间留空时显示至片段结束。")

    def layer_dialog(self, title, fields, save, *, note="", switches=None):
        modal = tk.Toplevel(self.root)
        modal.title(title)
        modal.transient(self.root)
        modal.resizable(False, False)
        box = ttk.Frame(modal, padding=18)
        box.pack(fill="both", expand=True)
        variables = {}
        first = None
        for row, (name, label, default, *options) in enumerate(fields):
            var = variables[name] = tk.StringVar(value=default)
            ttk.Label(box, text=label).grid(row=row, column=0, sticky="w", padx=(0, 10), pady=4)
            if options:
                entry = ttk.Combobox(box, textvariable=var, values=options[0], width=26, state="readonly")
            else:
                entry = ttk.Entry(box, textvariable=var, width=28)
            entry.grid(row=row, column=1, sticky="ew")
            if first is None:
                first = entry
            if name == "color":
                def choose():
                    _, chosen = colorchooser.askcolor(variables["color"].get(), parent=modal)
                    if chosen:
                        variables["color"].set(chosen)
                ttk.Button(box, text="选色", command=choose).grid(row=row, column=2, padx=(6,0))
        flags = {}
        row = len(fields)
        if switches:
            section = ttk.LabelFrame(box, text="此视角的叠加项", padding=(8, 6))
            section.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(10, 0))
            for i, (name, (label, enabled)) in enumerate(switches.items()):
                flag = flags[name] = tk.BooleanVar(value=enabled)
                ttk.Checkbutton(section, text=label, variable=flag).grid(
                    row=i//2, column=i%2, sticky="w", padx=(0, 10), pady=2)
            row += 1

        def confirm(_event=None):
            try:
                save({**{key: var.get().strip() for key,var in variables.items()},
                      **{key: flag.get() for key, flag in flags.items()}})
            except ValueError as error:
                messagebox.showerror("检查图层设置", str(error), parent=modal)
                return
            modal.destroy()
        if note:
            ttk.Label(box, text=note, style="Quiet.TLabel").grid(row=row, column=0, columnspan=3, sticky="w", pady=7)
        ttk.Button(box, text="应用", style="Primary.TButton", command=confirm).grid(
            row=row+1, column=1, sticky="e", pady=(12,0))
        modal.bind("<Return>", confirm)
        modal.bind("<Escape>", lambda _: modal.destroy())
        modal.wait_visibility()
        modal.grab_set()
        first.focus_set()

    def timeline_range(self):
        try:
            start, duration = (float(self.values[key].get()) for key in ("start","seconds"))
            if start >= 0 and duration > 0 and isfinite(start) and isfinite(duration):
                return start,duration
        except ValueError:
            pass
        return 0., 6.

    def update_timeline(self):
        start, duration = self.timeline_range()
        tracks = []
        for i, layer in enumerate(self.project.videos):
            clip = layer.edit
            media_end = clip.trim_out
            if (length := self.clip_durations.get(layer.id)) is not None:
                media_end = length if media_end is None else min(length, media_end)
            last = start+duration if media_end is None else clip.bag_time(media_end)
            first = clip.bag_time(clip.trim_in)
            tracks.append(Track(("video", i), first, max(first, last), f"{i+1}. {layer.path.name}"))
        tracks.extend(Track(("text", i), layer.start, layer.end, " ".join(layer.text.split()))
                      for i, layer in enumerate(self.project.texts))
        if self.project.show_speed:
            tracks.append(Track(("speed", 0), self.project.speed_start, self.project.speed_end or start+duration, "速度表"))
        self.timeline.update_view(start, duration, self.elapsed if self.elapsed is not None else start,
                                  tuple(tracks), self.selection, int(self.values["fps"].get()),
                                  self.timeline_events, self.project.layouts, self.project.markers)
        self.update_event_choices(start, duration)

    def seek_timeline(self, seconds):
        start, _ = self.timeline_range()
        self.values["preview_offset"].set(f"{max(0, seconds-start):.6f}")
        self.start_render(True)

    def apply_interval(self, value):
        if not self.selection:
            return
        start, end = value
        kind, index = self.selection
        if kind == "video":
            layer = self.project.videos[index]
            edit = layer.edit
            first, last = max(0., edit.video_time(start)), edit.video_time(end)
            if (length := self.clip_durations.get(layer.id)) is not None:
                last = min(last, length)
            if last <= first:
                self.update_timeline()
                self.status.set("裁剪范围须保留至少一段视频。")
                return
            # Keep an open-ended source interval if only the start handle moved.
            end_cap = edit.trim_out if edit.trim_out is not None else length
            if end_cap is None:
                end_cap = edit.video_time(sum(self.timeline_range()))
            updated = replace(edit, trim_in=first, trim_out=edit.trim_out if abs(last-end_cap) < 1e-7 else last)
            if edit != updated:
                self.replace_layer(kind, index, replace(layer, edit=updated))
                self.invalidate_video(index)
                if self.frame is not None:
                    self.start_render(True)
        elif kind == "text":
            self.replace_layer(kind, index, replace(self.project.texts[index], start=max(0.,start), end=end))
        elif kind == "speed":
            self.project = replace(self.project, speed_start=max(0.,start), speed_end=end)
            self.refresh_layers()
