# Windows 独立运行与可选原生 RViz 能力矩阵

本表按 2026-11-27 工作树记录实现、依赖及证据边界。默认 Windows 安装包是独立的
C++ / Qt6 程序，随包部署 Qt、OpenCV、FFmpeg 和 VC 运行库。独立录包读取、点云、
视频、编辑及制图不要求用户安装 ROS、WSL 或 Python。此架构目标、静态依赖检查与
“已在完全没有这些组件的干净 Windows 上完成全部工作流”是不同验收项，不能互相替代。

2026-11-28 最新用户安排：干净Windows安装/运行移交用户在正式发布后手测，不再作为v1
发行前置门槛，不在README单列，也不再寻找或准备VM/新环境。该移交不改变已有测试事实，
不表示本机已经验证通过。安装包依赖完整、当前最终包本机实际运行和安全覆盖升级仍须完成。

可选 Linux 原生 RViz 宿主提供实际 librviz / pluginlib / OGRE 能力，拥有独立运行环境。
它不属于 Windows 原生插件实现，也不能成为独立功能启动、导入数据或导出的前置条件。
完整默认插件的逐类证据见 [RVIZ_COMPATIBILITY_MATRIX_CN.md](RVIZ_COMPATIBILITY_MATRIX_CN.md)。

## 两种发行载荷

| 项目 | 默认 `Standalone` | 显式 `OptionalNativeRviz` |
| --- | --- | --- |
| 打包入口 | `package.ps1` 不提供 `-NativeRvizHostBinary` | 明确提供该参数和真实 Linux x86_64 ELF |
| 自动搜寻本机构建 ELF | 禁止；本机已有 host 也不会自动加入包 | 不搜索替代文件，只使用明确路径 |
| `rviz_host` 文件夹 | 不包含；staging 残留时拒绝，要求使用干净 staging | 包含 ELF、启动封装、许可证、对应 C++ 重建源码及说明 |
| `native-rviz-backend.json` | `package_profile: Standalone`、`included: false` | `package_profile: OptionalNativeRviz`、`included: true`、`optional: true` |
| `windows-runtime.json` | `profile: Standalone` | `profile: OptionalNativeRviz` |
| NSIS 独立调用默认值 | `INCLUDE_NATIVE_RVIZ_HOST=0`，即使输入目录残留该文件夹也不复制 | 必须显式 `INCLUDE_NATIVE_RVIZ_HOST=1` |
| Windows 安装过程 | 只部署载荷、快捷方式及文件关联；不探测/安装/启动 ROS、WSL 或 Python | 同样不安装、配置或启动外部 ROS/WSL 环境 |
| 应用自动连接 | 相邻 manifest 的独立模式须在自动发现之前阻止宿主探测；包含保存的旧路径和上级源码路径 | 原生增强由用户选择，失败只影响该后端；不得阻断独立功能 |
| 外部运行依赖 | Windows 和合适的图形驱动；其余已部署依赖来自包内 | 原生增强额外要求兼容 Linux/WSL、ROS 1 Noetic、RViz、Qt5/OGRE、所需插件和工作图形环境 |

最后一行的外部 ROS 生态可能使用其自身的 Python 工具；这不是独立 Windows 软件的依赖。
宿主及启动器不能以“C++ 实现”为理由声称其完整 ROS 环境完全不需要 Python。
自动连接守卫属于应用运行逻辑；只移除安装包中的 ELF 不足以证明它没有从旧设置或源码目录
触发 WSL，必须单独验证。当前打包测试不监控全部子进程活动，不替该运行验收项盖章。

## 能力、实现与验收边界

“已实现”说明存在实际代码路径，“已有专项证据”只认证该专项；表内所有 GUI、性能及
真实素材限制继续适用。原生结构测试不等于该 Display 已收到真实数据并正确渲染。

