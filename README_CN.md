# SLAM FPV Renderer

把 ROS1 SLAM 点云和位姿做成多机位飞行画面，可叠加实飞视频、文字、速度表和可选的规划、传感器图层。

[English](README.md) · [验证记录](docs/VALIDATION.md)

![工作台模拟效果，包含空间叠加面板、飞行安全走廊和稳定 FPV](docs/studio-preview.png)

*截图使用模拟数据。*

## Windows 启动

安装带 Tcl/Tk 的 64 位 Python 3.10 或更高版本。克隆仓库后，双击 [Start_SLAM_FPV.cmd](Start_SLAM_FPV.cmd)。首次启动会联网安装依赖。想先看效果，可以直接点「使用模拟数据」。

## 合成流程

1. **读取数据。** 在「数据」打开 ROS1 `.bag`，自动读取点云、里程计话题及坐标系，可用下拉框更换，再设置导出起点与时长。
2. **选机位。** 右上角选择主视角，从「图层 → + 视角」添加小窗。共有 12 个机位可选，包括稳定 FPV、原始 FPV、高位斜视、左右跟随和环绕。两种 FPV 都隐藏实飞和规划轨迹线，角落三轴显示原始机体姿态。
3. **组合图层。** 实拍可作为底片或画中画，支持裁剪及双锚点对齐。可添加定时文字和 m/s 速度表。选中图层后，拖动位置，拉右下角缩放，也可以切换透明底或深色底。
4. **导出。** 支持 MP4 和 PNG，视频默认 1080p、60 FPS。自动检测 NVIDIA NVENC，必要时回退到 CPU H.264。

## 空间叠加

在「空间叠加」按需勾选，重新生成预览。话题和传感器坐标系自动识别，也可从下拉列表更换。

| 图层 | 数据来源 |
| --- | --- |
| 规划轨迹 | `nav_msgs/Path` 或 `visualization_msgs/Marker[Array]` |
| 飞行安全走廊 | `visualization_msgs/Marker[Array]`，支持折线、线段集、三角面和立方体 |
| 相机 FOV | 根据 `CameraInfo` 和光学 TF 绘制紧凑的细线视锥 |
| 雷达当前帧高亮 | 高亮最近一帧 PointCloud2、Livox CustomMsg 或 LaserScan |

录包时一并录制规划器的可视化话题。传感器坐标系通过 TF 或外参 JSON 连到世界坐标系。已飞轨迹和规划轨迹的线宽在「画质」中分别调整。缺少数据或 TF 时跳过相应图层。找不到单帧扫描时，高亮主点云话题的最新一帧。

## 命令行

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output captures/run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

使用 TF 时选 `--pose-source tf`，按需录制 `/tf` 与 `/tf_static`。实时订阅与回放见 [ROS1 桥接](examples/ros1_bridge.py)。实录 bag、导入素材和导出视频保存在本地。

[MIT 许可](LICENSE)
