# 原生 RViz 兼容性与实测矩阵

本矩阵记录当前机器实际安装的 ROS 1 Noetic、RViz **1.14.26**、Qt5/librviz/OGRE
宿主与 Windows Qt6 客户端。结构基线范围是该 ABI 下实际发现的33个Display（32个默认插件及
`rviz_plugin_tutorials/Imu`），不代表 ROS2、其他 RViz 版本或未安装第三方插件已经兼容。
Qt6 与 Qt5 进程以 JSON 通信，不跨 ABI 共享 QObject 或 OGRE 对象。Windows独立主程序
不依赖Python；可选ROS环境自身工具（如roscore）的依赖另计。下述Python文件仅是开发验收工具。

2026-11-28 本次v1真实渲染门禁优先 FAST-LIO/EGO-Planner/SUPER 常用Display，具体官方配置、
核心清单和缺少的真实消息见 [生态范围调整](V1_RVIZ_ECOSYSTEM_SCOPE_CN.md)。下表仍完整保留
33类结构与实数据分级；生态以外插件“结构通过、真实渲染未验证”可转后续独立PR，不冒充全支持。
此收敛不放宽核心Display、多视角隔离、画布/导出、安装包依赖完整及安全升级。
按同日最新用户调整，干净Windows安装/运行由用户正式发布后手测，不再作为v1前置门槛；
移交不等于本机已通过，真实后端/插件能力和未测状态继续如实记录。

下方33类结构表与7张真实录包输出已使用历史冻结ELF `8C87BB91B7…` 独立复验，报告为
`captures/rviz-plugins-8c87-20261128/report.json`，33类共读取547个原生属性节点。
该版本另修复原生相机属性/类型编辑使共享Bag失去同步的问题，以及越界FloatProperty的拒绝；
20项相机编辑/类型切换和9张原生输出的精确范围见 `NATIVE_CAMERA_PROPERTY_SYNC_CN.md`。
旧5DA1证据仍保留其原始版本归属，空机位配置与客户端删除恢复专项没有冒称已在8C87全部重跑。
历史 `rviz-plugins-ac3e-20261127` 对应 `AC3E76EAAA…`；`rviz-plugins-current` 目录及117响应
客户端日志仍对应 `AFE1767617…`，不改写其版本归属。F974版本的33次YAML往返失败记录也保留，
见下方回归修复。405项控制回归不是33类全覆盖。完整SHA256及各证据版本见文末。

## Windows 独立后端的13类子集

`ReplayController::independentDisplayTypes()` 明确提供 Grid、Axes、Group、PointCloud2、
LaserScan、Path、Marker、MarkerArray、Odometry、Pose、PointStamped、PoseWithCovariance、TF。
这是独立C++/Qt解码、绘制及有限属性实现，使用相应Class名称帮助配置兼容；没有运行librviz
插件、原生Property或原生编辑器。不从下面的原生33类结构测试推定这13类的全部独立样式/
数据/GUI通过，也不将13类当成全部默认插件。Camera/Image/RobotModel等缺失能力保持明确。
详细平台矩阵见 `WINDOWS_BACKEND_COMPATIBILITY_CN.md`。

## 验收层次与证据

- `native/rviz_host/verify_plugins.py` 查询实际 pluginlib factory，逐个创建原生实例、读取
  Property 树及实际编辑器选项，修改最多三个可用枚举，save→load_yaml→save 比较真实配置，
  最后删除实例并比较 Display、Group、Camera、Scene Node、Movable Object 计数。
- 当前 `captures/rviz-plugins-8c87-20261128/report.json`：33/33 创建、真实属性、YAML 往返、删除及上述
  资源计数归基线通过，错误列表均为空；547个原生属性节点，23/23 次枚举写入及实际值回读通过，
  其中17次实际条件属性结构/可见性变化。`plugin_contract_passed:true`，进程退出0且自有会话正常关闭。
  属性数量包含 Display 根节点及当时原生状态节点，会随数据、URDF 和插件状态变化。
- 同一脚本的真实录包专项只为实际匹配的 Topic 启用单个 Display，按消息 Header 定位，
  排队完成及 TF 检查通过后才请求原生 RenderTexture。7 张 1920×1080 PNG 已实际打开检查，
  见下方逐项结果。最终逐类属性数量、23次枚举写入及17次条件变化与历史结构基线一致。
  **构造/属性/配置成功不等于真实传感器渲染通过**。
- `captures/rviz-lifecycle-current/report.json`：405 项检查通过（含协议响应计数，不是405种
  独立功能）。明确生成的控制夹具覆盖多实例、Group 批删、UUID、撤销重做、100次增删及源移除。
