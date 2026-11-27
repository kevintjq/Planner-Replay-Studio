<h1 align="center">Planner Replay Studio</h1>

<p align="center">ROS1 点云回放 · EGO / SUPER 规划叠加 · 多路实飞合成</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-0078D4?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/Input-ROS1%20bag-22314E?style=flat-square" alt="ROS1 bag">
  <img src="https://img.shields.io/badge/Export-60%20fps-00599C?style=flat-square" alt="60 fps">
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="MIT License">
</p>

<p align="center"><a href="README.md">English</a> · 简体中文</p>

<p align="center">
  <a href="#快速开始">快速开始</a> ·
  <a href="#输入与叠加">输入与叠加</a> ·
  <a href="#多路视频对齐">多路视频对齐</a> ·
  <a href="#保存与导出">保存与导出</a>
</p>

---

![Planner Replay Studio 模拟数据预览](docs/studio-preview.png)

## 快速开始

1. [下载 Windows x64 安装包](https://github.com/kevintjq/Planner-Replay-Studio/releases/download/studio-v0.3.2/PlannerReplayStudio-Setup.exe)，双击安装，从开始菜单打开 **Planner Replay Studio Full**。默认装到 `%LOCALAPPDATA%\Programs\Planner Replay Studio Full`，安装时可改路径，也可勾选桌面快捷方式。
2. 在「数据」打开 ROS1 `.bag`，或点「使用模拟数据」。确认点云和位姿话题，选择视角后点「生成预览」。
3. 在「图层」添加实拍、图片或文字。拖动画面调整位置和大小，在时间轴上调整显示时段或添加缩窗过渡。

## 输入与叠加

| 数据 | 支持内容 |
| --- | --- |
| 点云与位姿 | `PointCloud2`、`Odometry` 和 TF。支持累积扫描、整图替换及按机体距离着色 |
| 规划 | EGO / SUPER 规划轨迹、目标点和安全走廊。各图层可单独开关 |
| 传感器 | 相机 FOV、雷达当前帧高亮、话题线速度（m/s）。在速度表的「设置」中选择速度话题 |

在「数据 → 空间叠加」启用所需数据并确认自动识别结果。可从下拉列表更换话题，并在各视角单独选择要显示的图层。

## 多路视频对齐

1. 在「图层」添加视频，逐路打开「裁剪 / 对齐」。
2. 在回放和实拍画面中找到同一个动作，点击「对齐这两帧」。
3. 时间越往后偏差越大时，在「精调」添加第二个锚点，并用中段检查复核。

## 保存与导出

| 产物 | 操作 |
| --- | --- |
| 项目文件夹 | 点「导出项目文件夹」打包项目和素材。迁移时整夹复制并打开 `project.slamfpv` |
| MP4 或透明 PNG | 点「导出 1080p · 60 fps」生成视频，或在「更多设置」调整输出。透明 PNG 需勾选「透明画布」 |
| 无人机拖影图 | 打开「固定机位轨迹图」，选起始帧、框选无人机、设置采样间隔后保存 PNG |

[工作台指南](docs/studio.zh-CN.md) · [MIT 许可证](LICENSE)
