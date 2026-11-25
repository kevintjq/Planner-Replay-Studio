# SLAM FPV Renderer

将 ROS1 SLAM 点云渲染为正后方跟随主画面和高位斜俯视小窗，可叠加实拍视频并导出 MP4 或 PNG。

[English](README.md) · [模拟演示](docs/synthetic-demo.mp4) · [验证记录](docs/VALIDATION.md)

## Windows 安装

安装 64 位 Python 3.10 或更高版本并保留 Tcl/Tk。克隆仓库后，双击 [Start_SLAM_FPV.cmd](Start_SLAM_FPV.cmd)。首次启动联网安装依赖，以后直接打开。

## 操作

1. 在「数据」打开 ROS1 `.bag`，选择点云和 Odometry 或 TF，确认世界与机体坐标系。设置导出起点和时长。
2. 在「图层」一次导入多个视频。选中某一视角后用「裁剪 / 对齐」设置其入点、出点、画幅及时间锚点。点「替换视频」更换素材。点「设为全屏底片」把该视频放到点云下方，其他视频仍可自由摆放。
3. 添加文字或速度表。选中图层后可输入显示时间，也可在下方时间轴拖动区间两端。拖动蓝色播放头定位预览。时间均从首帧点云计起。速度表显示由世界坐标系位姿估算的三维速度（m/s），最高和平均值从导出起点累计。
4. 在预览里直接拖动叠加图层。高位斜视小窗背景透明，两个视角都显示 TF 三轴。文字和速度表使用透明底，实拍小窗可调整不透明度。导出 MP4 或 PNG，默认 1080p、60 FPS。自动编码先试 NVIDIA NVENC，再回退 CPU H.264。

「画质」默认使用 **SLAM 原貌 + SLAM 高度彩虹**。高度范围留空时，从首批非空点云确定，并在整段保持固定。也可输入两个世界坐标系 Z 值，如 `-1 4`。体素填 `0` 可跳过体素筛选，点数上限仍生效。已有地图按原始颜色渲染时可选「原始 RGB」。

## 命令行与录制

```powershell
.\.venv\Scripts\slam-fpv.exe inspect run.bag
.\.venv\Scripts\slam-fpv.exe bag run.bag --output run.mp4 --cloud-topic /cloud_registered --odom-topic /Odometry --world camera_init --body body
.\.venv\Scripts\slam-fpv.exe bag --help
```

在 ROS1 主机上录制实际的话题。例如：

```bash
rosbag record -O run.bag /cloud_registered /Odometry /tf /tf_static
```

TF 模式选 `--pose-source tf`。传感器坐标系点云需要连接到世界坐标系的 TF，或通过 [外参模板](examples/extrinsics.json) 填入实测外参。实际 ROS1 订阅或回放使用 [ROS1 桥接](examples/ros1_bridge.py) 和 `slam-fpv listen --help`。Windows 可直接读包，无需安装 ROS1。