- 历史 `build-native/native-rviz-client-lifecycle.log`：Qt6客户端真实录包运行退出0，117个成功
  响应，覆盖实际属性、配置保存、随机定位、离屏预览、批删/撤销/重做、源关闭和进程清理。
  它不替代主窗口播放头同步或鼠标交互验收。

`captures/` 内真实实验图及源素材仅保留本机，不进入 Git。没有相应真实数据的行保持“未验收”。
一些原生插件在 Topic 为空时仍给出 `Status: Ok / Topic: OK`，本软件不能据此宣称已经接收数据。

## 全部实际安装 Display

“创建/属性/往返/删除/资源”分别对应真实实例、真实 Property、YAML 结构往返、实例消失、
上述 OGRE/Display 资源数量归基线；表内 `通过` 指这五项均通过。
以下结构列均为 `8C87BB91B7…` 的独立逐类结果，不从405项控制回归推定。
四类执行位姿派生数据的新增实渲染仅属于文末 `D5AB2C57E7…` 专项，不改写原录包来源。
“枚举/条件”是本次接受的枚举编辑次数和实际条件属性变化次数，0 不代表不存在动态属性。
默认包均为 `rviz`；唯一第三方包为 `rviz_plugin_tutorials`。

| Class | 包 | 五项结构检查 | 属性数 | 枚举/条件 | 真实数据或渲染结果 | 必要数据/依赖及限制 |
| --- | --- | --- | ---: | ---: | --- | --- |
| `rviz/AccelStamped` | 默认 | 通过 | 14 | 0/0 | 未验收，真实录包无对应数据 | `geometry_msgs/AccelStamped`、消息 Frame 到 Fixed Frame 的 TF |
| `rviz/Axes` | 默认 | 通过 | 6 | 0/0 | 已见真实坐标轴输出 | 有效 Reference Frame；不属于传感器消息接收测试 |
| `rviz/Camera` | 默认 | 通过 | 9 | 1/0 | 未验收，真实录包无相机图像 | Image + CameraInfo + TF、实际 image_transport；附属窗口删除已覆盖 |
| `rviz/DepthCloud` | 默认 | 通过 | 33 | 1/1 | 未验收，真实录包无深度数据 | 深度 Image + CameraInfo，可选彩色 Image、TF 与正确编码 |
| `rviz/Effort` | 默认 | 通过 | 13 | 0/0 | 未验收，原生报缺 `robot_description` | JointState、URDF、链接资源及 TF |
| `rviz/FluidPressure` | 默认 | 通过 | 28 | 1/1 | 未验收，真实录包无对应数据 | `sensor_msgs/FluidPressure`、TF |
| `rviz/Grid` | 默认 | 通过 | 14 | 2/1 | 已见实际网格输出 | 有效 Reference Frame；不属于传感器消息接收测试 |
| `rviz/GridCells` | 默认 | 通过 | 8 | 0/0 | 未验收，真实录包无对应数据 | `nav_msgs/GridCells`、TF |
| `rviz/Group` | 默认 | 通过 | 1 | 0/0 | 不适用独立几何；分组生命周期已验 | 保存层级和子实例；不能用空 Group 代替子插件渲染验收 |
| `rviz/Illuminance` | 默认 | 通过 | 28 | 1/1 | 未验收，真实录包无对应数据 | `sensor_msgs/Illuminance`、TF |
| `rviz/Image` | 默认 | 通过 | 11 | 0/0 | 未验收，真实录包无对应图像 | Image 及实际 image_transport，图像编码/附属面板需实测 |
| `rviz/InteractiveMarkers` | 默认 | 通过 | 8 | 0/0 | 未验收，原生等待消息 | InteractiveMarkerUpdate/初始状态、服务器服务及交互反馈；离线完整性未认证 |
| `rviz/LaserScan` | 默认 | 通过 | 28 | 1/1 | 未验收，真实录包无对应数据 | `sensor_msgs/LaserScan`、TF |
| `rviz/Map` | 默认 | 通过 | 19 | 1/0 | 未验收，真实录包无对应地图 | OccupancyGrid、可选地图更新、TF |
| `rviz/Marker` | 默认 | 通过 | 6 | 0/0 | 原录包无对应数据；D5AB派生执行线专项已验 | 明确派生Marker、TF；原生规划消息及mesh/纹理外部URI另验 |
| `rviz/MarkerArray` | 默认 | 通过 | 6 | 0/0 | D5AB派生执行线与位姿箭头已验；控制夹具覆盖多实例 | 原生规划消息、外部资源与消息DELETE语义另验 |
| `rviz/Odometry` | 默认 | 通过 | 30 | 3/2 | 两个真实 Topic 已接收并见箭头 | `/Odometry`、`/Odometry_imu`，Fixed Frame=`camera_init` |
| `rviz/Path` | 默认 | 通过 | 23 | 2/2 | 用户配置 `/path` 仍无原录包源；D5AB明确派生执行Path已验 | 保留实际Odometry Header及位置；不能冒充原生规划Path |
| `rviz/PointCloud` | 默认 | 通过 | 28 | 1/1 | 未验收，录包为 PointCloud2 而非此类型 | `sensor_msgs/PointCloud`、TF、实际字段 |
| `rviz/PointCloud2` | 默认 | 通过 | 28 | 1/1 | 两个真实 Topic 已接收并见彩色点云 | `/cloud_registered`、`/cloud_registered_body`，实际 Header/TF |
| `rviz/PointStamped` | 默认 | 通过 | 10 | 0/0 | 未验收，真实录包无对应数据 | `geometry_msgs/PointStamped`、TF |
| `rviz/Polygon` | 默认 | 通过 | 8 | 0/0 | 未验收，真实录包无对应数据 | `geometry_msgs/PolygonStamped`、TF |
| `rviz/Pose` | 默认 | 通过 | 15 | 1/1 | 原录包无对应Topic；D5AB派生实际位姿箭头已验 | 明确派生PoseStamped、TF；原生规划目标Pose另验 |
| `rviz/PoseArray` | 默认 | 通过 | 16 | 1/1 | 未验收，真实录包无对应数据 | `geometry_msgs/PoseArray`、TF |
| `rviz/PoseWithCovariance` | 默认 | 通过 | 27 | 3/2 | 未验收，真实录包无对应数据 | `geometry_msgs/PoseWithCovarianceStamped`、TF及协方差 |
| `rviz/Range` | 默认 | 通过 | 9 | 0/0 | 未验收，真实录包无对应数据 | `sensor_msgs/Range`、TF |
| `rviz/RelativeHumidity` | 默认 | 通过 | 28 | 1/1 | 未验收，真实录包无对应数据 | `sensor_msgs/RelativeHumidity`、TF |
| `rviz/RobotModel` | 默认 | 通过 | 15 | 1/0 | 未验收，原生报缺 `robot_description` | URDF、mesh/材质资源、各 Link 的 TF；缺URDF时动态 Link 树不能认证 |
| `rviz/TF` | 默认 | 通过 | 13 | 0/0 | 真实 `/tf` 已接收并见小型帧标记 | `camera_init`→`body`，未补造缺失的 `livox_frame` |
| `rviz/Temperature` | 默认 | 通过 | 28 | 1/1 | 未验收，真实录包无对应数据 | `sensor_msgs/Temperature`、TF |
| `rviz/TwistStamped` | 默认 | 通过 | 14 | 0/0 | 未验收，真实录包无对应数据 | `geometry_msgs/TwistStamped`、TF |
| `rviz/WrenchStamped` | 默认 | 通过 | 14 | 0/0 | 未验收，真实录包无对应数据 | `geometry_msgs/WrenchStamped`、TF |
| `rviz_plugin_tutorials/Imu` | 第三方 | 通过 | 9 | 0/0 | **失败：1594条消息已投递，缺TF，阻止渲染输出** | `/livox/imu` 的 `livox_frame` 不存在；保留原生错误，不伪造静态变换 |

