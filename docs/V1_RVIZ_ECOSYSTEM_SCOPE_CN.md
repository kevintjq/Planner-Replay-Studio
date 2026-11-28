# v1 RViz 生态范围调整与真实数据缺口

记录：2026-11-28。依据用户最新发行验收调整：本次优先 FAST-LIO、EGO-Planner、SUPER
常用 Display 的真实渲染及配置兼容；其他原生默认/第三方插件可保留结构通过但真实渲染未验证，
明确列为后续独立 PR。此前完整插件兼容目标、有效实现和失败证据保留。本文件记录范围与
证据，**不代表核心生态已经通过**，不授权稳定版发布。

## 核心与后续边界

v1 核心为 Grid、Axes、Group、TF、PointCloud2、Odometry、Path、Marker、MarkerArray、Pose。
Group 验收嵌套、实例身份、父子显隐、属性和配置持久化；它本身没有传感器画面。
TF 同时用于实际 Frame 显示和应用既定的精确时间/跟随门禁。PointCloud2、Odometry、Path、
Marker/Array、Pose 需实际相应消息及完整 TF，不能从里程计生成另一个消息类型冒充源数据。

官方配置内默认禁用的深度 Image、其他非核心默认插件及未安装第三方插件/工具，保留配置
与明确诊断；真实渲染和特殊交互转后续 PR。不能以“配置已导入”声称它们已显示，也不能
静默删除无法运行的节点。若真实核心项目实际依赖清单外插件，必须记录具体 Class、依赖及
受影响流程，明确交付限制；不能把失败自动改记为通过。

本次只调整插件实渲染验收顺序。多视角独立 Display/材质、TF 跟随、共享自由画布、项目
保存重开、精确 PTS 与随机跳转、速度表、视频/单帧/透明/矢量或混合导出、干净前景、
性能、安装包依赖完整、安全同路径覆盖升级仍按核心矩阵逐项验收。
同日最新用户调整：干净Windows安装/运行移交用户正式发布后手测，不再作为v1前置门槛，
不再准备或寻找VM/新环境；该移交不是本机通过声明，包依赖完整和安全覆盖升级要求不变。

## 官方配置依据

以下是实际读取的上游源码快照，不使用 README 截图推定支持。提交与文件 Git blob 固定，
其他分支、ROS2 配置及用户修改版不自动归入本范围。源配置中的话题名称只用于说明，软件
仍须从实际 Bag/ROS 目录选择，不能把名字硬编码为接收数据的条件。

