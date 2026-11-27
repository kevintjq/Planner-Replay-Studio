# Planner Replay Studio

在 Windows 上回放 ROS1 点云、查看无人机规划数据并剪辑飞行视频。

[English](README.md) · 简体中文

[下载 Windows x64 安装包](https://github.com/kevintjq/Planner-Replay-Studio/releases/download/studio-v0.3.1/PlannerReplayStudio-Setup.exe)，双击安装后，从开始菜单打开 **Planner Replay Studio Full**。运行环境随安装包提供。

![Planner Replay Studio 模拟点云与规划叠加](docs/studio-preview.png)

## 快速开始

1. 在「数据」中打开 ROS1 `.bag`，或选择「使用模拟数据」。按需选择话题和视角，规划轨迹、目标点、安全走廊、雷达当前帧和速度均可选。
2. 添加实拍视频、图片或文字，在时间轴上对齐、裁剪并调整显示时段，在预览画面中拖动或缩放图层。
3. 点击「导出 1080p · 60 fps」生成视频，或用「更多设置」调整导出参数。「固定机位轨迹图」可单独导出无人机拖影图。
4. 点击「导出项目文件夹」打包项目与素材。换电脑时复制整个文件夹，在工作台中打开里面的 `project.slamfpv` 即可继续编辑。

[工作台指南](docs/studio.zh-CN.md) · [MIT 许可证](LICENSE)