## 真实录包输出的逐项核查

所有下列输出来自同一只读录包，消息 Header 时间基准，目标 **1768571820557415724 ns**；
选择 `camera_init` 为 Fixed/Target Frame，使用相同 Orbit 参数并逐个隔离启用。
图像本次已实际打开检查；消息投递、TF、队列和目标时间证据保存在 JSON 的各 case 中。
不据此认证全时间区间、所有样式、遮挡选择、交互或第三方异步 worker 已完成。

| 输出（位于 `captures/rviz-plugins-8c87-20261128/`） | 实际观察 | 结论 |
| --- | --- | --- |
| `rviz-PointCloud2---cloud_registered-real.png` | 深灰背景中可见彩色点云几何，非空白/窗口截图 | 原生点云输出通过；默认点尺寸较细，不代表美化验收 |
| `rviz-PointCloud2---cloud_registered_body-real.png` | 可见彩色点云几何，TF就绪 | 原生点云输出通过 |
| `rviz-Odometry---Odometry-real.png` | 可见红色方向箭头 | 原生里程计输出通过；此时段位移很小，未声称长轨迹质量 |
| `rviz-Odometry---Odometry_imu-real.png` | 可见红色方向箭头 | 原生里程计输出通过 |
| `rviz-TF-real.png` | 画面中央可见较小的彩色 Frame 标记 | 当前真实TF输出通过；默认尺度较小 |
| `rviz-Axes-real.png` | 可见红绿蓝坐标轴 | Axes 输出通过 |
| `rviz-Grid-real.png` | 可见完整透视网格 | Grid 输出通过 |

