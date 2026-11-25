<h1 align="center">SLAM-FPV-Renderer</h1>

<p align="center">正后方跟随 · 高位斜俯视画中画 · ROS1 时间同步 · VTK EDL 点云</p>

<p align="center">
  <img src="https://img.shields.io/badge/Python-3.10%2B-3776AB?style=flat-square" alt="Python 3.10+">
  <img src="https://img.shields.io/badge/ROS1-bag%20%2F%20TF%20%2F%20Odometry-22314E?style=flat-square" alt="ROS1">
  <img src="https://img.shields.io/badge/VTK-EDL-5B8DC7?style=flat-square" alt="VTK EDL">
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="MIT License">
</p>

<p align="center"><a href="README.md">English</a> · 简体中文</p>

<p align="center">
  <a href="#安装与快速演示">安装</a> · <a href="#录制真实数据">录制</a> ·
  <a href="#直接读取-rosbag">直接读包</a> · <a href="#通过-ros1-离线回放或订阅话题">ROS 回放</a> ·
  <a href="#坐标系与外参">坐标系</a> · <a href="#参数说明">参数</a> · <a href="#故障排查">故障排查</a>
</p>

---

把已有 SLAM 点云和位姿渲染为一个同步画面：**正后方跟随主视角 + 右上角高位斜俯视**。相机随航向移动并保持水平，RGB 三轴显示完整机体姿态，金黄色线显示已渲染的历史轨迹。没有无人机模型，不发布飞控指令。

![模拟双视角预览](docs/synthetic-preview.png)

**上图及仓库 MP4 为模拟场景和预设轨迹，不是实测飞行，也不是避障算法实验结果。** [观看模拟视频](docs/synthetic-demo.mp4)。真实包的读取与交叉检查单独记录在 [验证报告](docs/VALIDATION.md)，真实数据未上传。

| 功能 | 当前行为 |
| --- | --- |
| 点云 | VTK EDL、增量累积/整图替换、体素降采样、数量上限 |
| 主视角 | 默认位于机体正后方 3.8 m、上方 1.35 m，侧移为 0 |
| 画中画 | 后方 10 m、上方 6.7 m，约 34° 斜俯视；不是垂直俯视 |
| 姿态与轨迹 | 前景 RGB TF 轴、全姿态、金黄色历史轨迹，不添加飞机模型 |
| 数据 | ROS1 `.bag`、PointCloud2、Odometry、动态 TF 链与 `/tf_static` |
| 同步 | 消息 header 时间、平移插值、四元数 SLERP，禁止越界外推 |
| 导出 | PNG、恒定帧率 H.264 MP4、同名 JSON 运行报告 |
| ROS 订阅 | 独立 ROS Python 桥接到本机渲染器；实际 ROS 环境尚未联调 |

## 安装与快速演示

渲染器需要 **Python 3.10+** 和可用的 OpenGL 环境。实际验证平台为 Windows / Python 3.13。Linux 的 Python 3.10+ 是预期使用平台，本次没有验证。ROS1 的 Python 与渲染器 Python 可以独立运行，不要把 ROS Noetic 的 Python 3.8 强行装进渲染器虚拟环境。

```bash
git clone https://github.com/kevintjq/SLAM-FPV-Renderer.git
cd SLAM-FPV-Renderer
python -m venv .venv
```

Linux/macOS 激活：

```bash
source .venv/bin/activate
```

Windows PowerShell 激活：

```powershell
.\.venv\Scripts\Activate.ps1
```

安装依赖：

```bash
python -m pip install --upgrade pip
python -m pip install -e ".[test]"
```

先运行无需 ROS 的模拟演示：

```bash
slam-fpv demo --output captures/synthetic.mp4 --seconds 6
slam-fpv demo --output captures/synthetic.png --seconds 3 --start 2
```

默认导出 1280×720 / 24 FPS。PNG 保存起始偏移处的第一帧，MP4 按时间推进。视频包含落在采样网格上的终点，因此编码时长可能比请求跨度多一帧。若命令入口不在 PATH，可使用 `python -m slam_fpv.cli` 替代 `slam-fpv`。

`imageio-ffmpeg` 提供 FFmpeg。VTK 的离屏模式仍需要图形后端，不保证在任意无显示器服务器上直接可用。Linux X11 环境可尝试安装 Xvfb/Mesa 后使用 `xvfb-run -a slam-fpv ...`；本次没有验证此路径。

## 录制真实数据

先启动雷达驱动和 SLAM 系统。本工具只做渲染，不从原始雷达/IMU 自行建图。以下为一类 FAST-LIO 输出的话题示例，使用前请核对自己的名称和类型：

```bash
source /opt/ros/noetic/setup.bash
rostopic list
rostopic type /cloud_registered
rostopic echo -n 1 /cloud_registered/header
rostopic echo -n 1 /Odometry
```

