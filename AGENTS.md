# UAV Studio 开发约束

完整需求见 `docs/REBUILD_REQUIREMENTS_CN.md`；增量科研与品牌需求见
`docs/INCREMENTAL_RESEARCH_REQUIREMENTS_CN.md`。后者补充前者，不替换已完成工作。
实测 UI 修正见 `docs/UI_INTERACTION_FEEDBACK_CN.md`，独立画面导出见
`docs/FRAME_EXPORT_REQUIREMENTS_CN.md`；发生 UI 冲突时以最新实测反馈为准。
统一自由画布与科研制图增量见 `docs/UNIFIED_CANVAS_REQUIREMENTS_CN.md`。
四工作区专业职责、无数据启动和可选后端增量见 `docs/WORKSPACE_AND_EMPTY_SCENE_REQUIREMENTS_CN.md`。
本次正式发行范围以最新 `docs/V1_CORE_RELEASE_SCOPE_CN.md` 为准；高级需求保留在 Roadmap，后续独立 PR。
About、异步更新、安全覆盖升级和成功发行后的 Release 清理见 `docs/RELEASE_UPDATE_REQUIREMENTS_CN.md`。
2026-11-28 RViz 验收收敛见 `docs/V1_RVIZ_ECOSYSTEM_SCOPE_CN.md`：本次优先 FAST-LIO、
EGO-Planner、SUPER 常用 Display 的真实渲染及配置兼容；其他插件保留结构/实渲染分级状态，后续独立 PR。

- 应用运行使用 C++ / Qt；保留旧功能与兼容项目，不引入 Python 运行依赖。
- 使用统一外观管理器及语义 Design Tokens；导出配色独立于应用主题。
- 保持四工作区及用户提供的两份 SVG 参考布局。
- 优先稳定数据、时间同步、渲染和自由画布，再实现依赖模块。
- P0 / P1 / P2 是顺序，不是删减范围；全部需求进入验收矩阵。
- 科研指标必须有真实来源、计算定义、单位、区间和对齐方式。
- 不伪造测试、性能、科研指标或截图；未验证和缺失能力明确记录。
- v1.0.0 以2026-11-28最新用户收敛的四工作区必要核心端到端验收为发行门禁，不再穷尽/重复GUI与非必要细节；非阻塞UI、边界兼容和高级科研保留安装后用户手测/后续PR，未测项不得改记通过。崩溃、数据丢失、核心流程失败、依赖缺失和不安全升级仍须修复。成功发布并实际验证 v1.0.0 后按最新要求清理旧 Release，历史 Tags/Git 提交保留；此前保留回退资产。
- 常规修复、编译、测试自主进行；采用 Conventional Commits。
- 用户素材只读，不提交真实 bag、视频或敏感实验几何截图。
- 优先处理主题可读性、内联设置、真实 RViz 属性模型、Topic/TF 选择与 .rviz 兼容。
- 不得把硬编码属性样例、未接收数据、缺少 TF 的 Display 声称为完整原生插件或 OK。
- v1核心生态使用现有真实数据完成必要Display/属性/.rviz/TF与多视角代表流程；缺少原生三生态源、逐插件/属性穷尽对照及边界兼容保持未验并公开限制，按最新核心范围安排后续，不得宣称全插件兼容或把派生消息当原始规划输出。多视角、自由画布、导出、完整安装包与安全覆盖升级的必要核心流程仍须验证。
- 图片导出复用共享渲染和时间同步；不得放大窗口截图冒充高分辨率或位图冒充矢量。
- 当前画布、选中视角/图层、各视角、批量关键帧及科研多联图均纳入验收。
- Video Editor 与 Trajectory Studio 共用 C++/Qt 画布、图层、变换、选择、Inspector、外观、撤销、吸附及渲染导出；工作区实例和编辑状态独立保存。
- 拖影背景、真实无人机前景、轨迹线及起终点应可独立编辑；样式变化复用提取缓存，不重新执行前景提取。
- 默认空白 Grid/Axes 场景，不自动载入模拟；无数据同样可编辑 Display/属性/布局。数据源变更不得无故清空显示、相机与画布。
- Windows 独立 C++/Qt 基础功能不强制依赖 WSL/ROS/Python；原生 RViz 为自动管理的可选增强，保留真实差异矩阵。
- 按用户2026-11-28最新调整，干净Windows安装/运行由用户正式发布后手测，不再作为v1发行前置门槛，也不在README单列；不再为此寻找或准备VM/环境。移交不代表本机验证通过，未测证据字段保持false。安装包依赖完整、当前包本机运行及安全同路径覆盖升级仍是硬要求。
