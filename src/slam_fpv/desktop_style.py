"""Light desktop styling for the renderer and alignment editor."""

import sys
from tkinter import ttk

from PIL import Image, ImageDraw, ImageTk


SHELL = "#EFF2F7"
PANEL = "#FFFFFF"
INK = "#212B3D"
MUTED = "#52647A"
BLUE = "#2165D9"


def desktop_style(root):
    style = ttk.Style(root)
    style.theme_use("clam")
    font = "Microsoft YaHei UI" if sys.platform == "win32" else "Noto Sans CJK SC"
    style.configure(".", font=(font, 10), background=PANEL, foreground=INK)
    style.configure("Shell.TFrame", background=SHELL)
    style.configure("Shell.TLabel", background=SHELL, foreground=MUTED)
    style.configure("Title.TLabel", background=SHELL, foreground=INK, font=(font, 23, "bold"))
    style.configure("Subtitle.TLabel", background=SHELL, foreground=MUTED, font=(font, 10))
    style.configure("PanelTitle.TLabel", font=(font, 12, "bold"), foreground=INK)
    style.configure("Quiet.TLabel", foreground=MUTED, font=(font, 9))
    style.configure("TNotebook", background=SHELL, borderwidth=0, tabmargins=(0, 0, 0, 5))
    style.configure("TNotebook.Tab", padding=(11, 5), borderwidth=0, background=SHELL, foreground=MUTED)
    style.map("TNotebook.Tab", background=[("selected", PANEL), ("active", "#E3EBFA")],
              foreground=[("selected", BLUE)])
    style.configure("TEntry", padding=4, bordercolor="#DFE6F0", lightcolor=PANEL, darkcolor=PANEL)
    style.configure("TCombobox", padding=4, bordercolor="#DFE6F0", lightcolor=PANEL, darkcolor=PANEL)
    style.map("TCombobox", fieldbackground=[("readonly", "#F3F6FC")])
    style.configure("TProgressbar", background=BLUE, troughcolor="#E0E7F2", borderwidth=0)
    style.configure("Treeview", borderwidth=0, rowheight=33, background="#F5F7FB", fieldbackground="#F5F7FB")
    style.map("Treeview", background=[("selected", "#DEEAFE")], foreground=[("selected", "#174589")])

    photos = []

    def chip(fill, size=24):
        image = Image.new("RGBA", (64, 64))
        ImageDraw.Draw(image).rounded_rectangle((1, 1, 62, 62), radius=21, fill=fill)
        photos.append(ImageTk.PhotoImage(image.resize((size, size), Image.Resampling.LANCZOS)))
        return str(photos[-1])
    card = chip(PANEL, 34)
    style.element_create("Workspace.card", "image", card, border=12, sticky="nsew")
    style.layout("Workspace.TFrame", [("Workspace.card", {"sticky": "nsew"})])
    style.layout("TNotebook", [("Workspace.card", {"sticky": "nsew"})])
    idle, picked, hover = map(chip, (SHELL, PANEL, "#E1EAF9"))
    style.element_create("Workspace.tab", "image", idle, ("selected", picked), ("active", hover),
                         border=8, sticky="nsew")
    style.layout("TNotebook.Tab", [("Workspace.tab", {"sticky": "nsew", "children": [
        ("Notebook.padding", {"sticky": "nsew", "children": [("Notebook.label", {"sticky": "nsew"})]})
    ]})])
    style.layout("Treeview", [("Treeview.treearea", {"sticky": "nsew"})])
    for button, fills, foreground in (
        ("TButton", ("#EAF0FA", "#DFE9FA", "#D4E2F8", "#EEF1F6"), INK),
        ("Primary.TButton", (BLUE, "#1C59C6", "#164DB0", "#8DA8CD"), "#FFFFFF"),
    ):
        idle, hover, press, disabled = map(chip, fills)
        element = button.split(".")[0] + ".pill"
        style.element_create(element, "image", idle, ("disabled", disabled), ("pressed", press), ("active", hover),
                             border=8, sticky="nsew")
        style.layout(button, [(element, {"sticky":"nsew", "children":[
            ("Button.padding", {"sticky":"nsew", "children":[("Button.label", {"sticky":"nsew"})]})
        ]})])
        style.configure(button, padding=(11, 5), foreground=foreground)
        style.map(button, foreground=[("disabled", "#68778E"), ("!disabled", foreground)])
    style.configure("Shell.TButton", background=SHELL)
    style.configure("Shell.Primary.TButton", background=SHELL)
    return font, photos
