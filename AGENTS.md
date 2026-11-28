# UAV Studio 开发约束

完整需求见 `docs/REBUILD_REQUIREMENTS_CN.md`；增量科研与品牌需求见
`docs/INCREMENTAL_RESEARCH_REQUIREMENTS_CN.md`。后者补充前者，不替换已完成工作。
实测 UI 修正见 `docs/UI_INTERACTION_FEEDBACK_CN.md`，独立画面导出见
`docs/FRAME_EXPORT_REQUIREMENTS_CN.md`；发生 UI 冲突时以最新实测反馈为准。
统一自由画布与科研制图增量见 `docs/UNIFIED_CANVAS_REQUIREMENTS_CN.md`。
四工作区专业职责、无数据启动和可选后端增量见 `docs/WORKSPACE_AND_EMPTY_SCENE_REQUIREMENTS_CN.md`。
本次正式发行范围以最新 `docs/V1_CORE_RELEASE_SCOPE_CN.md` 为准；高级需求保留在 Roadmap，后续独立 PR。

- 应用运行使用 C++ / Qt；保留旧功能与兼容项目，不引入 Python 运行依赖。
- 使用统一外观管理器及语义 Design Tokens；导出配色独立于应用主题。
- 保持四工作区及用户提供的两份 SVG 参考布局。
- 优先稳定数据、时间同步、渲染和自由画布，再实现依赖模块。
- P0 / P1 / P2 是顺序，不是删减范围；全部需求进入验收矩阵。
- 科研指标必须有真实来源、计算定义、单位、区间和对齐方式。
- 不伪造测试、性能、科研指标或截图；未验证和缺失能力明确记录。
- v1.0.0 核心及基础缺陷全部实际验收前不发布稳定版；高级科研需求不阻塞本次发行，保留必要回退发行版。
- 常规修复、编译、测试自主进行；采用 Conventional Commits。
- 用户素材只读，不提交真实 bag、视频或敏感实验几何截图。
- 优先处理主题可读性、内联设置、真实 RViz 属性模型、Topic/TF 选择与 .rviz 兼容。
- 不得把硬编码属性样例、未接收数据、缺少 TF 的 Display 声称为完整原生插件或 OK。
- 图片导出复用共享渲染和时间同步；不得放大窗口截图冒充高分辨率或位图冒充矢量。
- 当前画布、选中视角/图层、各视角、批量关键帧及科研多联图均纳入验收。
- Video Editor 与 Trajectory Studio 共用 C++/Qt 画布、图层、变换、选择、Inspector、外观、撤销、吸附及渲染导出；工作区实例和编辑状态独立保存。
- 拖影背景、真实无人机前景、轨迹线及起终点应可独立编辑；样式变化复用提取缓存，不重新执行前景提取。
- 默认空白 Grid/Axes 场景，不自动载入模拟；无数据同样可编辑 Display/属性/布局。数据源变更不得无故清空显示、相机与画布。
- Windows 独立 C++/Qt 基础功能不强制依赖 WSL/ROS/Python；原生 RViz 为自动管理的可选增强，保留真实差异矩阵及干净 Windows 安装门禁。
