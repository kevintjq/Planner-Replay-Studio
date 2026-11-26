# Planner Replay Studio

Windows tools for replaying ROS1 SLAM point clouds and composing drone flight videos.

English · [简体中文](README_CN.md)

## Complete Windows workspace

[Download the complete Windows workspace installer](https://github.com/kevintjq/Planner-Replay-Studio/releases/download/studio-v0.2.0/PlannerReplayStudio-Setup.exe). Install it and open **Planner Replay Studio Full** from the Start menu.

The installer bundles Python and Tk. It includes video alignment, planner overlays, motion ghosts, existing `.slamfpv` projects and 1080p/60 fps export.

See the [video composition guide](docs/studio.md).

## C++ native preview

1. [Download PlannerReplayNative-Setup.exe](https://github.com/kevintjq/Planner-Replay-Studio/releases/download/native-v0.1.1/PlannerReplayNative-Setup.exe).
2. Double-click the installer and launch the app on completion. Future launches are available from **Planner Replay Studio** in the Start menu.
3. Open a ROS1 `.bag` or choose the built-in demo.

The installer bundles the C++ / Qt Quick runtime. Use Windows x64 with an OpenGL 3.3-compatible graphics driver. [Native build and usage guide](native/README.md)

![Native replay with a sample ROS1 bag](docs/native-preview.png)

## Choose a workspace

| | Native preview | Video composition workspace |
| --- | --- | --- |
| Launch | Run the setup EXE, then use the Start menu | [Install the complete workspace](#complete-windows-workspace) |
| Replay | `PointCloud2`, accumulated or replaced maps, distance colors, flown path and `Odometry` speed | Multi-view replay, TF and selectable overlays for EGO and SUPER |
| Editing and export | Play, seek and inspect point clouds | Multi-video sync, timed overlays, motion-ghost stills and 1080p/60 fps export |

[Native migration checklist](docs/NATIVE_PARITY.md)

## Repository layout

| Path | Contents |
| --- | --- |
| [`native/`](native/) | C++ / Qt Quick desktop app, ROS1 bag reader and native tests |
| [`src/slam_fpv/`](src/slam_fpv/) | Python video composition workspace and CLI |
| [`tests/`](tests/) | Python workspace tests |
| [`docs/`](docs/) | User guides, screenshots and [validation notes](docs/VALIDATION.md) |
| [`examples/`](examples/) | ROS1 bridge, extrinsics template and bag validation |

[MIT License](LICENSE)
