"""Light desktop styling for the renderer and alignment editor."""

import sys
from tkinter import ttk

from PIL import Image, ImageDraw, ImageTk


SHELL = "#EAEDF3"
PANEL = "#FFFFFF"
INK = "#20232C"
MUTED = "#5F6878"
BLUE = "#1769DE"
SURFACE = "#F4F6FA"
HAIRLINE = "#DCE3EE"
SELECTED = "#E3EEFF"
PREVIEW = "#131924"
FOCUS = "#69DCFF"


def desktop_style(root):
    style = ttk.Style(root)
    style.theme_use("clam")
    root.configure(background=SHELL)
    root.option_add("*Menu.background", PANEL)
    root.option_add("*Menu.foreground", INK)
    root.option_add("*Menu.activeBackground", SELECTED)
    root.option_add("*Menu.activeForeground", INK)
    font = "Microsoft YaHei UI" if sys.platform == "win32" else "Noto Sans CJK SC"
    style.configure(".", font=(font, 10), background=PANEL, foreground=INK)
    style.configure("Shell.TFrame", background=SHELL)
    style.configure("Shell.TLabel", background=SHELL, foreground=MUTED)
    style.configure("ShellSection.TLabel", background=SHELL, foreground=INK, font=(font, 11, "bold"))
    style.configure("Title.TLabel", background=SHELL, foreground=INK, font=(font, 24, "bold"))
    style.configure("Subtitle.TLabel", background=SHELL, foreground=MUTED, font=(font, 10))
    style.configure("PanelTitle.TLabel", font=(font, 13, "bold"), foreground=INK)
    style.configure("Quiet.TLabel", foreground=MUTED, font=(font, 9))
    style.configure("LayerReady.TLabel", foreground="#17734A", font=(font, 9))
    style.configure("LayerWarning.TLabel", foreground="#9B4E13", font=(font, 9))
    style.configure("TNotebook", background=SHELL, borderwidth=0, tabmargins=(0, 0, 0, 5))
    style.configure("TNotebook.Tab", padding=(11, 5), borderwidth=0, background=SHELL, foreground=MUTED)
    style.map("TNotebook.Tab", background=[("selected", PANEL), ("active", "#E7E9EE")],
              foreground=[("selected", BLUE)])
    for widget in ("TEntry", "TCombobox"):
        style.configure(widget, padding=5, bordercolor=HAIRLINE, lightcolor=PANEL, darkcolor=PANEL)
        style.map(widget, bordercolor=[("focus", BLUE)], fieldbackground=[("readonly", SURFACE)])
    style.configure("TCombobox", arrowcolor=MUTED, arrowsize=12)
    style.configure("TProgressbar", background=BLUE, troughcolor=HAIRLINE, borderwidth=0)
    style.configure("Treeview", borderwidth=0, rowheight=32, background=PANEL, fieldbackground=PANEL)
    style.configure("Treeview.Heading", relief="flat", background=SURFACE, foreground=MUTED,
                    padding=(8, 5), font=(font, 9, "bold"))
    style.map("Treeview.Heading", background=[("active", HAIRLINE), ("pressed", SELECTED)])
    style.map("Treeview", background=[("selected", SELECTED)], foreground=[("selected", "#124B9B")])
    style.configure("TScale", troughcolor=HAIRLINE, background=BLUE, lightcolor=BLUE, darkcolor=BLUE,
                    bordercolor=BLUE, sliderrelief="flat", sliderlength=14)
    style.configure("TScrollbar", troughcolor=PANEL, background="#C9D3E2", bordercolor=PANEL,
                    lightcolor="#C9D3E2", darkcolor="#C9D3E2", arrowcolor=MUTED, arrowsize=11)

    photos = []

    def checkbox(border, fill, mark=False):
        image = Image.new("RGBA", (64, 64))
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle((4, 4, 59, 59), radius=12, fill=fill, outline=border, width=5)
        if mark:
            draw.line(((16, 32), (27, 43), (47, 21)), fill="#FFFFFF", width=8, joint="curve")
        photos.append(ImageTk.PhotoImage(image.resize((16, 16), Image.Resampling.LANCZOS), master=root))
        return str(photos[-1])

    empty = checkbox("#9299A5", PANEL)
    checked = checkbox(BLUE, BLUE, True)
    disabled = checkbox("#CDD1D7", "#F2F3F6")
    disabled_checked = checkbox("#9CB7D9", "#9CB7D9", True)
    style.element_create("Workspace.check", "image", empty,
                         ("disabled", "selected", disabled_checked),
                         ("disabled", disabled), ("selected", checked), width=21, sticky="w")
    style.layout("TCheckbutton", [("Checkbutton.padding", {"sticky": "nswe", "children": [
        ("Workspace.check", {"side": "left", "sticky": "w"}),
        ("Checkbutton.focus", {"side": "left", "sticky": "w", "children": [
            ("Checkbutton.label", {"sticky": "nswe"}),
        ]}),
    ]})])

    def chip(fill, size=24):
        image = Image.new("RGBA", (64, 64))
        ImageDraw.Draw(image).rounded_rectangle((1, 1, 62, 62), radius=23, fill=fill)
        photos.append(ImageTk.PhotoImage(image.resize((size, size), Image.Resampling.LANCZOS)))
        return str(photos[-1])
    card = chip(PANEL, 42)
    style.element_create("Workspace.card", "image", card, border=15, sticky="nsew")
    style.layout("Workspace.TFrame", [("Workspace.card", {"sticky": "nsew"})])
    style.layout("TNotebook", [("Workspace.card", {"sticky": "nsew"})])
    style.layout("Workbench.TNotebook", [("Workspace.card", {"sticky": "nsew"})])
    style.layout("Workbench.TNotebook.Tab", [])
    style.configure("Workbench.TNotebook", tabmargins=0)
    idle, picked, hover = map(chip, (SURFACE, SELECTED, HAIRLINE))
    style.element_create("Workspace.tab", "image", idle, ("selected", picked), ("active", hover),
                         border=8, sticky="nsew")
    style.layout("TNotebook.Tab", [("Workspace.tab", {"sticky": "nsew", "children": [
        ("Notebook.padding", {"sticky": "nsew", "children": [("Notebook.label", {"sticky": "nsew"})]})
    ]})])
    style.layout("Treeview", [("Treeview.treearea", {"sticky": "nsew"})])
    for button, fills, foreground in (
        ("TButton", (SURFACE, "#E8EDF7", "#DCE5F3", "#F3F5F8"), INK),
        ("Primary.TButton", (BLUE, "#135CC7", "#104DA6", "#DCE8F9"), "#FFFFFF"),
    ):
        idle, hover, press, disabled = map(chip, fills)
        element = button.split(".")[0] + ".pill"
        style.element_create(element, "image", idle, ("disabled", disabled), ("pressed", press), ("active", hover),
                             border=8, sticky="nsew")
        style.layout(button, [(element, {"sticky":"nsew", "children":[
            ("Button.padding", {"sticky":"nsew", "children":[("Button.label", {"sticky":"nsew"})]})
        ]})])
        style.configure(button, padding=(11, 5), foreground=foreground)
        style.map(button, foreground=[("disabled", "#848489"), ("!disabled", foreground)])
    idle, selected, hover, pressed = map(chip, (SHELL, "#FFFFFF", "#E0E6F0", SELECTED))
    style.element_create("Workspace.segment", "image", idle, ("selected", selected),
                         ("pressed", pressed), ("active", hover), border=8, sticky="nsew")
    style.layout("Segment.TButton", [("Workspace.segment", {"sticky": "nsew", "children": [
        ("Button.padding", {"sticky": "nsew", "children": [("Button.label", {"sticky": "nsew"})]})
    ]})])
    style.configure("Segment.TButton", padding=(9, 7), foreground=MUTED, font=(font, 10, "bold"))
    style.map("Segment.TButton", foreground=[("selected", BLUE), ("active", INK)])
    style.configure("Shell.TButton", background=SHELL)
    style.configure("Shell.Primary.TButton", background=SHELL)
    return font, photos