录包另含 `livox_ros_driver2/CustomMsg`（`/livox/lidar`）；当前 factory 未声明兼容的 Display。
Topic 浏览须保留类型和来源并提示缺少渲染插件，不能过滤、强制映射为 PointCloud2 或声称默认支持。

脚本保留 `actual_sensor_rendering_verified:false` 的人工检查门禁，未自动提升为全插件认证；
七张图的实际内容观察、PNG尺寸与哈希另存同目录 `inspection.json`。该文件只记录已打开
检查的七张原生输出，不证明全部插件、全部时段或标准RViz/主应用GUI均通过。
七图仅覆盖Odometry、PointCloud2、TF、Axes、Grid五类；其 `target_ns`、`clock_ns` 均精确相等，
但该元数据核验不等于第三方插件异步worker完成认证。IMU没有生成图片，不能计入成功输出。

## 空附加机位配置的回归修复（2026-11-28）

F974宿主的完整探针 `captures/rviz-plugins-f974-20261128/report.json` 曾出现33/33创建与属性通过、
33/33 YAML往返失败：程序自身写出的 `UAV Studio / Render Views: []` 被RViz Config读取为
未保留集合类型的空节点。旧探针退出0且 `completed:true` 只表示流程结束，不表示检查通过；
该失败报告保留，不把它改写为最终版本通过。

定向红测 `captures/rviz-empty-views-red-f974-20261128/empty-view-configuration.json` 为10/13通过，
失败项是程序自写空机位快照、显式空列表和YAML null。历史5DA1修复宿主先用yaml-cpp校验原始节点，
仅对已验证的空sequence/null规范化RViz Config；没有把所有空节点放宽接受。
`captures/rviz-empty-views-normalized-20261128/empty-view-configuration.json` 已13/13通过：
缺省、空列表及null可恢复；空字符串、伪装列表/null的字符串、其他标量、空mapping及非法列表成员
继续拒绝。非法输入前先建立真实Orbit/TopDown机位，拒绝后场景、机位及资源计数保持不变。
当前探针对结构检查失败返回非零；`--view-config-only` 不运行33类检查，其
`plugin_contract_passed:null` 不得解释为全插件通过。

## Qt6客户端机位删除与参数恢复（2026-11-28）

`captures/native-view-lifecycle-20261128/report.json` 与同目录 `run.log` 记录真实Qt6客户端、
冻结宿主及只读Bag专项。实际TopDownOrtho的Scale=19.25、Angle=0.375、X=2.5、Y=-4.75、
Target Frame=`body`，连同未知嵌套Controller YAML，在两次删除/恢复循环后完整保持。
Display/Scene/source身份、索引结果、同步就绪及精确目标/时钟 `1768571820557415724 ns` 不变；
恢复期间 `bag.open`、`bag.index`、`bag.seek`、`bag.advance` 请求增量为0。
最终删除两个附加机位后Camera计数7→3、Scene Node164→160，Display13及Group2保持；
这是本次资源计数核查，不是显存或长期泄漏认证。

客户端只保留有界文本归档：最多4096个ID、64MiB UTF-16 YAML；超预算保留class tombstone，
恢复不完整快照时明确拒绝，不能静默改用默认Orbit。新项目清理归档，同项目重连保留已确认配置。
`build-native/native-view-archive-20261128.log` 的helper专项覆盖完整YAML、替换/FIFO、内存边界、
tombstone、非法输入及清理。上述是客户端/后端集成，`main_application_gui_verified:false`；
不替代真实主窗口删除/撤销/重做、主题切换、保存重开或完整用户工作流验收。

## 原生配置与标准 RViz 对照

`.rviz` 的未知键、缺失插件 `FailedDisplay` payload、Group、多同类实例及首次原文保留已有
专项回归；重复配置以真实插件 Class 和独立实例 UUID 恢复。未安装插件仍是失败状态。
用户原配置 `Target Frame=global` 实际缺失且 `/path` 无源，原文件保持只读。
`captures/rviz-readiness-current/native-core-ready.rviz` 是明确命名的验收副本，仅将 Target
改为实际 `camera_init` 并禁用无源 Path，不能把副本的就绪结果算成原配置无修改通过。

`native/rviz_host/verify_vanilla.py` 启动未修改的标准 `rviz -d`，与宿主使用同一验收副本、
同一真实录包、同一 master 和精确目标；配置 SHA256 前后不变。标准 RViz 的 EOL 模态阻塞
初轮观察后，仅为本开发对照进程设置其提示自身提供的 `DISABLE_ROS1_EOL_WARNINGS=1`，
不修改用户系统或生产配置。默认对照期限缩短至5分钟，观察完成以 stop 标记结束自有进程。

