# UAV Studio——实际测试反馈及 UI 交互专项修正

## 01. 当前任务执行说明

你目前正在执行此前提供的 UAV Studio 完整重构及科研功能升级任务。

我刚才查看了当前软件的实际测试效果，发现部分界面过暗、文字和控件看不清、弹出设置窗口过多、Topic 配置操作不够方便等问题。

以下内容属于当前开发任务的增量修正指令，不是要求重新开始整个项目。

请遵循以下原则：

- 不要回滚已经完成的有效代码。
- 不要破坏当前正在进行的关键代码修改。
- 不要重复开发已经完成且通过验收的功能。
- 将本次新增反馈纳入现有开发计划、需求文档、AGENTS.md 和完整验收矩阵。
- 完成当前必要的开发步骤后，优先处理本次反馈。
- 如果新旧 UI 设计要求存在冲突，以本次最新反馈为准。
- 修复完成后，继续推进此前尚未完成的全部任务。
- 不需要等待我的进一步确认。
- 自行开发、自行编译、自行运行、自行截图、自行测试、自行修复。

此前全部功能、技术架构、科研功能升级、项目品牌迁移、应用图标、性能优化及正式版本验收要求继续有效。

保持项目最终定位：

**UAV Studio — All-in-One Robotics Research Studio**

继续使用 C++ 和 Qt 开发，不引入 Python 应用运行依赖。

---

## 02. 全面修复 UI 可读性问题

目前软件部分区域过暗，文字、图标、控件和背景之间缺乏足够对比度，导致实际使用时难以辨认。

不要为了追求高级感而牺牲可读性。

必须全面检查四个主要工作区：

- Visualization（数据可视化）
- Synchronization（数据编辑与同步）
- Video Editor（多视角视频剪辑）
- Trajectory Studio（无人机轨迹图生成）

以及所有公共组件。

### 2.1 重点检查内容

- 主标题及普通文字。
- 次级文字及中文说明。
- 中英文属性名称。
- 输入框文字。
- 输入框占位文字。
- 下拉菜单及菜单选项。
- 按钮。
- 图标。
- 滑块。
- 滚动条。
- 复选框。
- 单选框。
- 开关。
- 时间轴。
- 时间刻度。
- 时间轴轨道。
- 曲线坐标轴。
- 图层列表。
- Displays 属性树。
- Inspector 属性面板。
- Tooltip。
- 右键菜单。
- 状态提示。
- 错误提示。
- 选中状态。
- 鼠标悬浮状态。
- 禁用状态。
- 浮动时间轴。
- 缩略图胶片带。
- 排除区间。
- 速度表及科研数据组件。

禁止出现：

- 深色背景上的黑色文字。
- 亮色背景上的白色文字。
- 难以辨认的暗灰色文字。
- 图标与背景颜色过于接近。
- 输入框与面板背景无法区分。
- 下拉菜单展开后文字看不清。
- 正常可操作控件看起来像禁用状态。
- 选中对象后文字消失或对比度下降。
- 因多层透明度叠加造成文字和控件过暗。

### 2.2 排查实际原因

重点排查：

- Qt/QML 默认 Palette。
- Qt Quick Controls 主题继承。
- QML 控件硬编码颜色。
- Qt Widgets 与 QML 主题不一致。
- 自定义控件颜色绑定错误。
- opacity 属性错误继承。
- 父组件透明度影响子组件。
- 深色模式下仍然使用亮色主题默认文字。
- 下拉菜单、弹出菜单及 Tooltip 使用独立错误主题。
- 主题切换后部分控件没有更新。
- 禁用状态与普通状态样式混淆。

不要只调整几个背景颜色就认为已经完成优化。

需要从主题系统及公共组件层面解决问题。

---

## 03. 亮色及深色模式重新优化

继续完整支持：

- Light（亮色模式）
- Dark（深色模式）
- System（跟随系统）

保持此前确认的 Apple 风格设计及整体布局。

亮色与深色模式必须使用相同的布局、控件尺寸、字体层级和交互逻辑。

### 3.1 深色模式建议配色

采用柔和深灰色，不使用过于压抑的纯黑背景。

建议初始 Design Tokens：

- Application Background：#1E2229
- Panel Background：#252931
- Input Background：#363D48
- Primary Text：#F1F3F7
- Secondary Text：#BFC8D5
- Disabled Text：#798390
- Accent Color：#82B4F5
- Selected Background：#364F6E
- Input Border：#505B69
- Divider：#414955

