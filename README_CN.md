# SLAM FPV Renderer

将 ROS1 SLAM 点云渲染为正后方跟随主画面和高位斜俯视小窗，可叠加实拍视频并导出 MP4 或 PNG。

[English](README.md) · [模拟演示](docs/synthetic-demo.mp4) · [验证记录](docs/VALIDATION.md)

## Windows 安装

安装 64 位 Python 3.10 或更高版本，保留安装器中的 Tcl/Tk。在 PowerShell 执行：

```powershell
git clone https://github.com/kevintjq/SLAM-FPV-Renderer.git
cd SLAM-FPV-Renderer
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e .
.\.venv\Scripts\python.exe -m slam_fpv.gui
```

## 操作

1. 在「数据源」打开 ROS1 `.bag`。从下拉框选点云话题和 Odometry 话题，或用录制的 TF。填写实际的世界与机体坐标系，机体系应为前左上（FLU）。增量扫描选「累积扫描」，重复发布的整图选「替换整图」。
2. 设置导出起点、时长和片段内的预览位置，点击「生成预览」。「画质与颜色」可选原始 RGB、按高度或自选纯色，轨迹颜色和线宽独立调整。「相机」可调跟随距离、高度和视场角。
3. 有实拍素材时，在「实拍视频」选择恒定帧率的视频，填时间偏移并重新生成预览，然后在预览中拖动视频、输入宽度。偏移 `+2` 表示视频从首帧点云后的第 2 秒开始出现。导出保留画面，音轨不混入。
4. 点击「导出 MP4 / PNG」。默认 1080p、30 fps、EDL、2× 超采样、CRF 15。若速度较慢，先把超采样调为 1 或降低分辨率。PNG 导出从起点开始的首个有效帧。

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
