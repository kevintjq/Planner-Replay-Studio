"""Topic catalog, 3D display configuration, and recorded message inspection."""
from dataclasses import replace
import tkinter as tk
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, ttk

from PIL import ImageTk

from .topic_displays import DISPLAY_COLORS, DISPLAY_TYPES, INSPECTOR_TYPES, TopicDisplay, supported_displays
from .rviz_config import read_rviz
from .topic_inspection import image_preview, message_children, message_summary, read_topic_at


class TopicPanel(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent, padding=(8, 6))
        self.app = app
        global_options = ttk.Frame(self)
        global_options.pack(fill="x", pady=(0, 6))
        ttk.Label(global_options, text="固定坐标系").pack(side="left", padx=(0, 4))
        self.fixed_frame = ttk.Combobox(global_options, width=12, state="readonly")
        self.fixed_frame.pack(side="left")
        self.fixed_frame.bind("<<ComboboxSelected>>", self.set_fixed_frame)
        self.show_main = tk.BooleanVar(value=True)
        ttk.Checkbutton(global_options, text="主点云", variable=self.show_main,
                        command=self.set_main_cloud).pack(side="left", padx=5)
        ttk.Button(global_options, text="导入 .rviz", command=self.import_rviz).pack(side="right")
        bar = ttk.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        for label, action in (("+ 按话题添加", self.choose), ("设置", self.edit), ("启用 / 停用", self.toggle),
                              ("查看消息", self.inspect), ("删除", self.remove)):
            ttk.Button(bar, text=label, command=action).pack(side="left", padx=(0, 3))
        self.table = ttk.Treeview(self, columns=("kind", "state"), show="tree headings", selectmode="browse")
        self.table.heading("#0", text="话题")
        self.table.heading("kind", text="显示")
        self.table.heading("state", text="状态")
        self.table.column("#0", width=185)
        self.table.column("kind", width=105, stretch=False)
        self.table.column("state", width=94, stretch=False)
        scroll = ttk.Scrollbar(self, orient="vertical", command=self.table.yview)
        self.table.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.table.pack(fill="both", expand=True)
        self.table.bind("<Double-1>", lambda _: self.inspect())

    def refresh(self):
        world = self.app.values.get("world")
        current = world.get() if world is not None else ""
        requested = self.app.project.display_fixed_frame
        choices = tuple(self.app.world_frames.cget("values")) if hasattr(self.app, "world_frames") else ()
        if self.fixed_frame.cget("values") != choices:
            self.fixed_frame.configure(values=choices)
        frame = current if self.app.bag_path else requested
        if self.fixed_frame.get() != frame:
            self.fixed_frame.set(frame)
        if self.show_main.get() != self.app.project.show_main_cloud:
            self.show_main.set(self.app.project.show_main_cloud)
        displays = self.app.project.topic_displays
        expected = tuple(display.id for display in displays)
        if self.table.get_children() != expected:
            selected = self.table.selection()
            self.table.delete(*self.table.get_children())
            for ident in expected:
                self.table.insert("", "end", iid=ident)
            if selected and selected[0] in expected:
                self.table.selection_set(*selected)
        types = {}
        if self.app.bag_path:
            for row in (self.app.bag_metadata or {}).get("topics", ()):
                types.setdefault(row["topic"], []).append(row["type"])
        for display in displays:
            label, compatible = DISPLAY_TYPES[display.kind]
            available = types.get(display.topic)
            reading = self.app.spatial_diagnostics.get(display.id)
            state = (
                "已停用" if not display.enabled else
                "无话题" if not available else
                "查看字段" if display.kind in INSPECTOR_TYPES and display.kind != "image" else
                "查看图像" if display.kind == "image" and all(kind in compatible for kind in available) else
                "类型不匹配" if not all(kind in compatible for kind in available) else
                reading.label if reading else "待预览"
            )
            values = (label + (" · 主画面" if display.main_only else ""), state)
            title = f"{display.name} · {display.topic}" if display.name else display.topic
            if self.table.item(display.id, "text") != title or self.table.item(display.id, "values") != values:
                self.table.item(display.id, text=title, values=values)

    def update_preview(self):
        if (self.app.bag_path is not None or self.app.demo_selected) and self.app.elapsed is not None:
            self.app.transport.request(self.app.elapsed)

    def set_main_cloud(self):
        if self.app.busy:
            self.refresh()
            return
        self.app.project = replace(self.app.project, show_main_cloud=self.show_main.get())
        self.update_preview()

    def set_fixed_frame(self, _event=None):
        if self.app.busy or not self.app.bag_path:
            self.refresh()
            return
        frame = self.fixed_frame.get()
        self.app.project = replace(self.app.project, display_fixed_frame=frame)
        self.apply_fixed_frame()
        self.update_preview()

    def apply_fixed_frame(self):
        frame = self.app.project.display_fixed_frame
        if not frame or not self.app.bag_path:
            return True
        if frame not in self.app.world_frames.cget("values"):
            return False
        self.app.values["world"].set(frame)
        self.app.sync_spatial()
        return True

    def import_rviz(self):
        if self.app.busy:
            return
        selected = filedialog.askopenfilename(parent=self, title="导入 RViz 显示配置",
                                              filetypes=(("RViz 配置", "*.rviz"), ("YAML", "*.yaml *.yml")))
        if not selected:
            return
        try:
            imported = read_rviz(Path(selected))
        except (ValueError, OSError) as error:
            messagebox.showerror("无法导入 RViz 配置", str(error), parent=self)
            return
        self.app.project = replace(
            self.app.project, topic_displays=imported.displays, display_fixed_frame=imported.fixed_frame,
            show_main_cloud=not any(display.kind == "cloud" and display.enabled for display in imported.displays),
        )
        self.app.spatial_diagnostics.clear()
        applied = self.apply_fixed_frame()
        self.refresh()
        self.update_preview()
        details = [f"已导入 {len(imported.displays)} 个话题显示。"]
        if not applied:
            details.append(f"当前 bag 未识别到固定坐标系 {imported.fixed_frame}，保留当前坐标系。")
        if imported.skipped:
            details.append("未按原插件显示：" + "、".join(imported.skipped))
        self.app.status.set(" ".join(details))

    def selected(self):
        choices = self.table.selection()
        return next((item for item in self.app.project.topic_displays if item.id in choices), None)

    def apply(self, previous, item):
        if self.app.busy:
            return
        displays = tuple(item if display == previous else display for display in self.app.project.topic_displays
                         if display != previous or item is not None)
        if previous is None:
            displays += (item,)
        self.app.project = replace(self.app.project, topic_displays=displays)
        self.app.spatial_diagnostics.clear()
        self.refresh()
        if item is not None:
            self.table.selection_set(item.id)
        if (self.app.bag_path is not None and self.app.frame is not None and
                any(display and display.kind not in INSPECTOR_TYPES for display in (previous, item))):
            self.update_preview()

    def edit(self):
        selected = self.selected()
        if selected is not None:
            self.choose(selected)

    def remove(self):
        selected = self.selected()
        if selected is not None:
            self.apply(selected, None)

    def toggle(self):
        selected = self.selected()
        if selected is not None:
            self.apply(selected, replace(selected, enabled=not selected.enabled))

    def choose(self, previous=None):
        if self.app.busy:
            return
        if not self.app.bag_path:
            self.app.status.set("请先打开 ROS1 bag，再按话题添加显示。")
            return
        box = self.app.open_settings("话题显示设置" if previous else "按话题添加显示")
        opened_project = self.app.project
        opened_bag = self.app.bag_path
        query = tk.StringVar()
        ttk.Label(box, text="按话题名或消息类型搜索").pack(anchor="w")
        search = ttk.Entry(box, textvariable=query)
        search.pack(fill="x", pady=(0, 6))
        catalog = ttk.Treeview(box, columns=("type", "count"), show="tree headings", selectmode="browse", height=10)
        catalog.heading("#0", text="话题")
        catalog.heading("type", text="消息类型")
        catalog.heading("count", text="消息数")
        catalog.column("#0", width=150)
        catalog.column("type", width=140)
        catalog.column("count", width=56, stretch=False)
        catalog.pack(fill="both", expand=True, pady=(5, 8))
        rows = {str(i): row for i, row in enumerate(self.app.bag_metadata["topics"])}
        title_row = ttk.Frame(box)
        title_row.pack(fill="x", pady=(0, 6))
        title = tk.StringVar(value=previous.name if previous else "")
        ttk.Label(title_row, text="显示名称").pack(side="left", padx=(0, 4))
        ttk.Entry(title_row, textvariable=title).pack(side="left", fill="x", expand=True)
        options = ttk.Frame(box)
        options.pack(fill="x")
        kind = ttk.Combobox(options, width=12, state="readonly")
        ttk.Label(options, text="显示为").pack(side="left", padx=(0, 4))
        kind.pack(side="left")
        main_only = tk.BooleanVar(value=previous.main_only if previous else False)
        view = ttk.Checkbutton(options, text="仅主画面", variable=main_only)
        view.pack(side="left", padx=8)
        color = tk.StringVar(value=previous.color if previous else
                             DISPLAY_COLORS[len(self.app.project.topic_displays) % len(DISPLAY_COLORS)])
        swatch = tk.Label(options, width=3, bg=color.get())
        swatch.pack(side="left")

        def pick_color():
            _, chosen = colorchooser.askcolor(color.get(), parent=self.app.root)
            if chosen:
                color.set(chosen)
                swatch.configure(bg=chosen)

        choose_color = ttk.Button(options, text="颜色", command=pick_color)
        choose_color.pack(side="left", padx=5)
        add = ttk.Button(box, text="应用" if previous else "添加", style="Primary.TButton")
        add.pack(anchor="e", pady=(8, 0))
        kinds_by_label = {name: key for key, (name, _) in DISPLAY_TYPES.items()}

        def choice_state(_=None):
            spatial = kinds_by_label.get(kind.get()) not in (None, *INSPECTOR_TYPES)
            for control in (view, choose_color):
                control.state(["!disabled"] if spatial else ["disabled"])

        def select(_=None):
            current = catalog.selection()
            if not current:
                kind.configure(values=())
                kind.set("")
                add.state(["disabled"])
            else:
                row = rows[current[0]]
                kinds = supported_displays(row["type"])
                labels = tuple(DISPLAY_TYPES[entry][0] for entry in kinds)
                kind.configure(values=labels)
                if previous is not None and row["topic"] == previous.topic and previous.kind in kinds:
                    kind.set(DISPLAY_TYPES[previous.kind][0])
                else:
                    kind.current(0)
                add.state(["!disabled"])
            choice_state()

        def filter_topics(*_):
            catalog.delete(*catalog.get_children())
            needle = query.get().strip().casefold()
            matching = []
            for ident, row in rows.items():
                if needle in f"{row['topic']} {row['type']}".casefold():
                    catalog.insert("", "end", iid=ident, text=row["topic"], values=(row["type"], row["count"]))
                    matching.append(ident)
            if matching:
                ident = next((i for i in matching if previous and rows[i]["topic"] == previous.topic), matching[0])
                catalog.selection_set(ident)
                catalog.see(ident)
            select()

        def confirm():
            if self.app.busy:
                return
            if self.app.bag_path != opened_bag or self.app.project != opened_project:
                self.app.status.set("录包或项目已改动，请重新打开话题设置。")
                return
            selection = catalog.selection()
            if not selection or kind.get() not in kinds_by_label:
                return
            changes = dict(topic=rows[selection[0]]["topic"], kind=kinds_by_label[kind.get()],
                           main_only=main_only.get(), color=color.get(), name=title.get().strip())
            item = replace(previous, **changes) if previous else TopicDisplay(**changes)
            self.apply(previous, item)
            self.app.close_settings()
            if item.kind in INSPECTOR_TYPES:
                self.inspect()

        query.trace_add("write", filter_topics)
        kind.bind("<<ComboboxSelected>>", choice_state)
        catalog.bind("<<TreeviewSelect>>", select)
        add.configure(command=confirm)
        filter_topics()
        search.focus_set()

    def inspect(self):
        selected = self.selected()
        if selected is not None and self.app.bag_path and not self.app.busy:
            MessagePanel(self.app, selected.topic).request()


