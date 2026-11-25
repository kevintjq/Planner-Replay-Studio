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
3. 在「实拍视频」选文件后打开「裁剪与时间对齐」。逐帧设入点、出点，拖框保留所需画面。裁剪保留素材的原始时间轴。支持按实际帧时间戳读取可变帧率视频。
4. 在左右画面找同一个转向或起飞动作，记录第一个锚点。长视频可在靠后的位置再记一个锚点，校正两段时间轴的漂移。也可直接填偏移，`+2` 表示视频第 0 秒对应首帧点云后的第 2 秒。应用后生成预览并拖动视频小窗。
5. 导出 MP4 或 PNG。默认 1080p、60 FPS、EDL 和 2× 超采样。MP4 自动测试 NVIDIA NVENC，初始化失败时用 CPU H.264。实拍画面随合成导出，未混入音轨。

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
