<h1 align="center">Planner Replay Studio</h1>

<p align="center">ROS1 point-cloud replay · EGO / SUPER overlays · Multi-camera flight editing</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-0078D4?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/Input-ROS1%20bag-22314E?style=flat-square" alt="ROS1 bag">
  <img src="https://img.shields.io/badge/Export-60%20fps-00599C?style=flat-square" alt="60 fps">
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="MIT License">
</p>

<p align="center">English · <a href="README_CN.md">简体中文</a></p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#inputs-and-overlays">Inputs and overlays</a> ·
  <a href="#multi-video-alignment">Multi-video alignment</a> ·
  <a href="#save-and-export">Save and export</a>
</p>

---

![Planner Replay Studio with simulated data](docs/studio-preview.png)

## Quick start

1. [Download the Windows x64 installer](https://github.com/kevintjq/Planner-Replay-Studio/releases/download/studio-v0.4.0/PlannerReplayStudio-Setup.exe), run it, and open **Planner Replay Studio Full** from the Start menu. The default folder is `%LOCALAPPDATA%\Programs\Planner Replay Studio Full`. You can change it during setup and opt in to a desktop shortcut.
2. In **Data**, open a ROS1 `.bag` or choose **Use simulated data**. Confirm the cloud and pose topics, select views, and generate a preview.
3. Add footage, images, or text under **Layers**. Drag to place and resize layers in the preview. Set visibility intervals and transitions on the timeline.

## Inputs and overlays

| Data | Support |
| --- | --- |
| Point clouds and pose | `PointCloud2`, `Odometry`, and TF. Accumulate scans, replace the map, or color points by distance to the drone |
| Planner output | EGO / SUPER planned paths, goals, and safety corridors. Each layer can be toggled independently |
| Sensors | Camera FOV, latest LiDAR scan highlighting, and topic-based speed in m/s. Choose its topic in the speed gauge settings |

Under **Data → Topic displays**, import an `.rviz` file or find and add any topic. Adjust color, visibility, and view placement per display, or double-click a row to inspect its fields. [Import and display support](docs/studio.md#topic-displays-and-rviz-import)

## Multi-video alignment

1. Add videos under **Layers** and open **Trim / Align** for each clip.
2. Find the same action in the replay and footage, then choose **Align these frames**.
3. If the gap grows over time, add a second anchor in **Fine alignment** and check the middle of the clip.

## Save and export

| Output | How to create it |
| --- | --- |
| Project folder | Choose **Export Project Folder** to copy the project and its media together. Move the entire folder and open `project.slamfpv` to resume editing |
| MP4 or transparent PNG | Choose **Export 1080p · 60 fps** for video, or **More settings** for custom export. Enable **Transparent canvas** for transparent PNG |
| Flight-motion still | Open **Fixed-camera trajectory image**, select a starting frame, box the drone, and set the sampling interval before saving a PNG |

[Workspace guide](docs/studio.md) · [MIT License](LICENSE)
