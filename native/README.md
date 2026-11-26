# Planner Replay 原生预览

下载 [PlannerReplayNative-Setup.exe](https://github.com/kevintjq/Planner-Replay-Studio/releases/download/native-v0.1.1/PlannerReplayNative-Setup.exe) 并双击安装。
安装完成后可立即启动，以后也可从开始菜单打开 **Planner Replay Studio**。
安装包包含运行依赖，支持 Windows x64 和 OpenGL 3.3 显卡驱动。

## 操作

- 打开 ROS1 bag，选择 `PointCloud2` 和 `Odometry` 话题。支持配准点云，以及里程计机体坐标系中的点云。后者需要同步位姿。
- 点击播放或拖动时间轴。左键旋转视口，右键平移，滚轮缩放。距离颜色以当前机体位置为基准。
- EGO 和 SUPER 的常见占据地图话题默认整图替换。速度读自里程计的三维线速度。
- bag 支持无压缩、BZ2 和 LZ4。预览版每块最多 256 MiB，每个所选点云话题最多载入 1600 万有效点，不做自动降采样。

本版覆盖原生点云回放。独立速度话题、通用 TF 树、规划及走廊图层、实拍合成和 60 fps 导出可在 [视频合成工作台](https://github.com/kevintjq/Planner-Replay-Studio/blob/main/docs/studio.zh-CN.md) 使用。

## 构建

需要 CMake 3.22、支持 C++20 的编译器、Qt 6.5 或更高版本，以及 BZip2 和 LZ4。在 Windows 上可通过 vcpkg 的 `x64-windows-static-md` 安装压缩库。生成安装包还需要 NSIS。构建、安装测试和发布命令见 [Windows 构建流程](https://github.com/kevintjq/Planner-Replay-Studio/blob/main/.github/workflows/native-windows.yml)。
