# SLAM FPV Renderer

A Windows desktop workspace for turning ROS1 SLAM point clouds and poses into multi-view flight videos. Combine point-cloud cameras with real flight footage, captions, a speed gauge and optional planner or sensor overlays.

[简体中文](README_CN.md) · [Validation](docs/VALIDATION.md)

![SLAM FPV workspace with the spatial overlays panel, a flight corridor and a stabilized FPV inset](docs/studio-preview.png)

*The workspace screenshot uses simulated data.*

## Start on Windows

Install 64-bit Python 3.10 or newer with Tcl/Tk. Clone the repository, then double-click [Start_SLAM_FPV.cmd](Start_SLAM_FPV.cmd). The first launch downloads dependencies. Open **Use simulated data** to try the workspace before loading a bag.

## Make a composition

1. Open a ROS1 `.bag` under **Data**. Select a PointCloud2 topic, choose Odometry or TF, and set the export interval.
2. Choose the main camera and add secondary views in **Layers → + View**. Pick from 12 presets, including stabilized FPV, raw FPV, oblique, side, chase and orbit. Both FPV modes hide the actual and planned flight paths and show the original body attitude on the corner triad.
3. Add flight footage as a backdrop or an inset. Align clips using matching events, then add timed captions or a speed gauge. Drag layers, resize them by the corner, and choose a transparent or dark background.
4. Export MP4 or PNG. The default video settings are 1080p at 60 fps, with automatic NVIDIA NVENC or CPU H.264 encoding.

## Spatial overlays

Enable any layer under **Spatial overlays**, then refresh the preview. The same settings apply to PNG and MP4.

| Layer | Input |
| --- | --- |
| Planned trajectory | `nav_msgs/Path` or `visualization_msgs/Marker[Array]` |
| Safety corridor | `visualization_msgs/Marker[Array]` with line strips, line lists, triangle lists or cubes |
| Camera FOV | Optical TF frame (+Z forward), horizontal and vertical angles (°), depth (m) |
| LiDAR FOV | LiDAR TF frame (+X forward, +Z up), horizontal and vertical angles (°), radius (m), center elevation (°). Use 360° for a full sweep. |
| Current scan | Latest `sensor_msgs/PointCloud2` frame, highlighted separately from the accumulated map |

Record the planner visualization topics together with the bag. Use the TF tree or the extrinsics JSON to connect sensor frames to the world frame. FOV outlines describe configurable viewing envelopes. Current scan returns come from the recorded point-cloud topic. Leaving that topic empty highlights the latest frame from the main point-cloud input, so select a dedicated scan topic if the main input is a full map. Convert Livox CustomMsg to PointCloud2 before importing.

## CLI

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output captures/run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

For TF, use `--pose-source tf` and include `/tf` and `/tf_static` as needed. See the [ROS1 bridge](examples/ros1_bridge.py) for live or replay use. Keep recorded bags, imported footage and video exports local.

[MIT License](LICENSE)
