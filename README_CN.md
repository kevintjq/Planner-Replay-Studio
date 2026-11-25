# SLAM FPV Renderer

把 ROS1 SLAM 点云和位姿做成多机位飞行画面，可叠加实飞视频、文字和速度表。

[English](README.md) · [验证记录](docs/VALIDATION.md)

![工作台模拟效果，包含点云主画面、高位斜视和稳定 FPV](docs/studio-preview.png)

*截图使用模拟数据。*

## Windows 启动

安装带 Tcl/Tk 的 64 位 Python 3.10 或更高版本。克隆仓库后，双击 [Start_SLAM_FPV.cmd](Start_SLAM_FPV.cmd)。首次启动会联网安装依赖。想先看效果，可以直接点「使用模拟数据」。

## 合成流程

1. **读取数据。** 在「数据」打开 ROS1 `.bag`，选择 PointCloud2 和 Odometry 或 TF，设置导出起点与时长。
2. **选机位。** 右上角选择主视角，从「图层 → + 视角」添加小窗。共有 12 个机位可选，包括稳定 FPV、原始 FPV、高位斜视、左右跟随和环绕。两种 FPV 都隐藏轨迹线，角落三轴显示原始机体姿态。
3. **组合图层。** 实拍可作为底片或画中画，支持裁剪及双锚点对齐。可添加定时文字和 m/s 速度表。选中图层后，拖动位置，拉右下角缩放，也可以切换透明底或深色底。
4. **导出。** 支持 MP4 和 PNG，视频默认 1080p、60 FPS。自动检测 NVIDIA NVENC，必要时回退到 CPU H.264。

## 命令行

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output captures/run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

使用 TF 时选 `--pose-source tf`，按需录制 `/tf` 与 `/tf_static`。实时订阅与回放见 [ROS1 桥接](examples/ros1_bridge.py)。实录 bag、导入素材和导出视频保存在本地。

[MIT 许可](LICENSE)
