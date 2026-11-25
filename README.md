<h1 align="center">SLAM-FPV-Renderer</h1>

<p align="center">Rear chase · Oblique picture-in-picture · Timestamped ROS1 data · VTK EDL</p>

<p align="center">
  <img src="https://img.shields.io/badge/Python-3.10%2B-3776AB?style=flat-square" alt="Python 3.10+">
  <img src="https://img.shields.io/badge/ROS1-bag%20%2F%20TF%20%2F%20Odometry-22314E?style=flat-square" alt="ROS1">
  <img src="https://img.shields.io/badge/VTK-EDL-5B8DC7?style=flat-square" alt="VTK EDL">
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="MIT License">
</p>

<p align="center">English · <a href="README_CN.md">简体中文完整操作指南</a></p>

<p align="center">
  <a href="#quick-start">Quick start</a> · <a href="#record-and-replay">Record & replay</a> ·
  <a href="#frames-and-extrinsics">Frames</a> · <a href="#parameters">Parameters</a> ·
  <a href="#validation">Validation</a> · <a href="#troubleshooting">Troubleshooting</a>
</p>

---

Render SLAM point clouds as a synchronized **rear chase main view** and a **high oblique inset**. The camera follows body yaw, remains level, and draws the full body attitude as small foreground RGB axes. A golden line shows the rendered history. There is no drone mesh and no flight-control output.

![Synthetic dual-view preview](docs/synthetic-preview.png)

**Synthetic illustration, not a measured flight or an obstacle-avoidance result.** [Watch the synthetic MP4](docs/synthetic-demo.mp4). Real ROS1 bag ingestion has separately been tested; see [validation evidence](docs/VALIDATION.md).

| Capability | Behaviour |
| --- | --- |
| Direct ROS1 bag | `rosbags`, no ROS installation or roscore needed |
| Pose | Select recorded Odometry or TF; dynamic TF chains and `/tf_static` |
| Synchronization | Header timestamps; linear translation + shortest-path quaternion SLERP; no extrapolation |
| Views | Exactly rearward by default; inset approximately 34° downward, never vertical with supported parameters |
| Rendering | VTK EDL, bounded voxel map, RGB or height colour, trajectory, foreground RGB TF axes |
| Export | PNG and constant-FPS H.264 MP4, plus JSON run report |
| ROS1 topics/replay | Loopback bridge from a separate ROS Python environment; ROS runtime integration remains unverified |

## Quick start

Use Python **3.10+** with a working OpenGL context. The tested host was Windows with Python 3.13; Ubuntu with Python 3.10+ is an intended target, not a verified platform. ROS1's Python environment is separate from the renderer environment.

```bash
git clone https://github.com/kevintjq/SLAM-FPV-Renderer.git
cd SLAM-FPV-Renderer
python -m venv .venv
```

Activate on Linux/macOS:

```bash
source .venv/bin/activate
```

Activate on Windows PowerShell:

```powershell
.\.venv\Scripts\Activate.ps1
```

Install and render:

```bash
python -m pip install --upgrade pip
python -m pip install -e ".[test]"
slam-fpv demo --output captures/synthetic.mp4 --seconds 6
slam-fpv demo --output captures/synthetic.png --seconds 3 --start 2
python -m pytest -q
```

The video dependency supplies an FFmpeg binary. VTK's offscreen option still needs a graphics backend; it does not guarantee headless operation on every machine. On Linux with X11 software rendering, try `xvfb-run -a slam-fpv ...` after installing Xvfb and Mesa. That path has not been tested here.

## Direct bag reading

Inspect before rendering:

```bash
slam-fpv inspect /absolute/path/run.bag
slam-fpv bag /absolute/path/run.bag --output captures/run.mp4 \
  --cloud-topic /cloud_registered --odom-topic /Odometry \
  --world camera_init --body body --start 1 --seconds 10
```

For body-frame clouds and recorded TF:

```bash
slam-fpv bag /absolute/path/run.bag --output captures/body-tf.mp4 \
  --cloud-topic /cloud_registered_body --pose-source tf \
  --world camera_init --body body --start 1 --seconds 10
```

These topic and frame names match one tested FAST-LIO-style bag; inspect your own data. `/cloud_registered` is not assumed to be a full map. Use `--cloud-mode accumulate` for incremental scans and `--cloud-mode replace` for a repeatedly published full map. PNG exports the first supported frame at or after `--start`; MP4 follows the timeline from that point. Clouds before `--start` are accumulated as history.

## Record and replay

Run the LiDAR driver and your SLAM estimator separately. This tool consumes their outputs; it does not estimate SLAM from raw LiDAR/IMU.

In a sourced ROS1 terminal:

```bash
rosbag record -O run.bag /cloud_registered /cloud_registered_body /Odometry /tf /tf_static
```

Record at least the selected PointCloud2 topic and selected pose source, plus all transforms needed to reach your world frame. Check `rostopic echo -n 1 /cloud_registered/header` and `/Odometry`; confirm `/tf_static` is actually included. Stop recording with Ctrl+C and let rosbag close cleanly.