深色模式要求：

- 主文字清晰明亮。
- 次级文字柔和但容易阅读。
- 输入框具有明确的视觉边界。
- 图标保持清晰。
- 下拉菜单内容清晰可读。
- 选中状态容易识别。
- 面板之间具有适度层次感。
- 避免大面积纯黑和刺眼纯白。

### 3.2 亮色模式建议配色

建议初始 Design Tokens：

- Application Background：#F2F4F7
- Panel Background：#FFFFFF
- Input Background：#E9EDF2
- Primary Text：#202630
- Secondary Text：#596579
- Accent Color：#2868BC
- Selected Background：#DCE9FA
- Input Border：#CBD3DE
- Divider：#DDE2E9

亮色模式要求：

- 整体干净、清爽。
- 不使用过多纯白造成视觉层级缺失。
- 控件、背景和面板之间容易区分。
- 保持与深色模式一致的设计语言。

以上颜色仅作为初始设计基准，可以根据实际运行效果调整。

普通文字与背景的对比度原则上达到至少 4.5:1，重要图标及控件边界尽量达到至少 3:1。

对比度检查应以实际合成后的显示颜色为准，而不是仅检查颜色配置文件。

### 3.3 统一主题管理

建立或完善统一的 Theme Manager。

集中管理：

- Colors（颜色）。
- Typography（字体）。
- Spacing（间距）。
- Border Radius（圆角）。
- Icons（图标）。
- Component States（控件状态）。
- Motion（动画）。
- Shadows（阴影）。

禁止在各个业务模块中重复硬编码不一致的颜色。

所有 QML、Qt Widgets、自定义控件及公共组件必须正确响应主题变化。

### 3.4 主题切换性能

切换亮色、深色或跟随系统模式时，不得：

- 重新加载 ROS Bag。
- 重新解析全部 Topic。
- 重新创建三维场景。
- 重新初始化渲染引擎。
- 重新初始化视频解码器。
- 丢失相机位置。
- 丢失 TF 跟随状态。
- 丢失时间轴位置。
- 丢失图层编辑状态。
- 中断正常播放。
- 出现明显黑屏、白屏、闪屏或卡顿。

软件界面主题、三维场景背景、最终视频配色及科研图片输出样式应保持独立。

用户使用深色软件界面时，也可以直接生成纯白背景的论文配图。

---

## 04. 尽量取消独立设置弹窗

我不喜欢点击某个设置后频繁弹出独立的小窗口。

除非确实必要，否则普通配置操作统一采用当前页面内的设置方式。

优先使用：

- ComboBox（下拉选择框）
- Searchable ComboBox（可搜索下拉框）
- Expandable Section（可折叠面板）
- Property Tree（属性树）
- Inline Editor（内联编辑）
- Segmented Control（分段选择器）
- Inline Color Picker（内联颜色选择器）
- Embedded Panel（嵌入式面板）

### 4.1 重点改造的功能

- 添加 Display。
- 选择 Display 类型。
- 配置 Topic。
- 配置 Fixed Frame。
- 配置 Target Frame。
- 配置 TF。
- 点云属性。
- 轨迹属性。
- Marker 属性。
- 相机类型。
- 相机跟随参数。
- 颜色选择。
- 速度表配置。
- 数据曲线配置。
- 图层属性。
- 时间同步配置。
- 视频剪辑参数。
- 轨迹图采样参数。
- 不透明度。
- 排除区间。
- 科研制图属性。
- 视频导出参数。

所有普通配置优先放在左侧 Displays 属性树或右侧 Inspector 中。

### 4.2 具体交互要求

点击普通设置后，应优先：

- 展开对应属性。
- 显示下拉选项。
- 在当前区域显示更多设置。
- 在右侧 Inspector 中切换对应属性内容。
- 在当前页面展开高级设置面板。

不要点击一次按钮就弹出一个独立窗口。

下拉菜单本身可以使用正常的轻量弹层，但不应创建独立顶层窗口或阻塞当前工作区。

设置面板展开、折叠及切换时采用自然、克制的过渡动画。

连续调整参数时优先保证即时响应，不增加不必要的动画延迟。

### 4.3 允许保留的窗口

以下确实必要的窗口可以保留：

- 系统文件打开窗口。
- 系统文件保存窗口。
- 文件覆盖确认。
- 系统授权。
- 严重错误确认。
- 用户主动选择的独立浮动面板。
- 用户主动选择的浮动时间轴。
- 单独预览窗口。