class MessagePanel(tk.Toplevel):
    def __init__(self, app, topic):
        super().__init__(app.root)
        self.app, self.topic, self.path = app, topic, app.bag_path
        self.title(topic)
        self.geometry("680x500")
        toolbar = ttk.Frame(self, padding=8)
        toolbar.pack(fill="x")
        self.time = tk.StringVar(value=f"{app.elapsed or 0.:.3f}")
        ttk.Label(toolbar, text="录包时间 (s)").pack(side="left")
        ttk.Entry(toolbar, textvariable=self.time, width=10).pack(side="left", padx=4)
        ttk.Button(toolbar, text="读取", command=self.request).pack(side="left")
        ttk.Button(toolbar, text="查看播放位置", command=self.use_timeline).pack(side="left", padx=6)
        self.summary = tk.StringVar()
        ttk.Label(self, textvariable=self.summary, padding=(8, 0, 8, 6)).pack(anchor="w")
        self.picture = ttk.Label(self)
        self.photo = None
        self.tree = ttk.Treeview(self, columns=("value",), show="tree headings")
        self.tree.heading("#0", text="字段")
        self.tree.heading("value", text="值")
        self.tree.column("#0", width=210)
        self.tree.column("value", width=420)
        self.tree.bind("<<TreeviewOpen>>", self.expand)
        scroll = ttk.Scrollbar(self, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.tree.pack(fill="both", expand=True, padx=(8, 0), pady=(0, 8))
        self.pending = {}

    def use_timeline(self):
        self.time.set(f"{self.app.elapsed or 0.:.3f}")
        self.request()

    def request(self):
        if self.path != self.app.bag_path:
            self.summary.set("录包已切换，请重新打开话题。")
            return
        if self.app.busy:
            self.summary.set("请暂停预览或等待当前任务结束。")
            return
        try:
            seconds = float(self.time.get())
        except ValueError:
            messagebox.showerror("时间无效", "请输入秒数。", parent=self)
            return
        main = self.app.values["cloud_topic"].get()

        def work():
            result = read_topic_at(self.path, self.topic, seconds, main, self.app.check_cancel)
            return "topic_message", (self, result, image_preview(result) if result else None)

        self.app.run_job(work, f"正在读取 {self.topic}…")

    def show_message(self, message, image=None):
        if not self.winfo_exists():
            return
        self.tree.delete(*self.tree.get_children())
        self.pending.clear()
        self.picture.pack_forget()
        self.photo = None
        if image is not None:
            self.photo = ImageTk.PhotoImage(image, master=self)
            self.picture.configure(image=self.photo)
            self.picture.pack(before=self.tree, pady=(0, 6))
        if message is None:
            self.summary.set("该时刻前没有消息。")
            return
        note = " · 原始字节" if message.raw else ""
        self.summary.set(f"{message.kind} · {message.time:.6f} s{note}")
        root = self.insert("", "消息", message.value)
        self.tree.item(root, open=True)
        self.expand_item(root)

    def insert(self, parent, name, value):
        node = self.tree.insert(parent, "end", text=name, values=(message_summary(value),))
        children = message_children(value)
        first = next(children, None)
        if first is not None:
            self.pending[node] = first, children
            self.tree.insert(node, "end")
        return node

    def expand(self, _event):
        self.expand_item(self.tree.focus())

    def expand_item(self, node):
        waiting = self.pending.pop(node, None)
        if waiting is None:
            return
        self.tree.delete(*self.tree.get_children(node))
        first, children = waiting
        self.insert(node, *first)
        for child in children:
            self.insert(node, *child)