| 能力 | Windows 独立 C++ / Qt6 路径 | 可选原生 RViz 路径 | 当前证据及限制 |
| --- | --- | --- | --- |
| 四工作区、亮/暗/系统主题、保存编辑状态 | Qt Quick、统一 AppearanceManager / Design Tokens；与 ROS 无关 | 使用相同应用 UI；原生属性来自实际宿主 | 需四工作区亮暗真实截图及完整交互验收；不以模型检查替代 |
| ROS1 `.bag` 文件、连接/类型/消息定义/字段浏览 | C++ bag 读取、BZ2/LZ4 及消息定义解析；不调用 `rosbag`/ROS master | 可由原生 rosbag/ROS 链路读取、索引、投递 | 读取未知消息的元数据不代表能渲染未知消息；源文件只读 |
| 实验时间、视频 PTS | `Recording` 的整数 ns，C++ FFmpeg 的整数 PTS + 有理数 time_base；偏移/同步锚点映射 | JSON ns 字符串、真实 Header/record 明确选择、精确 `/clock` 与 TF | 独立 FFprobe/解码及随机定位专项已有证据；完整真实视频/录包长期漂移另验，见 `PRECISE_VIDEO_SYNC_CN.md` |
| 点云、LaserScan | 实际 OpenGL `CloudView` 和有限已实现解码/显示属性 | 实际 RViz PointCloud/PointCloud2/LaserScan 等插件，依实际安装环境 | 原生 PointCloud2 两个真实 Topic 已验；LaserScan 原生真实数据未验，不能从类创建成功推定 |
| Livox 自定义消息 | 指定独立 scan 解码支持 `livox_ros_driver/CustomMsg` 与 `livox_ros_driver2/CustomMsg`，是本软件独立实现 | 当前原生 factory 未发现兼容 `livox_ros_driver2/CustomMsg` 的插件 | 不把独立扫描解码称为原生插件；不强制将自定义类型映射成 PointCloud2 |
| Path、Pose、PointStamped、Odometry | 显式支持的几何消息解码和独立绘制；有限样式/历史 | 对应真实插件、实际完整原生属性 | 用户录包原生 `/path` 无源，不能由 Odometry 伪造 Path 后称已通过；原生 Odometry 两 Topic 已验 |
| Marker/MarkerArray | 有限已实现基础几何、消息时间/ID/删除生命周期 | 实际 Marker/MarkerArray 插件及其资源加载语义 | 独立实现不能声称覆盖原生所有 mesh、纹理、材质及交互；原生实际完整数据另验 |
| Grid、Axes、Group | 有实际独立对象、层级及对应有限属性 | 真实 RViz 对象及 Property | 原生 Grid/Axes 实际输出已见；空 Group 不属于传感器渲染证据 |
| TF 与相机跟随 | 实际 TF 树、时间插值与共享相机跟随实现；缺失链保留错误 | 原生 tf2、FrameManager、ViewController 实际属性 | 真实 `camera_init→body` 已验；缺失 `livox_frame` 不补造，原生 Imu 已投递1594条但因缺 TF 拒绝输出 |
| 多机位/小窗口 | 独立 CloudView 机位及自由画布布局 | 共享原生Scene/Display/Replay，独立真实ViewController/RenderPanel及三个实际画布Image | r5自动工作流验证同时间三相机合成；每机位独立Display显隐/材质、多源隔离未完成；可见旧机位仍拒绝原生导出 |
| `.rviz` 导入/编辑/保存 | YAML、固定支持类、独立实例/Group/未知字段保留；`rvizDisplayPropertyTree` 是独立属性，不是实际 librviz Property | 实际 RViz Reader/Writer、VisualizationManager 和插件动态 Property | 未编辑原文、编辑后的未知字段/失败插件保留有专项回归；两条路径不混称“完整原生” |
| 原生 Display / View / Tool 属性 | 没有 pluginlib 或原生编辑器；只提供明确定义的独立显示属性 | 实际工厂发现、真实属性树/枚举/条件项/Status | 33 Display 结构往返通过、6 View/9 Tool 已发现；不等于全部编辑器或全部工具 GUI 已验 |
| Camera/Image、RobotModel/Effort、Map/DepthCloud、交互插件和第三方插件 | 无完整原生实现；保留配置/类型并明确能力边界 | 依已安装插件、图像/URDF/资源/服务/TF 实际运行 | 本轮多数缺对应真实数据/URDF/服务，维持未验收；不能硬编码属性样例冒充真实模型 |
| 视频编辑、时间轴、自由画布、速度表 | 共享 C++ 模型/渲染、实际数据绑定及 Qt 交互；不依赖宿主 | 原生主/附加机位进入同一画布，按各机位最终像素尺寸原子准备 | r5实际主应用自动合成保留文字/速度表并恢复编辑/播放状态；完整手动GUI、全部工作区及长时间性能仍需验收 |
| 无人机真实前景/拖影图 | C++ OpenCV 提取、前景与背景/轨迹独立图层及缓存 | 不依赖 RViz 宿主 | 真实样本提取/质量和编辑流程按前景验收文档核查；不得用模拟轮廓替代真实无人机 |
| 视频导出 | 共享渲染 + 包内 FFmpeg/FFprobe、准确 PTS 解码；无需外部工具 PATH | 原生各机位按精确ns请求真正RenderTexture，再进入同一合成器 | r5三机位两帧短片与同时间静图编码前像素一致；有损MP4不逐像素一致，视频上限3840×2160；长片/完整GUI另验 |
| PNG/JPG/TIFF/PDF/SVG 单帧、批量及多联图 | 共享画布/时间；适用文字/轨迹/曲线可用矢量，视频/3D 按实际位图嵌入 | 原生视角是实际离屏位图，PDF/SVG 中诚实嵌入 | Qt TIFF 插件必须入包；主合成静图当前单边8192/40MP；批量、各视角、选层及多联 GUI 门禁保持未完成项 |
| 透明/背景色/8K/分块 | 以相应独立渲染器的实际能力及输出测试为准；应用主题不决定导出配色 | 当前原生输出opaque PNG，不能透明覆盖背景；尚未实现原生分块 | r5实际主画布7680×4320合成通过，主机位全8K、小窗按像素面积；不认证任意布局、GPU边界或所有格式；原生alpha正确拒绝 |
| 连续播放与性能 | 有限独立渲染路径，须按真实素材测量 | 严格历史推进已测约174–191ms/33ms实验步，预览上限4Hz | 原生路径没有30fps实时认证，不将4Hz预览称流畅实时播放；P95/P99及长时间资源验收仍开放 |
| ROS2 / Windows 原生 RViz | 当前无 ROS2/RViz2 运行后端，也未交付 Windows 原生 librviz | 当前为 Linux ROS1 Noetic 宿主 | 下方可行性调研只用于后续方案，不构成现有功能声明 |

