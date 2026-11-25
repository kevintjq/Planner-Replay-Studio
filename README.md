# SLAM FPV Renderer

Turn ROS1 SLAM point clouds into a rear-follow view with a high oblique inset. Export MP4 or PNG and optionally add movable flight footage.

[简体中文](README_CN.md) · [Demo](docs/synthetic-demo.mp4) · [Validation](docs/VALIDATION.md)

![Synthetic dual-view preview](docs/synthetic-preview.png)

## Windows desktop

Install 64-bit Python 3.10+ with Tcl/Tk. In PowerShell:

```powershell
git clone https://github.com/kevintjq/SLAM-FPV-Renderer.git
cd SLAM-FPV-Renderer
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e .
.\.venv\Scripts\python.exe -m slam_fpv.gui
```

1. Open a ROS1 `.bag`, select PointCloud2 and Odometry or TF, then set the export interval.
2. Add multiple flight videos in the Layers tab. Edit the trim, crop, alignment, width and opacity of each video independently. Use matching events as anchors to align time and correct drift.
3. Add timed captions or a speed gauge. Drag annotation handles to change their visible intervals and scrub the timeline to preview. Times are relative to the first cloud. The gauge estimates 3D speed in m/s from world-frame poses.
4. Drag overlays directly in the preview. Export MP4 or PNG at 1080p and 60 fps by default. Automatic encoding tests NVENC and falls back to CPU H.264.

## CLI

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

For TF, use `--pose-source tf` and record `/tf` and `/tf_static`. For live ROS1 or ROS-based replay, use [the bridge](examples/ros1_bridge.py) and `slam-fpv listen --help`.

MIT license. See [LICENSE](LICENSE).