此前要求的迷你时间轴及浮动窗口功能必须继续保留。

这些属于用户主动选择的工作区布局方式，不属于需要取消的设置弹窗。

---

## 05. Topic 配置操作全面参考原生 RViz

这是本次修正的核心要求之一。

我希望配置 Topic、Display、TF 和点云属性时，操作方式与原生 RViz 基本一致。

不要重新设计一套与 RViz 完全不同的复杂操作流程。

我的目标是已经熟悉 RViz 的科研人员打开 UAV Studio 后，不需要重新学习如何配置 ROS 数据。

保留 RViz 熟悉的操作逻辑，同时使用此前确定的现代 Apple 风格 UI。

### 5.1 左侧 Displays 属性树

建立完整的 RViz 风格 Displays 管理树。

示例结构：

Global Options（全局设置）
    Fixed Frame
    Background Color

Grid
    Enabled
    Plane
    Cell Size
    Color
    Alpha

TF
    Show Names
    Show Axes
    Show Arrows
    Marker Scale
    Frames
    Tree

PointCloud2
    Status
    Topic
    Style
    Size
    Alpha
    Decay Time
    Position Transformer
    Color Transformer

Odometry
    Status
    Topic
    Shape
    Keep
    Position Tolerance
    Angle Tolerance

Path
    Status
    Topic
    Color
    Alpha
    Line Style
    Line Width

MarkerArray
    Status
    Topic
    Namespaces

以上只是界面结构示例。

实际属性应来自当前 ROS/RViz 版本的真实插件属性系统，不得将这些示例硬编码为完整属性列表。

### 5.2 Displays 基础管理

每个 Display 支持：

- 复选框启用及禁用。
- 展开及折叠。
- 状态显示。
- 属性编辑。
- 重命名。
- 删除。
- 复制。
- 拖动排序。
- 分组。
- 恢复默认值。

状态支持：

- OK（正常）
- Warning（警告）
- Error（错误）
- Disabled（已禁用）
- No Data（暂无数据）

状态文字及图标必须在亮色和深色模式下均清晰可见。

展开 Status 后，应能查看具体原因，例如：

- 缺失 TF。
- Topic 暂无数据。
- 消息类型不兼容。
- 数据时间戳异常。
- 插件加载失败。
- 缺少外部资源。

对于实际没有接收到数据或存在坐标变换错误的 Display，不得显示虚假的 OK 状态。

### 5.3 Add（添加）操作

在左侧 Displays 面板底部保留类似 RViz 的操作入口：

- Add（添加）
- Remove（删除）
- Rename（重命名）
- Duplicate（复制）

点击 Add 后，优先在当前面板中展开添加区域。

提供两种操作模式：

#### By Display Type（按显示类型添加）

列出当前 ROS/RViz 版本实际支持的全部默认 Display 类型及兼容插件。

例如：

- Axes
- Grid
- GridCells
- TF
- PointCloud
- PointCloud2
- LaserScan
- Map
- Odometry
- Path
- Pose
- PoseArray
- PoseWithCovariance
- Marker
- MarkerArray
- Image
- Camera
- RobotModel
- InteractiveMarkers
- Range
- Polygon
- WrenchStamped
- 其他对应版本的原生 Display。

支持：

- 搜索。
- 分类。
- 查看显示类型说明。
- 快速添加。
- 自动创建对应 Display。

添加完成后，可以直接在属性树中选择 Topic 及修改参数。

#### By Topic（按话题添加）

自动读取当前 ROS Bag 或实时 ROS 环境中的实际 Topic。

按照 ROS 命名空间组织树形结构。

例如：

/cloud_registered
    PointCloud2

/odom
    Odometry

/planning
    /trajectory
        Path
    /visualization_marker
        Marker
    /marker_array
        MarkerArray

以上仅为示例，不得硬编码话题名称。

支持：

- Topic 搜索。
- 按命名空间展开。
- 消息类型显示。
- 数据源区分。
- 可用 Display 类型自动匹配。
- 快速添加。
- 批量添加适用的显示对象。

选择对应 Topic 及 Display 类型后，应立即创建真实的显示对象。

不要要求用户手动填写内部插件 Class 或完整消息类型名称。

### 5.4 Topic 下拉框

每个适用的 Display 都应该拥有独立的 Topic 属性。

Topic 使用可搜索下拉选择框。

要求：