建议录制：

```bash
rosbag record -O run.bag \
  /cloud_registered /cloud_registered_body /Odometry /tf /tf_static
```

至少要有一个所选 PointCloud2 话题、所选位姿源，以及点云/机体到世界系所需的完整 TF 链。核对 `/tf_static` 的静态变换确实被录入。完成后 Ctrl+C，等待 rosbag 正常关闭，再处理 `.bag` 文件。

只录 `/livox/lidar` 的 `livox_ros_driver2/CustomMsg` 和 IMU 不足以直接渲染。应先用匹配的驱动/SLAM 系统生成 PointCloud2 与位姿，再录制或导出其结果。本工具不会把原始雷达包伪装成建图结果。

## 直接读取 rosbag

此模式使用 `rosbags`，**不需要安装 ROS，也不需要 roscore**。先检查话题：

```bash
slam-fpv inspect /absolute/path/run.bag
```

读取世界系点云及 Odometry：

```bash
slam-fpv bag /absolute/path/run.bag \
  --cloud-topic /cloud_registered --odom-topic /Odometry \
  --world camera_init --body body \
  --start 1 --seconds 10 --output captures/run.mp4
```

读取机体系点云，并用 TF 作为位姿源：

```bash
slam-fpv bag /absolute/path/run.bag \
  --cloud-topic /cloud_registered_body --pose-source tf \
  --world camera_init --body body \
  --start 1 --seconds 10 --output captures/body-tf.mp4
```

导出一张图：

```bash
slam-fpv bag /absolute/path/run.bag --start 5 --output captures/run.png
```

`--start` 从第一条点云的 **header 时间**开始计算，不按文件名或包记录时间计算。开始偏移之前的点云仍会累积为地图历史；轨迹从实际导出区间开始。若 SLAM 发布的是完整地图，添加 `--cloud-mode replace`；若是每帧已配准扫描，保留默认 `accumulate`。

## 通过 ROS1 离线回放或订阅话题

这条路径用于已经运行 ROS 的机器；直接读包通常更适合确定性离线导出。先在 ROS Noetic 的系统环境安装桥接依赖：

```bash
sudo apt install ros-noetic-rospy ros-noetic-sensor-msgs ros-noetic-nav-msgs \
  ros-noetic-tf2-msgs ros-noetic-rospy-message-converter
```

**终端 1：** 加载 ROS 并启动 master。

```bash
source /opt/ros/noetic/setup.bash
roscore
```

**终端 2：** 激活 Python 3.10+ 渲染器环境，启动接收和导出。

```bash
source .venv/bin/activate
slam-fpv listen --output captures/replay.mp4 --seconds 10 --delay 0.3
```

**终端 3：** 使用 ROS 的系统 Python，不要激活渲染器环境。

```bash
source /opt/ros/noetic/setup.bash
rosparam set use_sim_time true
python3 examples/ros1_bridge.py \
  --cloud-topic /cloud_registered --odom-topic /Odometry
```

**终端 4：** 暂停启动回放，确认桥接就绪后按空格。

```bash
source /opt/ros/noetic/setup.bash
rosbag play run.bag --clock --pause
```

每次重新播放都要重新启动 listener 与 bridge。不要使用 `--loop` 向同一接收会话反复发送时钟回退的数据。桥接发送原始 header 时间，不以接收时间代替。

物理实时话题使用相同的 listener 与 bridge，直接运行雷达/SLAM 节点即可。两者必须位于同一主机，接收器只绑定 `127.0.0.1`；端口和话题参数必须相符。当前行为是离屏导出文件，没有交互式实时预览窗口。`listen` 从首个可用点云开始，`--start` 不用于此模式；达到时长或连接断开后结束。

**验证范围：** 本机 JSONL 回环传输及导出已有自动测试；ROS master、`rospy` 订阅、真实 `rosbag play` 和物理实时流尚未在本次 Windows 环境验证。桥接脚本独立于包运行，可使用 ROS Noetic 自带的 Python 3.8。

## 坐标系与外参

统一变换约定：

```text
p_parent = R_parent_child × p_child + translation_parent_child
p_world = T_world_cloud(t_cloud) × p_cloud
T_world_body(t) = T_world_odom_parent(t) × T_odom_parent_body(t)
```

| 项目 | 约定 |
| --- | --- |
| 位置 | 米 |
| 四元数 | `[x, y, z, w]`，归一化后使用 |
| 相机世界方向 | +Z 向上 |
| 机体前方 | +X 前、+Y 左、+Z 上，即 FLU |
| Odometry | `pose.pose` 表示 child 在 `header.frame_id` 中的位姿 |
| 世界系 | 可为局部初始化系；`camera_init` 不自动等同地理 ENU |

