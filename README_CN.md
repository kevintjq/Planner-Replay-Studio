# Planner Replay Studio

在 Windows 上回放 ROS1 SLAM 点云，制作无人机演示视频。

[English](README.md) · 简体中文

## Windows 原生预览版

1. 打开 [Windows 构建页面](https://github.com/kevintjq/Planner-Replay-Studio/actions/workflows/native-windows.yml?query=branch%3Amain+is%3Asuccess)，选择 `main` 上最近一次成功的构建。
2. 在 **Artifacts** 下载 **PlannerReplayNative-Windows-x64**，完整解压 ZIP。
3. 双击 `PlannerReplayNative.exe`，打开 ROS1 `.bag` 或使用内置模拟数据。

原生版使用 C++ / Qt Quick，运行需要支持 OpenGL 3.3 的显卡驱动，无需安装 Python、Tk 或 ROS。[原生版用法和构建说明](native/README.md)

![原生预览版回放示例 ROS1 bag](docs/native-preview.png)

## 选哪个版本

| | 原生预览版 | 视频合成工作台 |
| --- | --- | --- |
| 启动方式 | 双击 `PlannerReplayNative.exe` | 双击 [`Start_Planner_Replay.cmd`](Start_Planner_Replay.cmd)，需要带 Tcl/Tk 的 Python 3.10+ |
| 点云回放 | `PointCloud2`、累积或整图替换、机体距离着色、已飞轨迹和 `Odometry` 速度 | 多机位、TF 及 EGO / SUPER 可选规划图层 |
| 剪辑与导出 | 播放、拖动时间轴、查看点云 | 实拍对齐、定时叠加、固定机位拖影图、1080p/60 fps 导出 |

需要制作演示视频时，见 [视频合成工作台指南](docs/studio.zh-CN.md)。

## 仓库目录

| 路径 | 内容 |
| --- | --- |
| [`native/`](native/) | C++ / Qt Quick 应用、ROS1 bag 读取和原生测试 |
| [`src/slam_fpv/`](src/slam_fpv/) | Python 视频合成工作台和命令行 |
| [`tests/`](tests/) | Python 工作台测试 |
| [`docs/`](docs/) | 使用指南、截图和 [验证记录](docs/VALIDATION.md) |
| [`examples/`](examples/) | ROS1 桥接、外参模板和 bag 验证工具 |

[MIT License](LICENSE)
