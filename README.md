<h1 align="center">SLAM FPV Renderer</h1>

<p align="center">SLAM point clouds · Multi-video alignment · 60 fps export</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows-0078D4?style=flat-square" alt="Windows">
  <img src="https://img.shields.io/badge/Input-ROS1%20bag-22314E?style=flat-square" alt="ROS1 bag">
  <img src="https://img.shields.io/badge/Python-3.10%2B-3776AB?style=flat-square" alt="Python 3.10+">
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="MIT License">
</p>

<p align="center">English · <a href="README_CN.md">简体中文</a></p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#multi-video-alignment">Multi-video alignment</a> ·
  <a href="#inputs-and-overlays">Inputs and overlays</a> ·
  <a href="#timeline">Timeline</a> ·
  <a href="#save-and-export">Save and export</a>
</p>

---

A Windows workspace for composing multi-view videos from ROS1 SLAM point clouds, poses and flight footage.

![SLAM FPV workspace using simulated data](docs/studio-preview.png)

| Feature | Description |
| --- | --- |
| Point-cloud views | 12 presets, including stabilized FPV, chase and oblique views |
| Multiple videos | Trim, align and place each clip as a backdrop or picture-in-picture |
| Optional overlays | Planned paths, goals, safety corridors, camera FOV, latest LiDAR scan, speed gauge and timed captions |

## Quick start

1. Install 64-bit Python 3.10 or newer with Tcl/Tk.
2. Clone the repository and double-click [Start_SLAM_FPV.cmd](Start_SLAM_FPV.cmd). The first launch downloads dependencies.
3. Open a ROS1 `.bag` under **Data** or choose **Use simulated data**. Pick views and layers, then generate a preview.

## Multi-video alignment

1. Use **Layers → + Video** to select multiple clips. Select a clip and open **Trim / Align**.
2. Find a recognizable event on the SLAM timeline, step through the footage to match it, then record an anchor and apply.
3. For noticeable clock drift, add a second matching event near the end. Use **Check midpoint** to verify the result.

All clips use the same bag timeline and keep their own trim ranges and anchors. One anchor sets the time offset. Two also correct linear clock drift.

## Inputs and overlays

| Data | ROS topic type |
| --- | --- |
| Map and pose | `PointCloud2` + `Odometry` or TF |
| Planned path | `Path`, `Marker[Array]`, SUPER colored `ARROW` segments or EGO `Bspline` |
| Goal | SUPER `visualization/goal`, EGO `goal_point`, `PoseStamped` or `PointStamped` |
| Safety corridor | `Marker` or `MarkerArray` with line strips, line lists, triangles or cubes |
| Camera FOV | `CameraInfo` and optical-frame TF |
| Latest LiDAR scan | `PointCloud2`, Livox `CustomMsg` or `LaserScan` |

Topics and frames are detected automatically and can be changed in the dropdowns. Enable data under **Spatial overlays**, then choose what each view shows. Adjust actual and planned path widths under **Quality**. The preview reports scan source, age and missing data.

SUPER defaults to `visualization/exp_traj` and `visualization/exp_sfc` when present. EGO prefers `planning/bspline` and can also use `optimal_list`. EGO B-splines default to `world`, which can be changed below the trajectory topic. Goals and trajectories switch independently.

## Timeline

- Select a planning event to jump to a replan or goal update. Enable the previous-plan overlay to compare with a dashed grey path.
- Set a layout marker at the playhead to switch between the composition, full-screen point cloud, and full-screen footage. Click a purple marker to jump back to it.

## Save and export

| Format | Description |
| --- | --- |
| `.slamfpv` | Stores media paths, trims, anchors, layout markers and export settings. Moved media can be relinked. |
| MP4 | Defaults to 1080p at 60 fps with automatic NVIDIA NVENC or CPU H.264 encoding; optional single-track source audio and 0.5× / 0.25× playback |
| PNG | Enable **Transparent canvas** to export an alpha channel |

Export checks sample pose, TF, planner layers and clip coverage. Source audio follows the selected clip’s alignment and trim.

<details>
<summary>Command line</summary>

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output captures/run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

[ROS1 live bridge](examples/ros1_bridge.py) · [Validation](docs/VALIDATION.md)

</details>

## License

[MIT License](LICENSE)