`--pose-source odom` 时，指定 Odometry 对 `--body` 具有优先权，TF 中所有以该机体为 child 的边会被忽略，以免两个发布源混用。Odometry 的 `child_frame_id` 必须匹配 `--body`。其余父系到世界系的变换仍从 TF 查找。选择 `tf` 时不读取 Odometry 位姿。

点云 header 已是世界系时不重复应用机体位姿。雷达系点云必须有完整 TF 链。缺少安装外参时，可以提供 JSON：

```json
[
  {
    "parent": "body",
    "child": "lidar",
    "translation": [0.0, 0.0, 0.0],
    "quaternion_xyzw": [0.0, 0.0, 0.0, 1.0]
  }
]
```

它表示 **body ← lidar**。上面的零偏移/单位旋转只是格式示例，必须用自己的标定替换。保存为文件后：

```bash
slam-fpv bag run.bag --cloud-topic /lidar_points \
  --extrinsics /absolute/path/extrinsics.json --output captures/run.mp4
```

不要重复提供包中已有的变换。本工具不会自动转换 NED、FRD 或相机 optical 系，不估计安装外参和时间偏移。应在上游正确发布或明确补齐变换。

### 时间同步规则

两个视角共用同一个输出时间和地图状态。点云按自身 header 时间变换，只在时间到达后加入地图；每个视频时刻的机体位置线性插值，姿态使用最短路径 SLERP。动态变换两侧样本间隔超过 `--max-gap` 会被拒绝，不进行越界外推。

包的存储时间、文件名日期和 header 时间可能不同，渲染以 header 为准。零时间戳、缺失路径和不支持的插值不会被悄悄替换成单位变换。开头无法支持的点云/位姿可跳过，并在 JSON 中计数；开始处理后的 TF 或位姿缺口会停止导出。失败可能留下部分视频，必须查看 JSON 的 `complete`、`error` 和终端退出码。退出码 0 表示正常完成，2 表示已处理的输入/配置/运行错误。

离线位姿样本按时间排序；点云 header 时间倒退则拒绝处理。系统不替数据修复错误时钟。实时桥接有有界等待和队列，可能在超载、缺失变换或连接结束时丢弃尚未渲染的点云；最终视频优先使用直接读包。

## 参数说明

