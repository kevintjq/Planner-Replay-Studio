# Validation record

## Point-cloud and playback performance

Measured on the same Linux runner with a synthetic 850,000-point XYZ/RGB frame. Timings are best-of-four five-call averages, excluding ROS bag parsing, OpenGL drawing, and file I/O.

| Operation | v0.4.2 | C++ data path |
| --- | ---: | ---: |
| PointCloud2 unpacking and invalid point filtering | 43.18 ms | 7.00 ms |
| Distance coloring | 66.61 ms | 8.96 ms |

The synthetic inputs produced matching XYZ/RGB results. Tests also cover organized rows with padding, mixed field types, big-endian color and nonfinite coordinates. Desktop checks exercised ROS1 playback with two videos, background composition, timeline seeking, portable projects, and 60 fps export.

## Native preview

The Windows x64 workflow compiled the native app, ran its bag-reader and startup tests, and launched the packaged executable in smoke-test mode. [Windows build and test run](https://github.com/kevintjq/uav-studio/actions/runs/45853048677)

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

## Core native GUI workflow — 2026-11-28

This section records completed manual GUI actions and inspection of their existing outputs. It does not
rerun the earlier model, protocol, recording or rendering tests, and does not certify the final release package.
The Windows C++/Qt GUI executable SHA256 was
`A1FC691118FF34BBD386DB169237D8347A114D519AE6F23151967C5822D6B928`.
The two native-host sessions below have separate provenance; results are not silently transferred between hosts.

### Native camera editing, project recovery and real Topic inspection

| Actual GUI action | Observed result | Local evidence |
| --- | --- | --- |
| Edit the native camera with the 8C87 host | Scale changed from 19.25 to 20; playhead remained exactly `8.135480780480801` seconds; Save Project completed | `captures/manual-native-current-a1fc/` |
| Reopen the saved project with the D5AB host | The two additional views and Scale 20 recovered; the GUI showed 8.14 seconds and Global Status OK | `captures/manual-native-final-d5ab/restored-scale-20-d5ab.jpg` |
| Synchronize and inspect a real message | GUI showed Synch 8/8 Topics; the `/Odometry` JSON message was read successfully, with Header `1768571820.657141209` seconds and storage time `1787066868409752823` nanoseconds | `captures/manual-native-final-d5ab/topic-message-inspection-d5ab.jpg` |

The first session used host SHA256
`8C87BB91B70B715D16511B5FDF91CBF250313E6F4DE506D9995DB426F5BC1A29`.
The visible reopened session used host SHA256
`D5AB2C57E7ABF05056930470571EDCFA4CFCDA51CDEE28A6E79848355C9B4BBC`;
its `visible-provenance.json` distinguishes the visible process from the earlier hidden launch.
The saved-project and host hashes are retained in each capture directory. Header and storage time remain
separate clocks; the 8.14-second label is a rounded GUI display, not an independent nanosecond measurement.

These observations close the specified camera-edit/save/reopen and real-message-inspection GUI paths on
this local recording. Recovery of two view slots does not certify independent per-view Display visibility,
every camera pose, arbitrary plugins or long-duration playback performance. Earlier camera/model evidence
remains in [the camera synchronization record](NATIVE_CAMERA_PROPERTY_SYNC_CN.md) and
[the RViz compatibility matrix](RVIZ_COMPATIBILITY_MATRIX_CN.md), under its original host version.

### Real drone trail project recovery and GUI 4K frame export

The A1FC GUI opened the existing `research-three-visible-0-3-4.uavstudio` project. It restored the saved
0–5-second interval, six sampled frames, three filtered samples and three visible instances at 0, 3 and
4 seconds, with the manual Keep/Erase corrections retained. The preview recovery is recorded in
`captures/manual-trajectory-core-a1fc/restored-three-uav-preview.jpg`.

Save Frame completed through the GUI and produced
`captures/manual-trajectory-core-a1fc/gui-three-uav-4k.png`: an actual **3840×2160 RGBA PNG**.
The task owner inspected a native-pixel detail crop of the actual output, saw all three drone instances
and did not observe selection borders, handles or other editor aids in the inspected output.
The success state is preserved in `gui-export-success-three-instances.jpg`, the unscaled detail in
`gui-three-uav-native-pixel-crop.png`, and the GUI/project hashes in the directory's `provenance.json`.
This was an exported canvas image, not a resized screenshot of the application window.

This is a usable result for this specific real clip **with manual mask correction**. The 4-second instance
retains its real partial tree occlusion; hidden aircraft pixels were not invented. The earlier C++
source/automatic/manual comparison found zero changes to visible source RGB pixels; that result comes
from the prior C++ comparison and was not inferred from this GUI screenshot or rerun here. The GUI
save/reopen/export observations supplement [the foreground acceptance record](FOREGROUND_ACCEPTANCE_PROGRESS_CN.md);
they do not claim general automatic semantic segmentation, complete occlusion recovery, new source detail
at 4K, or unchanged RGB after arbitrary output resampling.

### Video Editor real-media GUI 4K export

The same A1FC GUI reopened the previously mouse-edited and saved free-canvas project. In Video Editor,
the operator sought to 8.22 seconds, observed actual video and a synchronized position-difference speed
estimate, then used the inline Export settings and native Save dialog to export 8–10 seconds at 2160p,
60 fps, CPU H.264. The GUI reported successful completion and restored the playhead to 8.22 seconds.
FFprobe read the actual output as **3840×2160, 60/1 fps, 120 frames, 2.000 seconds**. Inspection of a
decoded output frame confirmed the saved video/gauge layout and no selection handles or editor chrome.
The output was rendered through the application exporter, not captured from the window.

Evidence remains local in `captures/manual-video-core-a1fc/`: `gui-video-4k.mp4`,
`gui-export-success-4k.jpg` and `export-first-frame-inspect.png`. This recording has zero-valued Odometry
twist; the gauge uses the explicitly selected position-difference estimate, not invented nonzero twist.
The source has no selected audio track. Earlier mouse editing, layer deletion/undo and project recovery
recordings retain their original scope and were not repeated. The application exited normally afterward.

### Final Windows 1.0.0 payload installation and runtime check

The final OptionalNativeRviz payload passed **57/57** checks in
`captures/rtv1final-native-window/runtime-check.json`. The test compiled the production NSIS source with
an isolated product identity, installed the complete payload, compared all **1,481 files** against its
SHA-256 manifest, and launched the installed executables with only the application and Windows system
directories on PATH and external Qt/ROS/Python environment variables cleared. Real Windows windows
were used; this was not an offscreen run.

CLI help, QML layout, transparent PNG, H.264/AAC export, real-video decode/edit and the installed CLI's
OpenGL 3D PNG export all exited successfully. FFprobe decoded the persistent 1920×1080 PNG. Uninstall
removed the isolated program, shortcuts and registration. The report sets
`runnable_distribution_verified=true` and `opengl_output_verified=true`. The first attempt launched
graphical checks hidden and timed out before an asynchronous grab; a Normal-window control identified
the harness issue. The final run changed only that launch strategy, not product code or image assertions.

| Final artifact | SHA-256 |
| --- | --- |
| `UAVStudio-1.0.0-Windows-x64-OptionalNativeRviz-Setup.exe` (88,808,549 bytes) | `A7F2A2CBA7AC4A2B7238B8E226DE873AE898AFA3D465C79D99EEC06430E59B6A` |
| Installed GUI | `567A1744B04BDF5F2207C58790EE2922746C103FFD16F301F94E09468E07906C` |
| Installed CLI | `EF6D447EFFC01F88C12A5383E8BA8612BFEC11CC591CB1104535B3C8B5918135` |

The production installer above is built from the same frozen payload and installer source. The runtime
test used isolated registration and shortcuts rather than changing the user's production installation.
Its bundled documentation predates this post-build acceptance entry. Safe same-path upgrade has a
separate **43/43** transaction/preservation fixture result in `captures/uga1fc3/report.json` and a
**36/36 complete-payload upgrade** result in `captures/uf2e8e/full-payload-upgrade.json`
(SHA-256 `667009DBFFA4E410E012D35AB43BFA2AEA8ED8D763199BE4CE2E76892FBF6389`).
The latter installed the existing complete 0.1.1 development payload (A1FC, 1,469 files) and upgraded it
at the same path to the final 1.0.0 payload (567A, 1,481 files), using the same isolated product identity.
Both installed payloads matched every file hash. The entire old application and uninstaller remained
byte-identical in the backup; project, settings and edit-history sentinel files survived both upgrade
and uninstall. The upgraded GUI produced a decoded 980×640 layout image in a Normal window with the
minimal environment. Production registration, associations and real application settings were unchanged.
This verifies one real historical-binary upgrade path, not every old version or manual GUI interaction;
the 43 fixture checks and the full-payload upgrade retain their distinct scopes. The upgrade's layout
check is not an additional OpenGL certification; the 57-check installed-runtime run provides that evidence.
The optional Linux host's ELF/source closure is included; this installation check does not launch or
certify the external ROS environment. The earlier real-host GUI evidence retains its own provenance.

These results complete the specified local final-payload gate, not universal plugin, platform or
long-duration certification. Existing model reports, recordings and GUI results retain their original
scope and version. User bags, videos, experiment geometry and these captures remain local in ignored
`captures/` paths. Publication and public-download verification are performed separately after committing.