`captures/rviz-vanilla-no-modal/standard-rviz-observed.png` 是实际 computer-use 抓取并原样
保存的标准 RViz GUI 截图。实际看到 Global Status: Ok、Fixed Frame=`camera_init`、mapping
分组内 surround/currPoints 两个独立 PointCloud2，均订阅 `/cloud_registered`，分别保留
Size3/1、Alpha1/0.1、Decay0/1000，以及 XYZ/Intensity 等动态原生属性。真实彩色点云可见，
与同目录 `native-same-config.png` 的几何和视角目检吻合。ROS Time 显示1768571820.56s，
协议目标精确到1768571820557415724ns，GUI的小数截显不能代替协议精度认证。

报告 `vanilla_gui_observed=true` 只覆盖这份配置和当前录包的上述画面/属性目检；
`pixel_equivalence_certified=false`、所有 Display/Tool GUI 全覆盖仍为未通过。未逐个目检
Odom/Camera/RobotModel等依赖资源的显示，也未据此认证全时段或全部交互。截图只保留本机，
不提交真实实验几何。主应用四工作区 GUI 仍需独立验收。

## 视角、工具及尚未验收范围

实际 factory 发现6个 ViewController：FPS、FrameAligned、Orbit、ThirdPersonFollower、
TopDownOrtho、XYOrbit；9个 Tool：FocusCamera、Interact、Measure、MoveCamera、PublishPoint、
Select、SetGoal、SetInitialPose、第三方 PlantFlag。发现清单不是逐工具交互验收；主应用尚未
覆盖其全部选择/测量/发布/交互反馈流程。当前指针经实际ToolManager当前Tool的
`processMouseEvent`转发，MoveCamera委托原生ViewController；Finished时遵循默认工具回退。
Qt6提供真实tools/viewControllers目录、activeToolClass和激活API，非硬编码样例属性。

主应用 `ReplayCanvas` 的主视角及附加原生机位共用同一 `NativeRvizClient`、Scene、Display
和 Replay，各机位使用独立的真实 ViewController/RenderPanel。Main Camera 按真实
`ViewControllerContainer` 根过滤属性模型。`MovieExporter` 按实验精确ns及各机位在最终画布的
像素尺寸请求原生 RenderTexture，原子准备同时间各视角后进入共享画布合成，严格核验
view ID、时间、TF/场景就绪、scene revision和尺寸。r5 主应用自动工作流的实测范围见下节；
早期宿主/客户端专项不能代替主应用或手动GUI验收。详见 `NATIVE_RVIZ_BACKEND_CN.md`。

原生导出遇到仍可见的旧 `CloudView` 机位继续拒绝，不以独立渲染结果替代原生结果。
主合成静图限单边8192/40MP，视频限3840×2160；本轮已实测下列布局的完整画布8K静图，
不据此认证任意布局、GPU边界或全部格式。
包含原生视角时暂不支持透明或覆盖场景背景；完整合成 PDF/SVG 明确嵌入位图，不能冒充纯矢量。

以下仍待完成：其他 Display/Tool 的标准 RViz GUI 全覆盖、主应用四工作区完整交互、每视角独立 Display 可见性、
多独立数据源同场景隔离。更多第三方完整渲染、ROS2及其他ABI按Roadmap保留；原生默认插件、
核心RViz操作、离线配置及同步导出仍为v1核心门禁，不能以扩展延期豁免。原生输出当前
opaque PNG，透明/分块尚未实现，不冒充矢量；GPU限制及实际MSAA如实返回。已测原生严格
历史推进约174–191ms/33ms实验步，预览上限4Hz，未达到30fps实时验收，不提前发布稳定版。

## 原生三相机主应用自动工作流（r5）

`captures/native-multiview-composition-20261127-r5/native-scene-acceptance.json` 记录真实Bag、
主应用实际画布、三个实际 `nativeSceneImage`/`exportViewId` Image 和 OGRE RenderTexture。
两次真实 `views.sync` 确认后，主视角、Orbit附加机位与切换为TopDownOrtho的第二附加机位
参与合成。没有用legacy视角、低清预览或窗口截图替代离屏源。该报告为程序操作真实GUI的
自动工作流，不是鼠标/键盘手动验收，也不重新认证上面的33类插件结构或全部真实数据。