- 自动读取当前 ROS Bag 中真实存在的 Topic。
- 支持实时 ROS 环境的话题发现。
- 默认筛选消息类型兼容的话题。
- 显示完整 Topic 名称。
- 支持输入关键字搜索。
- 支持长名称 Tooltip。
- 支持切换数据源。
- 支持不同 ROS Bag 中的同名 Topic。
- 支持切换 Topic 后立即更新显示。
- 支持保留暂时无数据的 Topic 配置。

同一个 Topic 可以创建多个独立 Display，分别设置颜色、大小、不透明度及历史保留时间。

切换一个 Topic 时，不得重新加载整个 ROS Bag 或重建全部三维场景。

### 5.5 Fixed Frame 和 Target Frame

在 Global Options 中提供 Fixed Frame 下拉选择框。

自动列出当前数据源实际存在的 TF Frame。

例如：

- map
- world
- odom
- base_link
- body
- lidar
- camera

上述名称仅为示例。

不得将可选 Frame 硬编码限制为这些名称。

相机 Target Frame、TF 跟随目标及其他坐标系选择同样采用可搜索下拉框。

允许用户选择任意实际存在的 Frame。

显示 TF Tree，方便检查不同坐标系之间的父子关系。

缺失必要坐标变换时明确提示。

不能为了使场景勉强显示而使用错误坐标变换或单位变换替代真实 TF。

---

## 06. 完整保留 RViz 原生属性系统

对应版本 RViz 中能够修改的属性，本软件也应尽可能完整支持。

禁止将 RViz 属性简化成几个颜色、透明度和大小设置。

优先复用 RViz 原生 Property 属性模型，或者建立经过实际验证的兼容实现。

### 6.1 点云属性

完整支持适用的：

- Topic
- Style
- Points
- Squares
- Flat Squares
- Spheres
- Boxes
- Size (m)
- Size (Pixels)
- Alpha
- Decay Time
- Queue Size
- Position Transformer
- Color Transformer
- Flat Color
- RGB
- Intensity
- Axis Color
- Channel Name
- Min/Max Value
- Autocompute Bounds
- Use Rainbow
- Invert Rainbow
- Selectable

实际属性根据插件版本、消息字段及显示模式动态生成。

例如，切换 Color Transformer 后自动展示该着色方式对应的参数。

不要显示无效属性。

### 6.2 轨迹及姿态属性

完整保留对应原生插件实际支持的：

- Color
- Alpha
- Shape
- Keep
- Buffer Length
- Line Style
- Line Width
- Position Tolerance
- Angle Tolerance
- Shaft Length
- Shaft Radius
- Head Length
- Head Radius
- Axes Length
- Axes Radius
- Offset
- Covariance

### 6.3 TF 属性

完整保留对应版本支持的：

- Show Names
- Show Axes
- Show Arrows
- Marker Scale
- Frame Timeout
- Update Interval
- Frames
- Tree
- 指定 Frame 可见性
- 其他原生属性

### 6.4 其他属性

Marker、MarkerArray、Map、LaserScan、Image、Camera、RobotModel 等显示对象同样必须保留对应版本的原生属性。

正确区分消息本身的字段与可编辑的 Display 属性。

例如 Marker Lifetime 属于 Marker 消息语义时，应正确处理其生命周期，而不是简单伪造一个原生 RViz 属性。

### 6.5 时间类属性

必须明确区分：

- Decay Time（历史点云保留时间）
- Playback Delay（播放延迟）
- Time Offset（时间偏移）
- Follow Lag（相机跟随滞后）
- Camera Smoothing（相机平滑程度）

这些功能不能互相混淆。

离线播放时，应基于正确的实验时间处理历史点云、Marker 生命周期和轨迹历史状态。

实时模式中，正确支持对应版本适用的队列、订阅及 QoS 属性。

离线模式中不适用的属性应明确说明原因，而不是提供看似可以调整但实际无效的控件。

### 6.6 中英文属性说明

继续保留标准英文专业名称及中文解释。

例如：

- Decay Time（历史点云保留时间）
- Queue Size（消息队列长度）
- Color Transformer（颜色映射方式）
- Position Transformer（位置转换方式）
- Fixed Frame（固定参考坐标系）
- Target Frame（目标坐标系）
- Alpha（不透明度）
- Size（显示大小）
- Keep（保留数量）
- Buffer Length（历史缓冲区长度）

复杂参数提供中文 Tooltip，说明作用、单位、取值范围及特殊值含义。