For ROS-based replay, use three terminals. On a ROS Noetic host, install the bridge dependencies in the **ROS system environment**:

```bash
sudo apt install ros-noetic-rospy ros-noetic-sensor-msgs ros-noetic-nav-msgs \
  ros-noetic-tf2-msgs ros-noetic-rospy-message-converter
source /opt/ros/noetic/setup.bash
roscore
```

Renderer terminal (Python 3.10+ virtual environment):

```bash
slam-fpv listen --output captures/replay.mp4 --seconds 10 --delay 0.3
```

ROS terminal, outside the renderer virtual environment:

```bash
source /opt/ros/noetic/setup.bash
rosparam set use_sim_time true
python3 examples/ros1_bridge.py --cloud-topic /cloud_registered --odom-topic /Odometry
```

Another ROS terminal:

```bash
source /opt/ros/noetic/setup.bash
rosbag play run.bag --clock --pause
# Press Space after the renderer and bridge are ready.
```

Restart listener and bridge for each replay; do not loop a bag into an existing session. A backwards header clock is rejected. For physical live topics, start the same listener and bridge with your sources running. The receiver binds only `127.0.0.1`; run both on the same host. It exports offscreen rather than opening an interactive preview window. Use direct bag mode for deterministic offline output.

Bridge JSONL transport has been tested over loopback, but `rospy`, roscore, actual ROS replay and physical live publishing have **not** been tested on the Windows validation host. ROS Noetic's system Python may be 3.8; the bridge is a standalone script so it does not need the renderer package installed there.

## Frames and extrinsics

```text
p_world = T_world_cloud(t_cloud) × p_cloud
T_world_body(t) = T_world_odom_parent(t) × T_odom_parent_body(t)
p_parent = R_parent_child × p_child + translation_parent_child
```

Positions are metres; angles are degrees only in the camera CLI. Quaternions use **[x, y, z, w]**. Camera following assumes world +Z up and body +X forward, +Y left, +Z up (FLU). No automatic NED/FRD/optical-frame conversion or calibration is performed. An initialization frame named `camera_init` need not be geographical ENU.

Odometry pose is the child pose in `header.frame_id`; `child_frame_id` must match `--body`. `--pose-source odom` treats that Odometry as authoritative for the body and ignores TF edges ending at that child. `--pose-source tf` ignores Odometry. Other frame-chain edges are read from TF.

Clouds already in `--world` are not transformed again. Sensor-frame clouds require a recorded TF path or a supplied static extrinsic:

```bash
slam-fpv bag run.bag --output captures/run.mp4 --cloud-topic /lidar_points \
  --extrinsics examples/extrinsics.json
```

The example JSON describes **body ← lidar**, with zero translation and identity rotation as placeholders. Replace it with measured calibration; never assume those values are correct for your hardware. Do not duplicate a recorded TF edge.

Both views share one output time and the same map state. Clouds are transformed at their own header stamps and appear only once their stamp is no later than the output time. Pose translation is interpolated linearly and orientation with SLERP. Dynamic brackets wider than `--max-gap` are rejected. Zero stamps, missing paths and extrapolation never become identity transforms. Leading unsupported clouds/poses are skipped and counted; a missing transform after processing starts stops the export. A partial file can remain; check `complete` in its JSON report. Offline processing sorts pose timestamps; decreasing cloud timestamps are rejected.

## Parameters

Run `slam-fpv bag --help`, `slam-fpv demo --help` or `slam-fpv listen --help` for the full executable interface.

| Option | Default | Meaning |
| --- | --- | --- |
| `--cloud-topic` / `--odom-topic` | `/cloud_registered` / `/Odometry` | Selected ROS topics |
| `--world` / `--body` | `camera_init` / `body` | Render frame / tracked FLU frame |
| `--pose-source` | `odom` | `odom` or `tf` |
| `--max-gap` | `0.5` | Largest allowed dynamic interpolation bracket, s |
| `--extrinsics` | none | Static parent ← child JSON file |
| `--start` / `--seconds` | `0` / `6` | Offline start offset and maximum span, s |
| `--output` | required | `.png` or `.mp4`; sibling `.json` is written |
| `--width` / `--height` / `--fps` | `1280` / `720` / `24` | Even output dimensions, constant video rate |
| `--distance` / `--camera-height` / `--side` | `3.8` / `1.35` / `0` | Main camera rear / up / left offsets, m |
| `--fov` | `65` | Vertical camera FOV, degrees |
| `--inset-distance` / `--inset-height` | `10` / `6.7` | Inset rear / up offsets, m; approximately 34° downward |
| `--axis` / `--axis-pixels` | `0.48` / `28` | Nominal axis length, m / minimum projected scale target |
| `--point-size` | `2` | Point raster size, pixels |
| `--color` | `rgb` | Packed RGB when available, otherwise height; `height` forces height |
| `--voxel` / `--max-points` | `0.08` / `200000` | Retained map voxel size, m / point limit |
| `--cloud-mode` | `accumulate` | `accumulate` or `replace` |
| `--no-edl` | off | Disable EDL for comparison or GPU diagnosis |
| `--port` / `--delay` | `8765` / `0.3` | Listener loopback port / pose lookahead buffer, s |