| 案例 | 输出/拒绝结果 | 原生相机帧 | 合成帧回调 |
| --- | --- | ---: | ---: |
| `selected-1080.png` | 成功，选中TopDownOrtho机位1920×1080 | 1 | 1 |
| `canvas-1080.png` | 成功，完整画布1920×1080 | 3 | 1 |
| `canvas-4k.png` | 成功，完整画布3840×2160 | 3 | 1 |
| `canvas-8k.png` | 成功，完整画布7680×4320 | 3 | 1 |
| `layers-1080.png` | 成功，保留主视角/Orbit并排除TopDownOrtho | 2 | 1 |
| `native-two-frames.mp4` | 成功，两个实验时刻、每时刻三个机位 | 6 | 2 |
| `cancelled.png` | 正确取消，无最终文件或合成帧发布 | 1 | 0 |
| `missing-target.png` | 正确拒绝缺TF，无最终文件或合成帧发布 | 1 | 0 |
| `transparent-rejected.png` | 正确拒绝不支持的原生alpha，无最终文件或合成帧发布 | 0 | 0 |
| `restored.png` | 成功，随机切换及失败后恢复完整画布 | 3 | 1 |

10个案例均 `passed/restored=true`，其中7个成功输出、3个正确失败/拒绝。成功案例实际消费
21个原生相机帧；23个总原生帧另外包含取消前1帧及缺TF批次已准备的另一有效机位1帧。
这些计数不是23个成功文件或23个成功合成帧。各案播放头、播放状态、捕获选项实值和源/编辑
状态恢复，`loading_signals=0`；原生临时Image源清空，没有重载录包。

| 完整画布 | 主视角实际离屏源 | Orbit实际离屏源 | TopDownOrtho实际离屏源 |
| --- | --- | --- | --- |
| 1920×1080 | 1920×1080 | 557×314 | 596×335 |
| 3840×2160 | 3840×2160 | 1114×627 | 1191×670 |
| 7680×4320 | 7680×4320 | 2228×1253 | 2381×1340 |

主视角在该布局中占全画布，小窗口按其最终像素面积渲染；三个机位并非各自全8K。逐帧核对
实际Image URL、view ID、精确 `target_ns/clock_ns` 字符串、scene revision、尺寸、GPU限制、
`native_render_texture=true` 及真实TF/场景就绪。静图目标8.025s对应
`1768571820582415724 ns`，MP4第二帧8.125s对应 `1768571820682415724 ns`。
本次原生源均不透明，背景 `#ff000000`，不能计作透明通过。

完整1080P静图与随机切换/拒绝后的恢复图像素hash相同，MAE/max均为0；同实验时刻静图与
MP4编码前第一帧MAE/max也均为0。实际MP4解码2帧MAE约0.603–0.609/255、PSNR约34.05–34.17dB，
不能称有损视频逐像素一致。文本/速度表保留、单视角排除文本及选层像素变化均通过程序检查。
各案UI heartbeat持续触发，只证明事件循环仍响应，不证明P95/P99、30fps播放或长片性能。

同轮完整CTest为29/29通过、0失败、47.50s，日志
`build-native/current-full-ctest-20261127.log`，SHA256
`C8F6BDA054D2903AFFE0C2B9D4BD004E41DD28B6452620B35BAB157CE9340A59`。
它覆盖组件、Qt Quick及smoke，不是29项手动GUI或全原生传感器插件验收；上述r5工作流
另有独立报告，不计入29个CTest。r5报告SHA256为
`5806DCB857347C41F0113C91EBC7AE58B24B5D25A67629ED908A1414A112EA0B`。

手动GUI仍有独立门槛：此前小数时间跳转后曾观察到画布未同步提示残留；随后7AE9…构建
实际手动暂停在8.069872064509182s，Inspector与中央画布经短暂pending后均匹配，截图与录屏
保存在 `captures/manual-native-current-7AE9/`。这次未复现不能视为已经找到旧问题根因，也不
认证全部随机跳转。r5自动输出不能替代这些手动门槛。每视角独立Display显隐/材质、
多源隔离、原生alpha/背景覆盖/分块及第三方异步完成也仍未认证或未支持。

## 历史AC3E自动生命周期与空场景增量证据

AC3E宿主的 `captures/rviz-autostart-control/report.json` 实测405项通过，
`build-native/rviz-tool-control.log`同样退出0。来源是明确生成的多Display控制夹具；405包含
协议成功响应，不代表405种功能。报告仍明确主应用GUI、标准RViz GUI、每视角独立可见性及
多源同场景隔离未认证。该控制夹具的可用类只有Grid、Group、MarkerArray、Odometry、Path、
PointCloud2、TF共7类；共2个Group、12个适用Display实例，另含1个缺插件占位。
它没有逐一创建其余26类，不能作为33类结构/属性/配置往返的证据。当前33类通过来自上述
独立 `verify_plugins.py` 复验；历史5DA1另有上述报告，均不代表全部33类有真实数据、渲染或GUI验收。

