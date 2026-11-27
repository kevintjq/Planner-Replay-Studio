# Validation record

## Point-cloud and playback performance

Measured on the same Linux runner with a synthetic 850,000-point XYZ/RGB frame. Timings are best-of-four five-call averages, excluding ROS bag parsing, OpenGL drawing, and file I/O.

| Operation | v0.4.2 | C++ data path |
| --- | ---: | ---: |
| PointCloud2 unpacking and invalid point filtering | 43.18 ms | 7.00 ms |
| Distance coloring | 66.61 ms | 8.96 ms |

The synthetic inputs produced matching XYZ/RGB results. Tests also cover organized rows with padding, mixed field types, big-endian color and nonfinite coordinates. Desktop checks exercised ROS1 playback with two videos, background composition, timeline seeking, portable projects, and 60 fps export.

## Native preview

The Windows x64 workflow compiled the native app, ran its bag-reader and startup tests, and launched the packaged executable in smoke-test mode. [Windows build and test run](https://github.com/kevintjq/Planner-Replay-Studio/actions/runs/45853048677)

The Linux desktop was used to check playback, seeking, rotating and zooming a sample ROS1 bag. The bag-reader check covered uncompressed, BZ2 and LZ4 versions of the same fixture. [Native preview screenshot](native-preview.png)

## Video composition workspace

The following checks date from **2026-11-25**.

### Environment

Windows, CPython 3.13, NumPy 2.4.6, VTK 9.7.2, rosbags 0.11.4, imageio 2.37.3, imageio-ffmpeg 0.8.1, pytest 9.2.0.

### Automated tests

```bash
python -m pytest -q
```

**15 passed in 9.75 s**. Includes real VTK EDL rendering, foreground RGB pixels, nonempty main/inset views, MP4 decode and motion, generated ROS1 `.bag` serialization/readback, rejected ambiguous/cyclic TF graphs, and local TCP bridge export. Generated fixtures and protocol messages are synthetic.

The first restricted execution could not connect to the loopback receiver (Windows socket permission). The same suite passed with local network access enabled. Graphics tests also exposed a viewport-clearing issue; the final renderer uses two independent EDL framebuffers and composites both frames at the same timestamp. RGB axes use foreground projected lines so the X axis remains visible when its projection overlaps Z.

### Real ROS1 bag checks

One existing local bag was read without modifying it. It contains 394 world clouds, 394 body clouds, 393 `/Odometry` samples and 393 TF messages, plus other unselected topics. Raw bags and rendered real-world geometry are not distributed in this repository.

| Check | Observed result |
| --- | --- |
| World-frame PointCloud2 + Odometry | 4-second header-time span, 97 decoded H.264 frames, 1280×720, 24 FPS, JSON `complete: true` |
| Body-frame PointCloud2 + recorded TF | Same span, 97 decoded H.264 frames, 960×540, 24 FPS, JSON `complete: true` |
| Matching point clouds after body → world transform | 393 corresponding cloud pairs; maximum absolute XYZ component difference approximately `3.59e-6 m` |
| Odometry versus recorded TF | Maximum matrix entry difference `8.88e-16` over 393 matched stamps |
| Initial unsupported body cloud | One cloud skipped because its stamp precedes the available dynamic TF; skip recorded explicitly |
| Timestamp convention | Stored bag times and sensor headers differ by months; header-based processing succeeds |
| Visual review | Main rear view and oblique inset, RGB axes, accumulated points, no drone model |

Render the two views on a bag with matching topics and frames:

```bash
planner-replay bag /absolute/path/run.bag --start 1 --seconds 4 --output captures/real-odom.mp4
planner-replay bag /absolute/path/run.bag --cloud-topic /cloud_registered_body --pose-source tf \
  --start 1 --seconds 4 --width 960 --height 540 --output captures/real-tf.mp4
```

Agreement checks validate interpretation of the two recorded paths, **not SLAM accuracy, extrinsic accuracy or ground-truth trajectory**. They are not an independent sensor accuracy experiment.

### Synthetic media

[`studio-preview.png`](studio-preview.png) is a screenshot of the desktop’s synthetic point-cloud preview. It illustrates the editing controls, camera layers and simulated speed gauge.

### Outside the scope of these checks

- Actual ROS master, rospy subscriptions, rosbag playback and physical live streams. Only the bridge transport/receiver has an end-to-end synthetic loopback test.
- The composition workspace on Linux/macOS in this Windows test run, plus other GPU backends and Python versions.
- Extremely large bags, ROS2, raw Livox CustomMsg, independent ground truth, loop-closure map correction, camera collision avoidance and per-point deskew.