| 生态 / 固定提交 | 实际配置及 Git blob | 常用显示与配置中的实际话题示例 |
| --- | --- | --- |
| FAST-LIO / `7cc4175de6f8ba2edf34bab02a42195b141027e9` | [rviz_cfg/loam_livox.rviz](https://github.com/hku-mars/FAST_LIO/blob/7cc4175de6f8ba2edf34bab02a42195b141027e9/rviz_cfg/loam_livox.rviz)；blob `ea3cdf3589918eaa09209202db44103b401eda67` | Grid、Axes、Group、PointCloud2（`/cloud_registered`、`/Laser_map`、`/cloud_effected`）、Odometry（`/Odometry`）、Path（`/path`）；另保留默认禁用的MarkerArray（`/MarkerArray`） |
| EGO-Planner / `bfda51284c8c1b476043255a8145ef925a3778a5` | [src/planner/plan_manage/launch/default.rviz](https://github.com/ZJU-FAST-Lab/ego-planner/blob/bfda51284c8c1b476043255a8145ef925a3778a5/src/planner/plan_manage/launch/default.rviz)；blob `c24cd62bd2c455505497e92ef511c0e29deeba69` | Axes、Grid、Group、PointCloud2（`/map_generator/global_cloud`、`/grid_map/occupancy_inflate`、`/grid_map/occupancy`）、Marker（`/ego_planner_node/goal_point`、`/ego_planner_node/optimal_list`、`/odom_visualization/robot`）、Path（`/odom_visualization/path`）；深度Image默认禁用 |
| SUPER / `2ad3419c127a617c6d7df6925e81a14175a9c096` | [super_planner/rviz/real_svc.rviz](https://github.com/hku-mars/SUPER/blob/2ad3419c127a617c6d7df6925e81a14175a9c096/super_planner/rviz/real_svc.rviz)；blob `ac1c0ec7ff2e0ddadcdf5b1ef569f2c51bc1094a` | Grid、Axes、PointCloud2（`/cloud_registered`、`/fsm_node/rog_map/occ`、`/fsm_node/rog_map/inf_occ`）、Odometry（`/lidar_slam/odom`）、Path（`/odom_visualization/path`）、Marker（`/odom_visualization/robot`）、MarkerArray（`/fsm_node/visualization/exp_sfcs`、`/fsm_node/visualization/exp_traj`、`/fsm_node/visualization/committed_traj`） |
| SUPER / 同上 | [super_planner/rviz/onboard.rviz](https://github.com/hku-mars/SUPER/blob/2ad3419c127a617c6d7df6925e81a14175a9c096/super_planner/rviz/onboard.rviz)；blob `fc6267fc73c9ffc873a5e873e98d66f0ea6cad17` | 补充嵌套Group、已启用Pose（`/planning/click_goal`）与多组MarkerArray；深度Image默认禁用，第三方`rviz_plugins/Goal3DTool`属于工具，不是Display |

补充读取 SUPER 同提交的 `super_planner/rviz/default.rviz`（blob
`8999f867da027014759f0013dd8a5585e3e6d6ee`）与 `fpv.rviz`（blob
`96a1984c4d27aecf050836c9a95992697fa1e82b`），可确认 PointCloud2、Marker/Array、Group、
Odometry 等复用以及默认禁用Image。配置中的Orbit/ThirdPersonFollower、固定坐标系和未知
Tool字段必须保留；不能因为未安装第三方Goal3DTool而宣称工具完整支持。

## 当前已有证据与缺口

当前原生Noetic宿主SHA256为
`8C87BB91B70B715D16511B5FDF91CBF250313E6F4DE506D9995DB426F5BC1A29`。
`captures/rviz-plugins-8c87-20261128/report.json` 已独立覆盖33类结构/547个属性、23次枚举、
17次条件变化及配置/资源恢复；真实输出只有五类七图。详见
[逐插件矩阵](RVIZ_COMPATIBILITY_MATRIX_CN.md)；本次范围调整没有重新运行宿主或改写历史报告。

| 核心显示 | 已有真实证据 | v1仍需补齐 |
| --- | --- | --- |
| Grid / Axes | 8C87原生各一张实际输出；独立空场景另有GUI记录 | 当前构建空场景/属性/主题/保存重开，配置中的尺寸与Frame对照 |
| Group | 原生实例/属性/YAML及删除资源结构通过 | 三生态实际嵌套配置的父子显隐、独立实例、排序、保存重开及GUI |
| TF | 8C87一张实际Frame输出及准确时间；另有原生时间/客户端相机专项 | 当前构建跟随/缺TF/恢复、随机跳转和全部视角交互 |
| PointCloud2 | 8C87两路真实点云输出 | 官方/用户核心配置的样式、颜色、衰减、地图替换/累积、TF及多实例/多视角对照；两图不覆盖全部属性 |
| Odometry | 8C87两路真实里程计输出 | 原生箭头/坐标轴/协方差适用属性、历史、准确时间、多实例与保存恢复对照 |
| Path | 33类结构中真实Property/YAML通过，用户FAST-LIO配置已做历史字节往返 | **缺原生录制的`nav_msgs/Path`源**；路径、样式/历史/TF、跳转与输出；明确标注的Odometry派生夹具不能算原生Path录制 |
| Marker | 原生结构通过；独立解码和受控夹具不等同真实规划数据 | **缺真实`visualization_msgs/Marker`源**；目标点/轨迹、类型、namespace/id、颜色、DELETE/生命周期、mesh资源与TF |
| MarkerArray | 原生结构及受控多实例检查通过 | **缺真实`visualization_msgs/MarkerArray`源**；SUPER轨迹/走廊/目标、顺序/删除/历史与TF、图片/视频对应时刻 |
| Pose | 原生结构通过，SUPER onboard配置明确启用 | **缺真实`geometry_msgs/PoseStamped`源**；目标姿态、箭头/坐标轴、TF、属性及配置/输出对照 |

当前已核查的真实Bag目录只有8个Topic：`/rosout`、`/cloud_registered_body`、
`/cloud_registered`、`/Odometry_imu`、`/livox/imu`、`/livox/lidar`、`/Odometry`、`/tf`。
因此Path/Marker/MarkerArray/Pose的生态核心真实数据门禁仍开放；其他素材或真实在线输出
只有完成目录/消息/TF/时间及来源核查后才可增补，不能用本Bag的五类七图替代。
可以从真实Odometry的原Header/pose派生明确语义的标准消息兼容性夹具，用于检查显示/时间/属性
通路；派生过程及原始数据归属必须公开，原Bag不变。这类检查不等于原生EGO/SUPER规划输出实测。

`/livox/lidar` 为 `livox_ros_driver2/CustomMsg`，不是标准PointCloud2插件消息；目录/字段读取
不等于librviz直接渲染该自定义消息。EGO自定义Bspline的独立解析同样不能替代原生Marker/Path
或第三方插件验收。IMU已投递1594条但缺`livox_frame` TF，无原生图片，继续保持失败。

历史FAST-LIO 11178字节配置的无编辑项目往返仅证明原文字节保留；缺少`/path`、配置中
Target Frame=`global`而真实源无该Frame等情况必须诊断。为验收修改过Fixed Frame/禁用无源项
的副本不能冒充原始配置无需修改即可完整显示。

## 后续PR与发行判定

- 其他默认插件（例如Image/Camera/DepthCloud、LaserScan、Map、RobotModel、InteractiveMarkers等）
  保留现有结构结果、实际依赖和未验状态；按真实消息、图像/CameraInfo、模型/资源及交互逐类补测。
- 未安装的第三方插件、特殊编辑器、第三方工具和异步worker继续单列；不会因33类结构通过变成已兼容。
- 核心三生态显示完成后，还须当前主程序实际GUI、同数据原生RViz对照、配置/项目保存恢复、
  全工作流、多视角/画布/输出、稳定性性能、安装包依赖完整及安全升级全部通过，才可正式发布。
- 本文件与总矩阵仅在取得真实证据后更新状态；当前缺核心消息不豁免，也不以截止日期自动发布。
