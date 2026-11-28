# ROS 动态 Display 架构专项修正

来源：用户最新实际运行反馈（2026-11-27）。这是最高优先级的核心增量，补充之前全部需求，不回滚有效实现。原生渲染、GUI、配置往返与性能均须实测，不能仅修改 QML 或用固定类型冒充完整 RViz。

2026-11-28 发行范围以 [RViz 生态调整](V1_RVIZ_ECOSYSTEM_SCOPE_CN.md) 为准：先完成三生态常用
Display 的真实渲染及配置兼容；其余默认/第三方插件保留长期完整目标，结构通过而真实渲染未验
须明确标注并转后续 PR。本次不取消动态实例/属性、全部Topic浏览、未知配置保留及其他核心门禁。

## 架构及实例

三维系统不能依赖一个全局点云、里程计、速度或固定轨迹 Topic，不能限制于少数预设消息类型/名称。检查 QML、Controller、Bag 解析、Topic 管理、渲染及场景模型。每个动态 Display 独立保存唯一 ID、Class、Name、数据源、Topic、消息类型、启用状态、原生属性树、状态、渲染资源、所属场景/视角、历史及缓存。同类型、同 Topic 均可创建任意多个实例，数量只受资源限制；颜色/透明度/大小/历史独立。

## 完整 Topic Browser

Bag 自动索引全部 Topic；实时 ROS 动态发现全部 Topic。命名空间展开、搜索、消息类型和数据源筛选，显示名称、类型、消息数量、频率、时间范围、可解析字段。不支持的类型也列出。无兼容 Display 的消息仍可查看原始消息、嵌套字段及数组；数值字段可加入曲线/仪表/统计。缺少定义或类型支持须给出具体原因。

## 原生 Displays 与界面职责

优先使用原生 RViz 插件发现、Display 实例与 Property 系统，支持实际对应版本全部适用默认与兼容第三方插件，不能用手写几个渲染类型代替。范围包含 Grid、GridCells、Axes、TF、PointCloud/2、LaserScan、Map、Marker/Array、Path、Odometry、Pose/Array/WithCovariance、Image、Camera、RobotModel、Range、Polygon、PointStamped、WrenchStamped、InteractiveMarkers，且不限于此。保留动态/条件子属性。

左侧 RViz Displays/Topic Browser：Global Options、Fixed Frame、Background、Displays、复选启用、展开折叠、Status、Add/Remove/Rename/Duplicate/Group。Add 内联展开 By Topic 与 By Display Type，使用真实话题和动态兼容插件。左右面板共享同一属性模型，选择对象后右 Inspector 显示相应属性。保持 Apple 风格、亮/暗/系统、可读文字与输入框、轻量列表、内联编辑，普通属性不弹独立窗口。

- Data：Bag、实时连接、数据源、Topic 清单、项目开/存/包、RViz 导入导出。
- Displays：实例管理、各自 Topic 和原生属性、重复类型、多实例、添加删除。
- Camera：场景 Fixed Frame 关联、Target Frame、任意 TF、位姿来源、距离、平滑、视角。
- Telemetry：速度表来源、自定义数值字段、曲线、统计区间及科研指标。
- 旧固定下拉只允许作为明确的快速预设，不限制完整显示系统。

## .rviz 往返

按 Class 和独立属性创建全部实例/Groups。例如 3 PointCloud2 + 2 Path + 4 MarkerArray + Odometry + TF + Grid 应保留全部 12 个 Display。恢复树、Class、Name、Enabled、Topic、Fixed Frame、颜色、Alpha、点大小、Decay Time、原生属性、视角及其他可序列化配置；缺失插件、未知属性、不兼容信息保留原文并报告。使用实际对应版本原生 RViz 打开导出文件验证，不仅检查文件存在。

## 专项验收

使用多消息、非预设名称、自定义消息 Bag 实际 GUI/底层渲染测试并保存真实截图：

1. 全部 Topic 与原生 ROS 工具清单一致，包括不支持的消息。
2. 多个 PointCloud2、MarkerArray、Path 同时存在；同 Topic 多实例属性独立。
3. 单实例独立改 Topic/样式；删除不影响其他实例；不同视角独立显示对象。
4. 多 Display/Group .rviz 完整恢复、未知插件/属性明确报告、原生 RViz 往返。
5. 自定义嵌套/数组数值消息可查看并生成曲线；缺定义明确提示。
6. 大量 Topic/Display 的真实响应与性能；主题切换、编辑不重读 Bag/重建场景。
7. 截图展示完整 Topic Browser、多 Display、展开原生树、导入后实际渲染。

保持 C++/Qt、无 Python 应用运行依赖；完成核心架构后继续此前科研、视频、制图、品牌及发行。未真实完成全部验收前不得发布稳定版。