## 原生三相机自动工作流与完整CTest

本机 `captures/native-multiview-composition-20261127-r5/native-scene-acceptance.json` 使用
Windows Qt6主应用、真实Bag及可选Linux/Noetic宿主，程序操作实际画布完成10案例：7个成功
输出，另3个分别正确取消、拒绝缺TF和拒绝原生透明；失败案例无最终文件或合成帧发布。
成功输出消费21个原生相机帧，总计23帧另含取消前1帧和缺TF批次的另一有效机位1帧。
这些不是23个成功文件或手动GUI通过项。

完整画布实际输出1080P/4K/8K，三个原生相机Image的source、精确ns、TF/scene就绪及尺寸
均逐帧检查。8K画布的实际原生源为主机位7680×4320、两个小窗2228×1253和2381×1340，
不是三个全8K源。选中视角、选层、短MP4及失败后的恢复均通过相应自动断言；源/编辑状态
保留、加载信号为0。恢复静图及同时间MP4编码前帧均与完整1080P静图像素差0；实际有损
MP4解码MAE约0.603–0.609/255。UI heartbeat只证明事件循环响应，不证明实时或长期性能。
完整案例、时间、报告hash和宿主版本归属见 [原生矩阵](RVIZ_COMPATIBILITY_MATRIX_CN.md)。

独立的本轮完整CTest为29/29通过、0失败、47.50s，日志
`build-native/current-full-ctest-20261127.log`；r5不属于这29个CTest。组件/自动工作流不能
代替手动鼠标交互或四工作区全流程。此前手动小数时间跳转的画布
未同步提示残留，在后续7AE9…构建的8.069872064509182s暂停操作中未复现：Inspector和中央
画布短暂pending后匹配，真实截图/录屏在 `captures/manual-native-current-7AE9/`；旧根因未确认，
全部随机跳转仍需验收。原生alpha、每视角Display显隐/材质、分块及第三方异步
完成的边界不变。本机已经安装WSL/ROS，以上结果不能声称干净系统已测通过；该项现已移交用户发布后手测。

## 本轮实际打包检查

- `native/tools/test-windows-runtime-payload.ps1` 本机20项通过：默认忽略已存在的开发 host、
  stale 目录拒绝、显式无效 ELF 拒绝、Python/ROS/WSL/外来 ELF 类别拒绝、真实 ELF 显式复制
  及 SHA256/平台声明/LF 无 BOM。该脚本不启动 ELF，报告 `runtime_execution_verified=false`。
- 当前旧开发 staging 的1447个文件通过静态载荷扫描，无 `rviz_host`、Python/ROS/WSL
  运行文件或外来 ELF。它是此前已存在的 dirty 工作树开发包；本检查没有重新认证其中二进制
  与当前源代码一致，不能代替最终干净构建及新包运行验收。
