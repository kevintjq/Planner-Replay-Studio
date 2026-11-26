# Planner Replay Studio

Windows tools for replaying ROS1 SLAM point clouds and composing drone flight videos.

English · [简体中文](README_CN.md)

## Windows native preview

1. [Download PlannerReplayNative-Setup.exe](https://raw.githubusercontent.com/kevintjq/Planner-Replay-Studio/windows-downloads/PlannerReplayNative-Setup.exe).
2. Double-click the installer and launch the app on completion. Future launches are available from **Planner Replay Studio** in the Start menu.
3. Open a ROS1 `.bag` or choose the built-in demo.

The installer bundles the C++ / Qt Quick runtime. Use Windows x64 with an OpenGL 3.3-compatible graphics driver. [Native build and usage guide](native/README.md)

![Native replay with a sample ROS1 bag](docs/native-preview.png)

## Choose a workspace

| | Native preview | Video composition workspace |
| --- | --- | --- |
| Launch | Run the setup EXE, then use the Start menu | [`Start_Planner_Replay.cmd`](Start_Planner_Replay.cmd), with Python 3.10+ and Tcl/Tk |
| Replay | `PointCloud2`, accumulated or replaced maps, distance colors, flown path and `Odometry` speed | Multi-view replay, TF and selectable overlays for EGO and SUPER |
| Editing and export | Play, seek and inspect point clouds | Multi-video sync, timed overlays, motion-ghost stills and 1080p/60 fps export |

For filming and editing, see the [video composition guide](docs/studio.md).

## Repository layout

| Path | Contents |
| --- | --- |
| [`native/`](native/) | C++ / Qt Quick desktop app, ROS1 bag reader and native tests |
| [`src/slam_fpv/`](src/slam_fpv/) | Python video composition workspace and CLI |
| [`tests/`](tests/) | Python workspace tests |
| [`docs/`](docs/) | User guides, screenshots and [validation notes](docs/VALIDATION.md) |
| [`examples/`](examples/) | ROS1 bridge, extrinsics template and bag validation |

[MIT License](LICENSE)
