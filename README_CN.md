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

1. 打开 bag 后导入实拍，工作台会打开第一路视频的对齐窗口。其他路从「裁剪 / 对齐」打开。
2. 在左右两边找到同一个动作，点「对齐这两帧」保存。需要校正时钟漂移时，再到「精调」记录第二个锚点。
3. 「精调」还提供候选动作、循环核对和中段检查。

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

- 点「播放」连续预览。时间轴聚焦后，空格播放或暂停，方向键逐帧。拖动播放头会请求低分辨率预览，导出仍使用设定的分辨率与帧率。
- 选中视频并按 S 分割，按 Delete 删除播放头所在片段，或从片段右键菜单操作。裁剪、分割和删除保留原有同步，删除时在同一时间位置留空，原声同步静音。Ctrl+Z / Ctrl+Y 撤销或重做，「吸附」可对齐事件、布局和片段边缘。
- 点击紫色布局标记跳转，拖动标记调整时间。右键「实拍切到小窗」可单独调整该标记的尺寸、位置和 0–5 秒过渡时长。
- 选中图层，把播放头放在切换时刻，点「在这里缩成小窗」。默认平滑过渡 0.4 秒。过渡结束后直接拖动小窗，或右键绿色标记设置位置和精调。
- 点击「+ 事件」在播放头处打标记，右键改名或删除。点击标记即可跳转，也可从下拉列表跳到重规划或目标更新。
- 视频、文字和速度表各占一轨。拖动片段两端裁剪，拖动视频片段中间能平移它的显示区间，仍按原有锚点取帧。勾选「对比上一条规划」可看灰色虚线。
- 把播放头放到切换时刻，从「添加布局标记」选择当前合成、点云全屏或一路实拍全屏。
- 同一段实拍先作底片再变小窗时，保留点云透明底，在对应时刻依次为同一路素材添加「实拍作底片（保留叠加）」和「实拍切到小窗」。小窗从第二个标记起平滑缩入，默认 0.4 秒。

## 保存与导出

| 项目 | 说明 |
| --- | --- |
| `.slamfpv` | 保存素材路径、裁剪、锚点、事件与布局标记和导出参数，素材移动后可重新定位 |
| MP4 | 默认 1080p、60 fps，自动选择 NVIDIA NVENC 或 CPU H.264；可选一路原声及 0.5× / 0.25× 慢放 |
| PNG | 勾选「透明画布」保留透明通道 |

点「导出 1080p · 60 fps」选文件位置即可开始。「更多设置」可选择 MP4 或 PNG，并自定义分辨率、帧率、编码、原声和慢放。

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