- 同一个 `windows.nsi` 的两种模式均实际完成隔离 fixture 的编译→安装→检查载荷→卸载。
  默认不传 define 时排除 `rviz_host`；显式 define 为1才包含。fixture 使用明确的非可运行
  占位文件，仅验证打包选项，不声称为真实可运行分发。编译器在沙箱内曾被系统 DLL 重定位
  错误阻止；同一命令经工具自动审核允许后在隔离测试目录成功，不改变系统安全设置。
- 四个相关 PowerShell 脚本及 workflow 的6段 PowerShell 已通过 AST 语法检查。CI 新门禁仍需下一次真实 runner 执行；
  未把本机修改后的 workflow 标记为已运行通过。
- 最终 PowerShell7 回归20项再次通过；另一次 Windows PowerShell5 尝试在显式 ELF 哈希
  步骤因该子进程找不到 `Get-FileHash` cmdlet 失败，保留原日志，不声称该环境已通过。
  这是打包开发工具的检查，未向应用添加 PowerShell 或 Python 运行依赖。
- `new-package-runtime-check.ps1` 默认 `-RuntimeProfile Standalone`，仅当显式指定
  `OptionalNativeRviz` 才接受宿主载荷。它验证两个 manifest、实际文件哈希、依赖、安装/
  卸载及指定 smoke，清除 Qt/ROS/Python 相关环境变量并限制 PATH，报告保留真实环境边界。

证据位于本机忽略目录 `captures/windows-payload-guard-final/`、
`captures/windows-backend-current/`。这些检查没有调用或启动 WSL，也没有运行主窗口。
完整新安装包的 smoke、自动后端选择、真实四工作区 GUI、视频/单帧输出及完整用户流程
必须在本轮最终构建后再验，不从已有包的历史日志借用结论。

## 新 staging 输入与源码/资源复核

2026-11-27 新增部署前 `Assert-FreshWindowsStaging`：只接受 CMake 安装的两个产品程序，
以及与指定 FFmpeg 开发包逐文件 SHA256 一致的DLL。Qt/OpenCV/CRT/说明/许可由后续步骤
部署；旧插件、旧codec、测试exe、媒体、可选宿主或其他目录均明确拒绝，原文件不删除。
每次打包使用新 staging，检查结果写入 `windows-runtime.json.staging_input`。

本机实际执行 `test-windows-staging-input.ps1` 26项通过，包括合法输入、同名不同codec、
残留目录/媒体/测试程序和缺少CLI的拒绝及输入保留；原载荷保护含真实ELF复制20项再次通过。
`check-windows-packaging.ps1` 的26项静态检查通过：41个运行C++翻译单元全部在产品构建图，
42个QML全部嵌入，128条CMake源码/测试/资源路径存在，图标格式和各尺寸存在，PowerShell
语法正确。图标变更现也触发Windows CI。检查均不运行主程序、不构建、不启动WSL；新增CI
步骤尚需真实runner执行。证据：`captures/windows-staging-input-audit/`、
`captures/windows-packaging-static-audit/`、`captures/windows-payload-guard-staging-audit/`。

这些保护针对打包源目录。新的NSIS在目录页及实际解包前检查目标位置：已有 `app`（目录、
文件或链接）或 `Uninstall.exe` 时返回2并拒绝，不覆盖、合并或清除旧载荷。用户可选择新的
安装位置。自动原位置升级、版本/载荷迁移及按所有权清单卸载尚未实现；本改动不把这些
列为已完成，也不修改历史发行版的卸载器。邻近后端manifest的自动连接守卫同样不能代替
磁盘载荷检查。

真实隔离安装fixture的42项检查已通过：包含旧codec、可选host及用户额外文件的原安装在
拒绝前后文件数/路径/SHA256完全一致，快捷方式、文件关联及卸载登记保持；同一安装包在
新位置成功安装，未继承旧codec/host；卸载其他独立位置不会破坏拒绝后的原安装或更新位置。
fixture中的旧codec/host是明确的非可运行文本，本检查不声称它们被加载，也不替代真实分发
运行。证据：`captures/installer-upgrade-refusal-unified-0d445e523959/installer-check.json`。