中英文显示不能改变 RViz 原生属性内部标识及配置文件兼容性。

---

## 07. 左侧 Displays 与右侧 Inspector 联动

保持此前确定的左侧管理树、中央预览、右侧 Inspector 和底部时间轴布局。

左侧 Displays 提供与原生 RViz 类似的快速属性展开及编辑操作。

右侧 Inspector 提供选中对象的完整属性及高级设置。

用户可以：

- 在左侧展开 Display 直接修改常用属性。
- 选中 Display 后在右侧修改完整属性。
- 折叠左侧属性以获得更多空间。
- 使用属性搜索快速找到参数。

左右两个面板必须使用同一份属性模型。

不得分别维护两份独立配置。

左侧修改 Topic、Color、Size 或 Decay Time 后，右侧 Inspector 必须立即同步。

右侧修改属性后，左侧属性树也必须立即更新。

全部属性应真实影响底层数据或渲染效果。

不要实现只有 UI 数值变化但三维场景没有变化的假交互。

---

## 08. 完整支持 .rviz 配置文件导入

这是此前已经提出的核心功能，本次必须重点检查并真正实现。

我希望直接使用实验室现有的 RViz 配置文件，不需要每次打开 UAV Studio 都重新添加 Topic 和设置属性。

### 8.1 导入入口

支持：

- File → Import RViz Config（导入 RViz 配置）
- 工具栏快速导入。
- 直接拖入 .rviz 文件。
- 最近使用的 RViz 配置。
- 保存常用配置预设。
- 导出兼容的 .rviz 文件。

导入操作简洁，不需要进入多个设置页面。

### 8.2 完整恢复配置

尽可能完整恢复对应 RViz 版本配置文件实际包含的：

- Global Options。
- Fixed Frame。
- Background Color。
- Displays。
- Display Groups。
- Display Class。
- Display Name。
- Enabled 状态。
- Topic。
- Color。
- Alpha。
- Style。
- Size。
- Decay Time。
- Queue Size。
- Position Transformer。
- Color Transformer。
- TF 配置。
- Grid 配置。
- LaserScan 配置。
- PointCloud 配置。
- Odometry 配置。
- Path 配置。
- Marker 配置。
- MarkerArray 配置。
- Image 配置。
- Camera 配置。
- RobotModel 配置。
- 当前视角控制器。
- 相机位置。
- 相机观察方向。
- 相机其他可序列化参数。
- Tools。
- 其他适用的原生属性。

不要只解析 Topic 名称。

不要只读取少数基础属性。

不要将所有导入的 Display 强行转换为统一默认样式。

优先复用原生 RViz 配置加载机制。

如果当前架构需要自行解析配置，必须严格兼容对应版本的 YAML 结构及原生属性语义，并通过实际 RViz 对照测试。

### 8.3 ROS 版本兼容

ROS 1 和 ROS 2 的 RViz 插件 Class、属性名称及配置结构可能存在差异。

首先确认项目当前支持的 ROS 版本。

针对实际支持版本完成完整导入及导出。

如需跨版本转换，应提供明确的转换机制及兼容报告。

不允许未经验证就声称兼容全部 ROS 1 和 ROS 2 的 .rviz 文件。

### 8.4 导入顺序

必须支持：

方式一：

先导入 ROS Bag，再导入 .rviz 配置。

方式二：

先导入 .rviz 配置，再导入 ROS Bag。

方式三：

导入 .rviz 配置后连接实时 ROS 环境。

如果配置中的 Topic 暂时不存在，应保留原有 Display 和全部属性。

后续加载匹配的数据源时，允许自动恢复显示。

不能因为暂时没有对应 Topic 就直接删除该配置。

### 8.5 缺失插件和兼容性处理

遇到以下情况时：

- 缺少第三方插件。
- 未知 Display 类型。
- 消息类型不兼容。
- RViz 版本差异。
- 未知属性。
- RobotModel 资源缺失。
- Marker Mesh 资源缺失。
- Fixed Frame 不存在。
- Topic 不存在。

需要：

- 保留原始配置信息。
- 显示具体兼容状态。
- 提供详细错误原因。
- 尽可能加载其他正常配置。
- 提供解决建议。

不得静默删除不支持的属性。

不得因为单个 Display 不兼容导致整个项目无法打开。

导入完成后，在当前页面提供可展开的兼容性报告，不要强制弹出复杂窗口。

