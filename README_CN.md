<h1 align="center">SLAM FPV Renderer</h1>

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
  <a href="#保存与导出">保存与导出</a>
</p>

---

在 Windows 上把 ROS1 SLAM 点云、位姿和实飞视频合成多视角画面。

![SLAM FPV 工作台模拟效果](docs/studio-preview.png)

| 功能 | 说明 |
| --- | --- |
| 点云视角 | 12 种机位，含稳定 FPV、后方跟随和高位斜视 |
| 多路视频 | 每路独立裁剪、对齐和布局，可作为底片或画中画 |
| 可选叠加 | 规划轨迹、安全走廊、相机 FOV、雷达当前帧、速度表和定时文字 |

## 快速开始

1. 安装带 Tcl/Tk 的 64 位 Python 3.10 或更高版本。
2. 克隆仓库，双击 [Start_SLAM_FPV.cmd](Start_SLAM_FPV.cmd)。首次启动会下载依赖。
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
| 规划轨迹 | `Path`、`Marker` 或 `MarkerArray` |
| 安全走廊 | `Marker` 或 `MarkerArray`，支持折线、线段、三角面和立方体 |
| 相机 FOV | `CameraInfo` 与光学坐标系 TF |
| 雷达当前帧 | `PointCloud2`、Livox `CustomMsg` 或 `LaserScan` |

话题和坐标系自动识别，可从下拉列表更换。先在「空间叠加」启用数据，再到各视角的设置里选要显示的叠加项。「画质」可分别调整已飞和规划轨迹的线宽。预览会提示扫描来源、帧龄和缺失的数据。

## 保存与导出

| 项目 | 说明 |
| --- | --- |
| `.slamfpv` | 保存素材路径、裁剪、锚点、图层布局和导出参数，素材移动后可重新定位 |
| MP4 | 默认 1080p、60 fps，自动选择 NVIDIA NVENC 或 CPU H.264 |
| PNG | 勾选「透明画布」保留透明通道 |

<details>
<summary>命令行</summary>

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output captures/run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

[ROS1 实时桥接](examples/ros1_bridge.py) · [验证记录](docs/VALIDATION.md)

</details>

## 许可证

[MIT License](LICENSE)
