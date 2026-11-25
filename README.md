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

1. Open a ROS1 `.bag`. Select a PointCloud2 topic and Odometry or recorded TF. Set the world and body frames to match the bag. The desktop labels are in Chinese.
2. Set the export start and duration, then generate a preview. Adjust the camera, point density, point-cloud color and trajectory color in the settings tabs.
3. Optionally open a flight video and use the editor to set in/out times and drag a crop rectangle. Find a matching event in both views, then mark an anchor. Mark a later matching event to correct clock drift, or enter a manual offset relative to the first cloud. Generate a preview to move the video layer.
4. Export MP4 or PNG. The default is 1080p at 60 fps with EDL and 2× supersampling. Automatic encoding tests NVIDIA NVENC and falls back to CPU H.264 if hardware initialization fails. The flight overlay is exported without audio.

## CLI

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

For TF, use `--pose-source tf` and record `/tf` and `/tf_static`. For live ROS1 or ROS-based replay, use [the bridge](examples/ros1_bridge.py) and `slam-fpv listen --help`.

MIT license. See [LICENSE](LICENSE).