统一构建 `0D445E523959…` 的第一次全新部署/NSIS编译及独立安装文件哈希通过，但离屏QML
检查超时；当时 `windeployqt` 只部署了 `qwindows.dll`，没有 `qoffscreen.dll`，保留失败报告。
打包现显式加入所选Qt6.8.3发行包的官方 `qoffscreen.dll`，不从其他Qt环境取插件；实际dumpbin
只列Qt6Gui/Qt6Core、VC/UCRT及Windows内核依赖，未新增ROS/WSL/Python依赖，其许可沿用包内
Qt许可。运行脚本新增 `-Offscreen` 插件预检，清除Qt/QSG/QML/ROS/Python等环境覆盖并限制PATH；
失败也尝试卸载本次独占的隔离目录。新全流水线及复验结果另记，不能用该修复宣称运行已通过。

## 发布后用户环境手测移交

本机只读探测没有找到 WindowsSandbox.exe、Hyper-V VM 服务/命令或可访问的 Windows
虚拟机配置。已安装 VMware Workstation，inventory 有5个空槽、0条非空 VM 配置。
查询可选 Windows 功能需要提升权限，因此未继续该查询，更没有启用功能、改安全设置、
卸载 WSL/ROS/Python 或启动任何用户 VM。探测报告不记录私人 VM 名称。

当前宿主最小 PATH 和清空环境可验证 DLL、工具及插件从包内解析；System32 仍然可能存在
WSL，主机既有设置/注册表/驱动/组件也仍存在，故不能证明“从未安装 WSL/ROS/Python”。
`clean_windows_without_wsl_verified` 当前为 false，表示本机没有取得该环境的验证证据。
用户已于2026-11-28将干净Windows安装/运行安排为正式发布后的手动测试，不再作为v1发行门槛。
不再寻找VM、准备新环境或将环境缺失作为本阶段阻塞；责任移交不能把该字段改成true。
用户后续可按安装→导入真实bag/video→独立显示/属性/TF/多视角→编辑→单帧/视频/拖影导出→
保存/重开→卸载验证，并检查未启用原生增强时没有不必要的WSL探测/子进程。
当前最终包仍须具备完整依赖，并完成本机实际运行、安全同路径覆盖升级及其他核心门禁。

## Windows 原生 RViz 可行性：官方资料边界

2026-11-27 查阅以下 ROS 官方维护资料。文档站当时返回访问拒绝，故核对相同官方仓库
中的原始文档/构建文件，而没有以第三方教程作为支持认证。

- [REP 3 官方原文](https://github.com/ros-infrastructure/rep/blob/master/rep-0003.rst)：
  Noetic 的目标平台列 Ubuntu20.04、Debian Buster、Fedora32，没有列 Windows；支持期限
  标为2020年5月至2025年5月。不能从“本机 Noetic 在 WSL 可用”推出 Windows 原生受支持。
- [REP 2000 官方原文](https://github.com/ros-infrastructure/rep/blob/master/rep-2000.rst)：
  Jazzy 的 Windows10 / VS2019 amd64 是 Tier1，包含源码/归档交付机制。这是 ROS2 平台
  支持，不能等同本项目 ROS1 Noetic 插件、bag、消息和配置已经兼容 Windows。
- [ROS2 Jazzy Windows 安装文档原文](https://github.com/ros2/ros2_documentation/blob/jazzy/source/Installation/Windows-Install-Binary.rst)：
  预编译包不包含全部 ROS2 包；其依赖环境和 ABI 约束必须实际部署验证。安装 ROS2 也不能
  自动解决用户 ROS1 bag 或第三方 ROS1 Display 的兼容问题。
- [RViz2 Jazzy 官方构建文件](https://github.com/ros2/rviz/blob/jazzy/rviz_common/CMakeLists.txt)：
  使用 `ament_cmake`、`rclcpp`、Qt5、rviz_ogre_vendor/rviz_rendering；它与目前的 ROS1
  `catkin`/`roscpp` 宿主不同，不是替换可执行文件后即可继承原生插件覆盖率。
- [ROS1 RViz noetic-devel 官方构建文件](https://github.com/ros-visualization/rviz/blob/noetic-devel/CMakeLists.txt)：
  当前源码包含 MSVC 条件和 Qt5/Qt6 选择，同时依赖 catkin、ROS1、OGRE、PythonLibs 等。
  因此 Windows 原生移植有可继续研究的源码基础；“绝对不能原生移植”也不是本表结论。

工程判断：未来 Windows 原生后端须独立完成 MSVC/Qt/OGRE/ROS ABI 部署、真实 bag/TF 时间
接入、逐插件重建/资源/动态属性及安装包验收。若采用 RViz2，则还需明确 ROS1 数据转换与
插件兼容策略。两种工作都没有在当前提交中实现；继续保留独立 C++ 核心和显式可选宿主，
不把移植调研当作已完成能力，也不取消最终原生兼容要求。
