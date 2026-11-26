# Validation record

Executed on **2026-11-25**. This records observed results, not support guarantees for untested platforms.

## Environment

Windows, CPython 3.13, NumPy 2.4.6, VTK 9.7.2, rosbags 0.11.4, imageio 2.37.3, imageio-ffmpeg 0.8.1, pytest 9.2.0. See `requirements-tested.txt`.

## Automated tests

```bash
python -m pytest -q
```

**15 passed in 9.75 s**. Includes real VTK EDL rendering, foreground RGB pixels, nonempty main/inset views, MP4 decode and motion, generated ROS1 `.bag` serialization/readback, rejected ambiguous/cyclic TF graphs, and local TCP bridge export. Generated fixtures and protocol messages are synthetic.

The first restricted execution could not connect to the loopback receiver (Windows socket permission). The same suite passed with local network access enabled. Graphics tests also exposed a viewport-clearing issue; the final renderer uses two independent EDL framebuffers and composites both frames at the same timestamp. RGB axes use foreground projected lines so the X axis remains visible when its projection overlaps Z.

## Real ROS1 bag checks

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

Reproduce on a bag with the same topics/frames and point order:

```bash
python examples/validate_real_bag.py /absolute/path/run.bag
planner-replay bag /absolute/path/run.bag --start 1 --seconds 4 --output captures/real-odom.mp4
planner-replay bag /absolute/path/run.bag --cloud-topic /cloud_registered_body --pose-source tf \
  --start 1 --seconds 4 --width 960 --height 540 --output captures/real-tf.mp4
```

Agreement checks validate interpretation of the two recorded paths, **not SLAM accuracy, extrinsic accuracy or ground-truth trajectory**. They are not an independent sensor accuracy experiment.

## Synthetic media

[`studio-preview.png`](studio-preview.png) is a screenshot of the desktop’s synthetic point-cloud preview. It illustrates the editing controls, camera layers and simulated speed gauge.

## Unverified

- Original ZIP/demo attachments were not retrievable; no source-equivalence or regression check against them.
- Actual ROS master, rospy subscriptions, rosbag playback and physical live streams. Only the bridge transport/receiver has an end-to-end synthetic loopback test.
- Linux/macOS, EGL/OSMesa/Xvfb deployment, different GPU backends and Python versions.
- Extremely large bags, ROS2, raw Livox CustomMsg, independent ground truth, loop-closure map correction, camera collision avoidance and per-point deskew.

README presentation follows the user's existing calibration repository: centred title and badges, English/Chinese navigation, ordered commands, parameter/issue tables, explicit frames and validation limits.
