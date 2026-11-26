<h1 align="center">Planner Replay Studio</h1>

<p align="center">SLAM 点云渲染 · 多路实飞对齐 · 60 fps 合成导出</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows-0078D4?style=flat-square" alt="Windows">
  <img src="https://img.shields.io/badge/Input-ROS1%20bag-22314E?style=flat-square" alt="ROS1 bag">
  <img src="https://img.shields.io/badge/Python-3.10%2B-3776AB?style=flat-square" alt="Python 3.10+">
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="MIT License">
</p>

<p align="center"><a href="README.md">English</a> · 简体中文</p>

<p align="center">
  <a href="#快速开始">快速开始</a> ·
  <a href="#多路视频对齐">多路视频对齐</a> ·
  <a href="#输入与叠加">输入与叠加</a> ·
  <a href="#时间轴">时间轴</a> ·
  <a href="#保存与导出">保存与导出</a>
</p>

---

在 Windows 上把 ROS1 SLAM 点云、位姿和实飞视频合成多视角画面。

![Planner Replay 工作台模拟效果](docs/studio-preview.png)

| 功能 | 说明 |
| --- | --- |
| 点云视角 | 12 种机位，含稳定 FPV、后方跟随和高位斜视 |
| 多路视频 | 每路独立裁剪、对齐和布局，可作为底片或画中画 |
| 可选叠加 | 规划轨迹、目标点、安全走廊、相机 FOV、雷达当前帧、速度表和定时文字 |

## 快速开始

1. 安装带 Tcl/Tk 的 64 位 Python 3.10 或更高版本。
2. 克隆仓库，双击 [Start_Planner_Replay.cmd](Start_Planner_Replay.cmd)。首次启动会下载依赖。
3. 在「数据」打开 ROS1 `.bag`，或点「使用模拟数据」查看效果。选择机位和图层后点「生成预览」。

## 多路视频对齐

1. 在「图层 → + 视频」选择多个视频。逐路选中视频，点「裁剪 / 对齐」。
2. 在共同时间轴上定位 SLAM 里的同一动作，再逐帧找到该动作对应的实拍画面，记录锚点并点「应用」。
3. 有明显时钟漂移时，在片尾附近再选一个两边都看得见的动作作为第二锚点，用「检查中段」核对同步。

所有视频以同一份 bag 为时间基准，各自保留裁剪范围和锚点。一个锚点校正时间偏移，两个锚点还可校正线性时钟漂移。

## 输入与叠加

| 数据 | 话题或格式 |
| --- | --- |
| 地图与位姿 | `PointCloud2` + `Odometry` 或 TF |
| 规划轨迹 | `Path`、`Marker[Array]`、SUPER 彩色 `ARROW` 或 EGO `Bspline` |
| 目标点 | SUPER `visualization/goal`、EGO `goal_point`、`PoseStamped` 或 `PointStamped` |
| 安全走廊 | `Marker` 或 `MarkerArray`，支持折线、线段、三角面和立方体 |
| 相机 FOV | `CameraInfo` 与光学坐标系 TF |
| 雷达当前帧 | `PointCloud2`、Livox `CustomMsg` 或 `LaserScan` |

话题和坐标系自动识别，可从下拉列表更换。先在「空间叠加」启用数据，再到各视角的设置里选要显示的叠加项。在「线条与标记」可分别调整已飞轨迹、规划轨迹和 TF 的线宽，以及实心目标点的直径。预览会提示扫描来源、帧龄和缺失的数据。

SUPER 优先识别 `visualization/exp_traj` 和 `visualization/exp_sfc`。EGO 优先识别 `planning/bspline`，也可选择 `optimal_list`。EGO B 样条使用默认 `world` 坐标系，可在轨迹话题下方更换。规划轨迹和目标点可分别关闭。

预览时，将鼠标移到点云主画面或小窗上滚动即可拉近、拉远。各视角的设置里也可直接输入相机缩放比例，随项目保存。

## 时间轴

- 点击「+ 事件」在播放头处打标记，右键改名或删除。点击标记即可跳转，也可从下拉列表跳到重规划或目标更新。
- 视频、文字和速度表各占一轨。选中片段后拖动两端裁剪；视频继续使用原有对齐锚点。勾选「对比上一条规划」可看灰色虚线。
- 把播放头放到切换时刻，从「添加布局标记」选择当前合成、点云全屏或一路实拍全屏。紫色标记可跳转。

## 保存与导出

| 项目 | 说明 |
| --- | --- |
| `.slamfpv` | 保存素材路径、裁剪、锚点、事件与布局标记和导出参数，素材移动后可重新定位 |
| MP4 | 默认 1080p、60 fps，自动选择 NVIDIA NVENC 或 CPU H.264；可选一路原声及 0.5× / 0.25× 慢放 |
| PNG | 勾选「透明画布」保留透明通道 |

导出前抽查位姿、TF、规划图层和各路视频的覆盖范围。原声沿用该路视频的对齐与裁剪。

<details>
<summary>命令行</summary>

```powershell
.\.venv\Scripts\planner-replay.exe inspect run.bag
.\.venv\Scripts\planner-replay.exe bag run.bag --output captures/run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\planner-replay.exe bag --help
```

[ROS1 实时桥接](examples/ros1_bridge.py) · [验证记录](docs/VALIDATION.md)

</details>

## 许可证

[MIT License](LICENSE)