The inset occupies the upper-right 32% × 33%. Axis screen scaling enlarges the whole triad uniformly, so its on-screen length is not a metric ruler. An axis aligned with the viewing ray can still overlap another axis. Increase `--axis`, use a small `--side`, or inspect the inset. No drone model is added. Offline export includes the end sample when it falls on the output grid, so encoded duration can exceed the requested span by one frame. `--start` applies to offline modes; listen runs from its first supported cloud and ends after `--seconds` or disconnection.

## Validation

```bash
python -m pytest -q
```

Tests cover TF composition/inversion, static extrinsics, quaternion interpolation, rejected extrapolation/gaps, padded/endian PointCloud2, packed RGB, bounded maps, camera positions, RGB visibility, EDL frame rendering, MP4 decoding/frame count/motion, generated ROS1 bag parsing and loopback bridge export. The checked environment and real-bag cross-checks are recorded in [docs/VALIDATION.md](docs/VALIDATION.md). The real bag is not distributed.

## Troubleshooting

| Symptom | Check / fix |
| --- | --- |
| Missing cloud topic / Livox CustomMsg | Inspect types; run SLAM or convert to PointCloud2 upstream. CustomMsg is not decoded here. |
| No timestamped TF path | Match frames, include `/tf_static`, check transform direction and header timestamps. Start later for initialization. |
| Transform gap / partial export | Inspect tracking gaps and split intervals. Increasing `--max-gap` does not repair lost tracking. |
| Empty/black frame or shader error | Verify OpenGL, driver and offscreen backend; compare `--no-edl`. Try Xvfb/Mesa on Linux. |
| Tiny/overlapping TF axes | Raise `--axis-pixels`, inspect inset or set a small `--side`. Foreground axes intentionally remain visible through clouds. |
| Blurred/ghosted map | Check frames/timing, then choose `replace` for full-map topics. Accumulation does not correct loop closure. |
| Slow rendering | Increase `--voxel`, reduce `--max-points`, output size or FPS. |
| MP4 fails | Check output permissions, even dimensions, FFmpeg availability and disk space. |
| Replay waiting / dropped bridge messages | Start listener first, then bridge, then playback; slow playback and increase `--delay` for delayed TF. |
| Bridge import error | Run the script with sourced ROS system Python and install `rospy_message_converter` there. |

## Known limits and provenance

- This repository is a new implementation of the recovered conversation requirements. Original attachments `SLAM-FPV-Renderer-v0.1.zip`, `edl_avoidance_demo.py` and `rear_chase_demo.py` were unavailable in this session. The similarly named example wrappers are **rebuilt**, not recovered originals; byte-for-byte compatibility has not been checked.
- Visualization only: no SLAM solver, obstacle avoidance, camera collision detection, per-point motion deskew, automatic extrinsic/time calibration or planning validation.
- Input is ROS1 PointCloud2 plus TF/Odometry; no ROS2, raw Livox CustomMsg, PCD map loading, camera image overlay or GUI editor.
- RGB supports scalar packed `rgb`/`rgba` 32-bit fields; no separate r/g/b fields or intensity colormap. Nonfinite XYZ points are removed. Height colour uses a fixed illustrative range.
- Offline loading indexes all TF/odometry in memory and reads clouds in a second pass. The map is capped and downsampled, so it is not a lossless reconstruction. Old accumulated points do not move after SLAM loop closures.
- TF foreground overlays deliberately ignore cloud occlusion. EDL depends on GPU/backend and is not a physical lighting model. The inset is synchronized, not an independently calibrated camera sensor.
- Bridge queues and history are bounded; overload or missing transforms can drop clouds. Its final buffered tail may remain unrendered on disconnect, and dropped/gap frames may shorten the live export. Deterministic bag mode is preferred for final videos.
- Only the documented host, synthetic fixtures and one local real bag have been validated. Linux/macOS, actual ROS master/replay, physical live streams, very large bags and other sensor/frame conventions remain unverified.

## Project structure

```text
src/slam_fpv/    geometry, PointCloud2, bag source, VTK renderer, CLI, loopback receiver
examples/       ROS1 bridge, extrinsic template, rebuilt demo entry points
tests/          executable unit, bag, graphics, video and loopback tests
docs/           labelled synthetic media and validation record
```

Contributions use Conventional Commits, e.g. `feat(tf): add timestamped transforms` and `test(renderer): verify video export`. Run tests and identify synthetic versus recorded evidence in changes.

## License

New code is released under the [MIT License](LICENSE). Original unavailable attachments and external datasets are not included or relicensed.