完整命令帮助：`slam-fpv bag --help`、`slam-fpv demo --help`、`slam-fpv listen --help`。

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--cloud-topic` | `/cloud_registered` | PointCloud2 话题 |
| `--odom-topic` | `/Odometry` | Odometry 话题 |
| `--world` / `--body` | `camera_init` / `body` | 世界系与跟踪机体系 |
| `--pose-source` | `odom` | 位姿来源：`odom` 或 `tf` |
| `--max-gap` | `0.5` | 动态变换最大插值跨度，秒 |
| `--extrinsics` | 无 | 静态外参 JSON 路径 |
| `--output` | 必填 | `.png` 或 `.mp4`，自动生成同名 `.json` |
| `--start` / `--seconds` | `0` / `6` | 离线起始偏移与最大跨度，秒 |
| `--width` / `--height` | `1280` / `720` | 输出尺寸，须为偶数 |
| `--fps` | `24` | MP4 帧率 |
| `--distance` | `3.8` | 主相机后方距离，米 |
| `--camera-height` | `1.35` | 主相机相对机体高度，米 |
| `--side` | `0` | 主相机左侧偏移，米；0 为正后方 |
| `--fov` | `65` | 垂直视场角，度 |
| `--inset-distance` / `--inset-height` | `10` / `6.7` | 画中画后方距离/高度，米 |
| `--axis` | `0.48` | TF 标称轴长，米 |
| `--axis-pixels` | `28` | TF 最小屏幕尺度目标 |
| `--point-size` | `2` | 点云栅格尺寸，像素 |
| `--color` | `rgb` | 优先 packed RGB，无 RGB 则按高度；`height` 强制高度色 |
| `--voxel` / `--max-points` | `0.08` / `200000` | 体素边长（米）/保留点数上限 |
| `--cloud-mode` | `accumulate` | 增量累积；完整地图改为 `replace` |
| `--no-edl` | 关闭 | 禁用 EDL，用于对比或排查显卡问题 |
| `--port` / `--delay` | `8765` / `0.3` | 本机桥接端口/等待 TF 的延迟，秒 |

画中画位于右上角，宽约 32%、高约 33%。相机只跟随 yaw，不跟随机体 roll；TF 轴仍显示完整姿态。TF 根据距离统一放大三轴，轴长不能当作测距标尺。正后方观察时 X/Z 可能发生真实投影重叠；可增大 `--axis-pixels`、查看画中画或设置少量 `--side`。轴以前景显示，故意忽略点云遮挡；这不是深度测量结果。

## 可执行测试

```bash
python -m pytest -q
```

测试包括坐标链和逆变换、静态外参、SLERP 与四元数符号、缺失/越界/超长插值拒绝、PointCloud2 行填充/大小端/RGB/NaN、地图上限、相机正后方与约 34° 高位视角、EDL 与 RGB 可见性、MP4 解码/帧数/运动、生成的 ROS1 包、JSONL 回环传输。测试中生成的包均为模拟夹具。

真实数据交叉检查可自行执行：

```bash
python examples/validate_real_bag.py /absolute/path/run.bag
```

该脚本默认比较 `/cloud_registered` 与 `/cloud_registered_body` 经 TF 变换后的对应点，同时比较 Odometry/TF 位姿。只有包确实包含这些话题和相同点序时才适用。它检查两条数据路径的一致性，不代表 SLAM 绝对精度或标定精度。实际执行结果和未验证范围见 [验证报告](docs/VALIDATION.md)。

## 故障排查

| 现象 | 处理方法 |
| --- | --- |
| 找不到点云或类型不匹配 | 先 `inspect`，确认是 PointCloud2；原始 Livox CustomMsg 需上游转换/SLAM |
| No timestamped TF path | 检查 frame、外参方向、`/tf_static`、header 时间；初始化不足时增大 `--start` |
| Transform gap / 部分视频 | 检查丢失跟踪与时间缺口，缩短或拆分区间；放宽阈值不能修复数据 |
| 黑屏、白屏、着色器错误 | 检查显卡/OpenGL 和离屏后端，用 `--no-edl` 比较；Linux 可尝试 Xvfb/Mesa |
| TF 太小或重叠 | 增大 `--axis-pixels`，查看高位画中画，或少量侧移 |
| 点云重影 | 核对坐标系、时间和外参；完整地图话题使用 `replace`；累积地图不修正回环 |
| 渲染慢 | 增大 `--voxel`、降低 `--max-points`、分辨率或 FPS |
| 无法生成 MP4 | 检查偶数尺寸、写入权限、FFmpeg、可用磁盘空间 |
| 桥接一直等待 | 按 listener → bridge → rosbag play 顺序启动，确认话题和端口 |
| 桥接导入失败 | 在已 source 的 ROS 系统 Python 中运行并安装 `rospy_message_converter` |
| 桥接丢帧 | 减慢 rosbag 回放，降低渲染开销；TF 延迟大可适度增大 `--delay` |

## 已知限制与源码来源

- 本仓库依据可恢复的对话需求重新实现。旧附件 `SLAM-FPV-Renderer-v0.1.zip`、`edl_avoidance_demo.py`、`rear_chase_demo.py` 在本次会话中无法取得。`examples/` 内同名脚本是新实现的演示入口，不是恢复出的原文件；尚未逐行核对兼容性。
- 不实现 SLAM、避障、相机碰撞检测、点云逐点去畸变、外参/时间标定，不对规划效果作评估。
- 当前支持 ROS1 PointCloud2 + TF/Odometry，不支持 ROS2、原始 Livox CustomMsg、PCD 导入、相机图像叠加或 GUI 编辑器。
- RGB 只支持单标量 32 位 packed `rgb`/`rgba`；不支持分离的 r/g/b 字段或强度色图。自动剔除非有限 XYZ，高度配色范围为固定示意范围。
- 离线先读取全部 TF/Odometry 到内存，再读取点云。大包会增加索引内存；地图体素化且限点，不是无损地图。已累积点不随后续回环优化重新变换。
- EDL 效果依赖 GPU/驱动；不是物理光照模型。两路视角共用状态，不代表两台实际相机。
- 桥接队列、位姿历史和实时轨迹均有上限，尾部缓冲在断连时可能不导出，丢帧可能缩短实时视频。直接读包更适合最终结果。
- 已验证范围为文档列出的 Windows 环境、模拟夹具和一份本地真实包；Linux/macOS、实际 ROS 订阅/回放、物理实时流、超大包和其他设备坐标约定尚未验证。

## 项目结构

```text
src/slam_fpv/
  geometry.py       变换链、插值、相机位置
  cloud.py          PointCloud2 与有界体素地图
  sources.py        ROS1 包索引和消息时间数据源
  renderer.py       VTK EDL、RGB TF 与同步画中画合成
  cli.py            参数、离线时间线、PNG/MP4/JSON 导出
  live.py           本机桥接接收
examples/           ROS1 桥接、外参模板、重新实现的演示入口、真实包检查
tests/              单元、读包、渲染、视频和回环传输测试
docs/               标注的模拟演示与验证记录
```

提交使用 Conventional Commits，例如 `feat(tf): add timestamped transforms`、`test(renderer): verify video export`。提交前运行测试，明确区分模拟与实测证据。

## 许可证

新代码采用 [MIT License](LICENSE)。无法取得的原附件和外部数据集不包含在本仓库中，也未被重新授权。