`build-native/native-runtime-automatic.out` 实测Qt6 `--auto-runtime`退出0，stderr为空：
Standalone manifest在任何WSL发现前阻断增强；缺依赖保留独立核心；实际WSL发现、旧发行版/
启动器回退、显式初始配置、连接前YAML排队及离线清空通过。默认无Bag真实Grid/Axes可预览，
未标为实验同步；真实MoveCamera/Select/FocusCamera/Measure发现与激活、MoveCamera wheel响应
通过，完整空间选择与测量GUI尚未验收。受控重连及编辑确认后立即结束自有WSL PID均恢复
最后确认的属性/UUID，清除旧像素/索引/时钟，显式Stop不自动重启；只清理自有会话。
此测试在Windows Qt6与实际Noetic宿主完成；本机缺Linux Qt6库，没有Linux Qt6客户端构建认证。

`build-native/native-rviz-manual-topic.out` 的 `--manual-topic` 专项实际退出0，stderr为空。
无Bag、尚未发现的手动Topic可设置到真实PointCloud2 RosTopicProperty；原生YAML保存/恢复及
UUID保持通过。状态明确等待源/消息类型兼容性未验证；已发现的不兼容类型继续拒绝。
没有唯一实际Topic属性时撤销新增Display并真实删除，未把配置成功称为消息渲染通过。

## 构建与证据版本

| 版本 | 宿主ELF SHA256 | 对应实测证据 |
| --- | --- | --- |
| 历史真实数据及插件结构基线 | `AFE176761709A1BF64F94ECCCDBFF04BA393BD1ED5A14EA26E8B55E2EA50CE7D` | 33类结构/资源，7张真实输出，117响应客户端，标准RViz核心GUI目检 |
| 自动运行/工具及插件逐类复验宿主 | `AC3E76EAAAF9CDECFBBB7F306A7E27E6FF5BA5DA092C2F00895F035D9D426193` | 独立33类结构/资源复验、7张真实输出；另有405项控制回归、自动生命周期/空场景工具增量、手动无源Topic专项 |
| r5原生三相机合成宿主 | `ECF8654E8A08C28C5D59C600CB613A4A635E93B69474DACC56E391DCCD0F0B0B` | 上述主应用自动10案例，21个成功消费原生相机帧、23个总帧；不从早期33类报告借用覆盖 |
| 空附加机位配置回归失败宿主 | `F9743C368C58480367EA152A2CE48A63C4432C39E74C6B7AAA7D53528411C378` | 33次YAML往返失败；定向13项中3项失败；仅保留为失败证据 |
| 历史插件/空机位/客户端恢复宿主 | `5DA199D7027396797395283C3CD6B1ABDE21F90A3BCC3D80378FD598006C70C4` | 独立33类/547属性/23枚举/17条件变化；五类七图；空机位13/13；TopDown参数/未知YAML两次删除恢复 |
| 相机属性/类型同步修复基线宿主 | `8C87BB91B70B715D16511B5FDF91CBF250313E6F4DE506D9995DB426F5BC1A29` | 独立33类/547属性/23枚举/17条件变化；五类七图；20项相机编辑/类型切换零Bag重放及越界拒绝，详见相机专项 |
| Marker前导常量时间索引修复宿主 | `D5AB2C57E7ABF05056930470571EDCFA4CFCDA51CDEE28A6E79848355C9B4BBC` | 16项Header布局回归；四类真实Odometry派生消息46项定向验收、4张1080P原生图；不借用8C87全部结构/GUI认证 |

历史AC3E宿主当时使用 `build-native/rviz-host/uav-studio-rviz-host`；r5及历史5DA1由
`build-native/multiview-launcher.sh` 明确启动
`build-native/rviz-host-multiview/uav-studio-rviz-host`，两者不可混用证据归属。
打包复制后须核对实际ELF与动态库，
最终主程序仍需完整GUI、保存恢复和输出工作流验收；本矩阵不授权发布稳定版。

历史AC3E逐类复验于2026-11-27本地完成（UTC 2026-11-28），命令为当时 `verify_plugins.py`
加 `--launcher native/rviz_host/run_host.sh --port 49343 --bag <只读真实Bag>`，输出目录为
`captures/rviz-plugins-ac3e-20261127/`。`provenance.json` 记录运行前后相同ELF和Bag哈希、
脚本与启动器哈希、时间及退出0；`owned_shutdown:true`，不留用自有宿主/master。
未修改开发测试脚本或生产宿主。报告SHA256为
`02DCCE86B4FA116A08AEB3F3EA8EAC5EF549AEB77735E67349DE296D744956FA`，
测试脚本SHA256为 `092EA9D75D7C539927D6CE35C82526691CA3336B654EF42D4E446AE8E91FF85B`。
日志为 `build-native/rviz-plugins-ac3e-acceptance.log`，stderr为空。所有真实输出仅留本机。

