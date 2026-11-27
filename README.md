# Planner Replay Studio

Replay ROS1 point clouds, inspect UAV planner output, and edit flight footage on Windows.

English · [简体中文](README_CN.md)

[Download the Windows x64 installer](https://github.com/kevintjq/Planner-Replay-Studio/releases/download/studio-v0.3.1/PlannerReplayStudio-Setup.exe) and open **Planner Replay Studio Full** from the Start menu. The installer includes the runtime.

![Planner Replay Studio with simulated point clouds and planner overlays](docs/studio-preview.png)

## Quick start

1. In **Data**, open a ROS1 `.bag` or choose **Use simulated data**. Select the topics and views you need. Planner paths, goals, flight corridors, current scans, and speed are optional.
2. Add flight videos, images, or text. Align the videos to the replay, trim them on the timeline, and adjust the layout in the preview.
3. Select **Export 1080p · 60 fps** to render the video, or use **More settings** for custom export options. Use **Fixed-camera trajectory image** to save a separate flight-motion still.
4. Choose **Export Project Folder** to keep the project and its media together. To continue on another computer, copy the whole folder and open `project.slamfpv` from the app.

[Workspace guide](docs/studio.md) · [MIT License](LICENSE)