### 8.6 保持 UAV Studio 原有 UI 布局

导入 .rviz 文件时，默认恢复：

- 场景配置。
- Displays。
- Topic。
- 显示属性。
- Fixed Frame。
- 相机参数。

不要强制将整个 UAV Studio 界面改成原生 RViz 布局。

RViz 的窗口布局及 Panels 可以作为可选导入内容。

不能覆盖本软件已经确认的视频剪辑、自由画布、Inspector 及科研制图工作区布局。

本软件独有的多视角、速度表、视频、时间轴、轨迹图等配置继续使用独立项目文件保存。

### 8.7 导出 .rviz

支持将兼容的显示配置导出为对应版本原生 RViz 可以读取的 .rviz 文件。

UAV Studio 特有属性及视频剪辑相关配置可以保存在独立项目文件中。

导出后必须实际使用对应版本的原生 RViz 打开文件进行验证。

不能只检查文件是否成功生成。

---

## 09. 简化实际用户操作流程

我希望最终软件具备以下简单工作流程。

### 已有 RViz 配置时

打开软件 → 导入 ROS Bag → 导入 .rviz → 自动恢复 Displays 和全部属性 → 检查 Topic、TF 状态 → 开始播放。

如果配置与数据完全匹配，应尽可能不需要再次手动配置。

### 没有 RViz 配置时

打开软件 → 导入 ROS Bag → 点击 Add → By Topic 或 By Display Type → 添加显示对象 → 展开属性修改 → 保存配置。

### 制作科研演示视频时

导入数据 → 导入 RViz 配置 → 添加全局及 TF 跟随视角 → 对齐实飞视频时间戳 → 自由添加小窗口 → 配置速度表和文字 → 编辑时间轴 → 导出视频。

所有操作应清晰、直观。

尽量减少不必要的页面切换、弹窗及重复操作。

---

## 10. 三维场景可读性优化

除了软件界面，还需要检查三维场景自身是否存在过暗问题。

确保：

- 点云清晰可见。
- 全局地图清晰。
- 局部地图清晰。
- TF 坐标轴清晰。
- 无人机位置及姿态容易识别。
- 实际轨迹与规划轨迹容易区分。
- 网格不会干扰主要数据。
- 相机小窗口内容清晰。
- 小窗口标题容易辨认。
- 速度表数值清晰。
- 科研文字标注与背景具有足够对比度。

提供独立可编辑属性：

- Background Color（场景背景颜色）
- Grid Color（网格颜色）
- PointCloud Color（点云颜色）
- Trajectory Color（轨迹颜色）
- TF Scale（TF 显示大小）
- 其他适用的显示属性

提供可选场景预设：

- Default RViz（原生 RViz 风格）
- Research Dark（科研深色风格）
- Research Light（科研亮色风格）
- High Contrast（高对比度风格）
- Publication（论文制图风格）
- Custom（自定义）

导入 .rviz 配置后，默认保留用户原有场景样式。

不要擅自改变原始配置中的颜色、点大小、透明度和历史点云保留时间。

用户主动选择其他预设时，再应用相应样式。

软件深色模式不代表三维场景必须使用深色背景。

---

## 11. UI 流畅度及性能要求

本次 UI 修正不能破坏此前已经进行的性能优化。

重点检查：

- Displays 属性树展开与折叠。
- Add 添加区域展开与折叠。
- Topic 下拉框搜索。
- Fixed Frame 切换。
- Target Frame 切换。
- 属性实时编辑。
- 亮色与深色主题切换。
- 左右面板联动。
- 时间轴缩放。
- 迷你时间轴切换。
- 浮动面板。
- 全屏。
- 最大化。
- 连续窗口缩放。
- 工作区切换。
- 多视角小窗口拖动。
- 点云持续更新期间的 UI 操作。

禁止因为修改普通属性或展开设置面板而：

- 重新加载全部 ROS Bag。
- 重新解析全部数据。
- 重新创建三维场景。
- 重复初始化渲染资源。
- 重复创建全部 Display。
- 中断视频播放。
- 阻塞 UI 主线程。

对于大量 Topic 或属性项，采用合理的数据模型、按需展开及轻量化列表渲染。

不要为每个数据项创建大量重量级独立控件。

保持此前的异步处理、GPU 资源复用、有界缓存及增量更新机制。

连续操作应自然、流畅、丝滑。

不能仅提高平均 FPS，还要重点减少偶发卡顿和 P95/P99 帧时间尖峰。