历史5DA1逐类复验于2026-11-28完成，使用 `verify_plugins.py --launcher
build-native/multiview-launcher.sh --port 49353 --bag <只读真实Bag>`，输出目录为
`captures/rviz-plugins-5da1-20261128/`；日志 `build-native/rviz-plugins-5da1-acceptance.log`，
stderr为空，退出0。报告SHA256为
`DB7F8AEF7F8DD7BFCE7A635DD8D5551BD433393FD22E5DF752F8314312D4BD0A`，
inspection SHA256为 `CF1C4F3844890010EDAB5466B0D86D5196BCB16AFA6F7B9B12AD490D1D932078`。
`provenance.json` 核对宿主、相关源码、启动器及只读Bag前后哈希均未变，自有会话正常关闭；
其图片哈希映射仅作过证据字段整理，沿用inspection已有七图哈希并逐文件重新核对，未重跑或
改写历史测试。该版本没有借用旧版标准RViz目检作为新认证：完整主GUI、全部真实数据、
标准RViz对照、每视角独立显隐/材质、原生Alpha/背景覆盖/分块及性能验收仍分别保留。

当前8C87逐类复验于2026-11-28完成，使用 `verify_plugins.py --launcher
build-native/property-sync-launcher.sh --port 49414 --bag <只读真实Bag>`，输出目录
`captures/rviz-plugins-8c87-20261128/`；退出0，自有会话正常关闭。报告SHA256为
`2DA385F6CA1B7A06AB985511B6A6D00A15A57DF90D0DE19BFE8C1AD7ACD3170C`。
运行前记录的宿主、相关源码及启动器哈希在运行后逐项保持，七张PNG已全部打开检查，
`inspection.json` 保留各图哈希、尺寸、观察与精确时钟/目标。该次未重新读取标准RViz GUI，
也没有将此Host专项标作主应用实际GUI已通过。

## 2026-11-28 Marker时间索引修复及四类派生数据实渲染

8C87在实际派生Bag的 `Header/reject` 索引中拒绝了标准 `visualization_msgs/Marker`。
原因为标准Marker定义在Header前声明常量，而旧检测仅看第一条非注释定义。修复只跳过
合法ROS1基础类型常量；遇到第一个实际字段立即判定，不跨实际字段、数组或嵌套MSG定义
寻找Header，也不改用record时间。`native-header-layout` 实际CTest通过16项，包含已安装
Marker/Path/PoseStamped/Image/Header定义及无Header、嵌套Header、前置真实字段等拒绝边界。

新宿主为 `build-native/rviz-host-header-constants/uav-studio-rviz-host`，完整SHA见上表。
本地 `captures/rviz-derived-compatibility-20261128/host-render-d5ab-r2/report.json`
记录46/46项通过、退出0；报告SHA256为
`9BAAD3B68D1120BCA53FA060F95DDFCD9D9BF08A71DBDCBD2EAEACFDB6994966`。
原始8C87失败及首轮脚本比较/资源基线问题均保留，没有改写成通过。

来源是只读实际 `/Odometry` 的明确派生标准消息：PoseStamped复制原Header/pose，Path累计
执行位置，Marker为执行LINE_STRIP，MarkerArray为同一线和最新实际位姿箭头。新Bag保留全部
17,726条原消息的序列化内容、record时间及connection header；四个新增Topic各393条。
Host、独立 `rosbag info` 与ROS Bag API核对全部12 Topic/类型/消息数一致。
这不是原生EGO/SUPER规划录制，不产生规划目标、计划路径、走廊或科研指标。

四类均创建/复制两个真实插件实例，验证独立属性和Topic，删除其中一个后保留兄弟实例，
保存/重开删除状态不复活已删实例；完整 `.rviz` 恢复8个原UUID及全部原生属性。
随后逐类单独启用，在精确Header目标 `1768571842657399654 ns` 下恢复历史及TF：
Path/Pose/Marker各实际发布并观察301条，MarkerArray拆分子Marker发布/观察602条，均无record
映射。四张PNG文件实际尺寸均为1920×1080，使用4×FSAA原生RenderTexture，时钟等于目标。
四张原图逐一打开，见实际执行轨迹/位姿箭头；`inspection.json` SHA256为
`4C80755C57B5EC9DAE044B1B1E3F9B0CD959505151AB4978ED65866579E766C3`。

删除全部8个实例并关闭Bag后，Display/Group/Camera/SceneNode、全部Ogre movable计数及
Display来源追踪回到预热基线；自有token/master正常退出且专用49476端口释放。宿主、相关
源码、启动器、原Bag及派生Bag前后SHA均不变。进程内存数值不等于GPU显存实测，未认证性能。
此专项仍明确主应用GUI、标准RViz GUI、每视角独立显隐及完整生态传感器兼容未验证；
原录包缺少原生规划数据的限制保留，不据此宣布全部v1门禁完成。