---

## 12. 保持之前确定的全部功能

本次修正主要针对 UI、操作体验及 RViz 配置兼容性，不代表减少其他功能。

以下所有功能必须继续保留及完善：

### ROS 可视化

- 全部对应版本 RViz 默认 Displays。
- 完整原生属性系统。
- 实时 ROS Topic。
- 离线 ROS Bag。
- TF Tree。
- SLAM 点云及地图。
- EGO-Planner、SUPER 等真实规划输出。
- 兼容第三方插件。

### 相机系统

- 全局视角。
- 任意 TF 跟随。
- 正后方跟随。
- 第三人称跟随。
- 高位倾斜鸟瞰。
- FPV。
- 自由视角。
- 环绕视角。
- 固定视角。
- 正交俯视。
- 关键帧运镜。
- 多视角独立显示属性。

### 视频剪辑

- 自由画布。
- 任意添加小窗口。
- 小窗口拖动及缩放。
- 文字及图片图层。
- 多轨时间轴。
- 多段不连续显示区间。
- 精确时间戳同步。
- 浮动及迷你时间轴。
- 字幕。
- 音频。
- 视频变速。
- B 站视频输出。
- 视频封面制作。

### 科研数据分析

- 实时速度。
- 最大速度。
- 平均速率。
- 手动统计区间。
- 通用 ROS Topic 数据曲线。
- 科研事件标记。
- 多算法对比。
- APE、RPE、RMSE 等轨迹评估。

### 科研制图

- 无人机运动拖影图。
- 最小 0.1 s 采样间隔。
- 滑块实时调整。
- 单帧不透明度。
- 全局不透明度。
- 单帧删除及恢复。
- 多段区间排除。
- 0.1 s 或更精细的区间编辑。
- 双层时间轴。
- 动态缩略图胶片带。
- 虚拟化采样帧列表。
- 长视频处理。
- 距离等间隔采样。
- 姿态变化采样。
- 事件采样。
- 前景提取。
- 蒙版编辑。
- 背景配准。
- 轨迹颜色映射及 Colorbar。
- 多视图论文配图。
- 局部放大。
- 科研标注。
- 真实视频与三维轨迹融合。
- 论文出版模式。
- 矢量及高分辨率图片导出。
- 科研素材包批量导出。

### 软件基础

- C++ / Qt。
- 不依赖 Python 运行。
- Apple 风格 UI。
- 亮色及深色模式。
- 中文属性说明。
- 中文代码注释。
- 项目保存及恢复。
- 全屏及窗口流畅度优化。
- UAV Studio 品牌统一。
- GitHub 仓库重命名。
- 全部文档及 README 更新。
- 专属应用图标及桌面快捷方式。
- Conventional Commits。
- 实际测试及正式版本发布。

不得通过删除原有功能解决本次 UI 问题。

---

## 13. 实际 GUI 测试与截图对照

本次反馈来自实际测试，因此必须通过实际运行软件验证修复效果。

不能只进行编译测试。

### 13.1 主题测试

实际启动软件，分别检查四个工作区的：

- 亮色模式。
- 深色模式。
- 跟随系统模式。

逐项检查文字、图标、按钮、输入框、下拉框、属性树、时间轴及选中状态。

特别检查下拉菜单展开后的颜色。

检查各种状态是否具有正确的文字及背景对比度。

### 13.2 交互测试

实际验证：

- 普通配置是否采用下拉框、内联编辑或折叠面板。
- 是否仍存在不必要的独立设置弹窗。
- Displays 属性树是否接近 RViz 使用习惯。
- 左侧属性树与右侧 Inspector 是否同步。
- Topic 是否支持真实数据源及消息类型筛选。
- Fixed Frame 是否列出真实 TF。
- Target Frame 是否支持任意实际 Frame。
- 修改颜色、透明度、大小、Decay Time 等属性后，场景是否真正更新。

不能只检查控件是否可以点击。

### 13.3 .rviz 专项测试

使用实际或可复现的 ROS Bag 及 RViz 配置文件。

测试：

1. 先导入 ROS Bag，再导入 .rviz。
2. 先导入 .rviz，再导入 ROS Bag。
3. 导入多个 Displays 及 Display Groups。
4. 恢复 Fixed Frame。
5. 恢复背景颜色。
6. 恢复 PointCloud2 属性。
7. 恢复 TF 属性。
8. 恢复 Path 和 Odometry 属性。
9. 恢复 Marker 和 MarkerArray。
10. 恢复相机视角。
11. 导入缺少 Topic 的配置。
12. 导入缺少第三方插件的配置。
13. 检查错误及兼容性提示。
14. 导出 .rviz。
15. 使用对应版本原生 RViz 重新打开导出的配置。
16. 比较原生 RViz 与 UAV Studio 的实际显示效果。
17. 保存项目并重新打开，验证属性保持。

### 13.4 流畅度测试

实际执行：

- 连续展开及折叠属性面板。
- 快速搜索及切换 Topic。
- 连续调整滑块。
- 连续切换主题。
- 连续切换工作区。
- 连续全屏及恢复。
- 连续最大化及恢复。
- 连续调整窗口大小。
- 播放 ROS Bag 时修改显示属性。
- 多视角播放时展开设置面板。

记录真实响应延迟、帧时间及资源占用情况。

发现明显卡顿时继续分析并修复。

### 13.5 截图对照

使用之前已经提供的 UI 参考图作为视觉基准。

保存四个工作区亮色及深色模式下的实际运行截图。

建议路径：

docs/ui-actual/

包括：

- visualization-light.png
- visualization-dark.png
- synchronization-light.png
- synchronization-dark.png
- video-editor-light.png
- video-editor-dark.png
- trajectory-studio-light.png
- trajectory-studio-dark.png

额外保存：

- Displays 属性树展开截图。
- By Topic 添加截图。
- By Display Type 添加截图。
- Topic 下拉框截图。
- .rviz 导入后场景截图。
- Inspector 高级属性截图。

实际截图应与参考图进行对照。

发现明显差异、颜色过暗、布局混乱或控件看不清时，继续修改并重新测试。

不得将设计参考图、静态 UI 原型或模拟效果图冒充实际软件运行截图。

---

## 14. 本次修正的执行优先级

在当前开发计划中提高以下任务的优先级：

### P0：必须优先修复

1. 深色模式文字及控件看不清。
2. 亮色与深色主题颜色绑定错误。
3. 普通设置频繁弹出独立窗口。
4. RViz 风格 Displays 属性树。
5. By Topic 和 By Display Type 添加功能。
6. Topic 可搜索下拉选择框。
7. Fixed Frame 和 Target Frame 选择。
8. 完整 .rviz 配置导入。
9. 导入后恢复真实显示属性。
10. UI 操作流畅度。

### P1：继续完善

- 左侧 Displays 与右侧 Inspector 完整联动。
- 完整 RViz 原生属性覆盖。
- .rviz 导出及原生 RViz 对照验证。
- 场景可读性预设。
- 全部工作区视觉一致性。
- 中文属性说明。
- GUI 回归测试。

以上完成后，继续推进此前全部科研视频、制图、实验分析、品牌迁移、应用图标及其他高级功能。

P0 和 P1 仅表示执行顺序，不代表可以省略其他任务。

---

## 15. 最终交付与执行原则

请将本次反馈作为当前任务的增量修正要求。

保持此前已经确认的整体 UI 布局及有效架构。

不要无理由重新设计整个软件。

不要为了修复 UI 而减少功能。

不要为了提高视觉效果而牺牲运行流畅度。

不要只修改界面而不实现底层功能。

不要只通过编译测试就宣称功能可用。

不要伪造测试结果、运行截图及性能指标。

自行分析、自行修改、自行运行、自行截图、自行测试、自行修复。

如果缺少 GPU、ROS 环境、真实测试数据或图形显示环境，应明确记录实际限制，同时继续完成其他可以执行的任务。

最终提供：

- 本次修正内容。
- 修复前后实际 UI 截图。
- 亮色及深色主题截图。
- RViz 风格 Topic 配置测试结果。
- .rviz 导入及导出测试结果。
- 原生 RViz 对照结果。
- UI 流畅度测试结果。
- 原有功能回归测试结果。
- 未完成事项及具体原因。

将上述内容纳入已有完整验收矩阵。

所有此前要求的核心功能和新增科研功能全部通过实际验收后，再统一更新项目文件、README、安装包、应用图标、桌面快捷方式，使用标准 Git Commit 提交，并发布正式版本。

**我希望最终的 UAV Studio 具备 RViz 的操作熟悉度、Apple 软件的界面美观度、成熟视频剪辑软件的交互流畅度，以及机器人科研软件的数据准确性。**

继续当前任务，将以上要求纳入当前开发计划并优先处理。
