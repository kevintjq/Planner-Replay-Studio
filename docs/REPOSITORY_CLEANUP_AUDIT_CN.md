# 仓库清理引用审计

审计日期：2026-11-27。依据 `V1_CORE_RELEASE_SCOPE_CN.md` 的清理门禁，先检查
构建、代码、CI、安装及文档引用，再判断文件是否可删除。本次只审计和更新文档，
没有删除源码、测试、示例、许可、用户素材、构建目录或验收证据。

2026-11-28 政策更新：干净Windows安装/运行已移交用户正式发布后手测，不再作为v1前置门槛，
不在README单列，也不继续寻找或准备VM。下文历史CI字段仍保留未测事实；移交不等于本机通过。
安装包依赖完整、本机最终包实际运行及安全覆盖升级继续按当前核心矩阵执行。

## 结果

- 当前受版本控制文件中未发现已确认无用的生成二进制、日志、临时文件或缓存。
- 已检查跟踪列表及忽略规则；忽略的构建产物与本地媒体不能据此认定为可删除。
- 没有进行删除，因此本记录不宣称完成“清理后的干净构建”。当前发行仍须通过
  核心验收、完整包依赖与安全升级，README 的预览标识及未勾选核心门禁保持有效。

## 保留项与实际引用

| 文件组 | 保留依据 |
| --- | --- |
| `src/planner_replay/`、`pyproject.toml`、`tests/` | `pyproject.toml` 仍声明旧 CLI/GUI 和两个 C++ 扩展；测试实际导入旧模块。保留兼容及回归能力，不进入原生应用运行依赖 |
| `packaging/windows/studio_entry.py`、旧 Windows CI | `.github/workflows/studio-windows.yml` 使用该入口；属于历史应用构建与回退路径 |
| `examples/` | 原生使用文档仍给出 `examples/ros1-native` 的 CMake 构建；其他桥接与外参示例保留可用参考 |
| `native/tests/`、`native/rviz_host/verify_*.py` | 原生 CMake 和各专项验收记录使用这些检查；Python 脚本是开发验证工具，不能据此删除或引入产品 Python 依赖 |
| `native/tools/rviz_config_probe.cpp` | `native/tools/rviz-probe/CMakeLists.txt` 实际构建原生 RViz 配置探针 |
| `native/tools/render_assets.cpp` | `native/CMakeLists.txt` 构建 `UAVStudioAssets`；用于生成品牌图标的各平台格式 |
| `assets/icons/` | SVG 用于 Qt 资源与 Linux；全部 PNG 尺寸用于 Linux 安装；ICO 用于 Windows 程序/安装器，ICNS 用于 macOS 配置。不同尺寸和格式不是重复垃圾 |
| `docs/design/appearance-*-reference.svg` | 用户要求的亮暗布局参考；不能用自动生成或历史截图替代 |
| `docs/native-preview.png`、`docs/studio-preview.png` | `docs/VALIDATION.md` 仍引用，并说明历史/合成来源；不作为当前已验收界面展示 |
| `LICENSE`、`native/installer/THIRD_PARTY.md` | 项目及打包依赖许可；必须保留 |
| `build-native/`、`captures/`、本地媒体 | 分别含活动依赖/构建、验收证据及只读用户素材。均不可作为普通清理目标批量删除 |

## 待确认的本地候选项

根目录 `aqtinstall.log` 为安装日志，已由 `.gitignore` 忽略，本次检查大小 11,523 字节。
它不是仓库跟踪垃圾；本次不删除，并先将候选项报告给主任务。活动安装可能继续写入，
需要清理时须先确认没有依赖安装进程使用它。

审计命令与输出保留在忽略目录 `captures/windows-backend-current/`：
`tracked.out.log`、`repository-all.out.log`、`legacy-references.out.log`、
`asset-references.out.log`、`tool-references.out.log`。其中是本地开发证据，不作为发行素材。

## README 同步核对

`README.md` 和 `README_CN.md` 均采用品牌首屏、概览、功能、四工作区、展示、架构、安装、
快速开始、使用、兼容、Roadmap、许可与致谢的顺序。两份各有23个不同本地链接，目标均存在；
代码围栏及段落标签配对，Roadmap 均为14项未勾选门禁。原生预览及旧完整版下载明确标为
历史构建，未把当前工作区改动声称为已发行。没有添加未经实际验收或含私有几何的截图。

静态核对结果见 `captures/windows-backend-current/readme-verification.json`。
本记录不替代实际 GUI、插件渲染、输出画质、安装或性能验收。

## Windows CI 后端与发布边界核对

2026-11-27 增量核对发现原 Windows 工作流把 `QT_QUICK_BACKEND=software` 与
CLI 三维 PNG 成功导出绑定。CloudView 需要实际 OpenGL 上下文及 FBO；软件后端不能
据此取得有效三维画面。当前工作流改用既有布局、透明二维 PNG、H.264/AAC、项目/视频
状态及 CLI 帮助入口，只记录二维和编解码兼容性。Hosted CTest 精确排除
`movie_gpu_preflight`，保留 GPU 不可用超时及拒绝原生断开/陈旧画面替代的回归。

这不是取消真实 GPU 门禁：同一构建仍须在真实 Windows/OpenGL 运行
`movie_gpu_preflight`，同一最终安装包仍须运行
`native/tools/new-package-runtime-check.ps1`，并完成
[核心发行范围](V1_CORE_RELEASE_SCOPE_CN.md) 的手动 GUI、完整工作流、性能、依赖完整及安全升级。
历史软件CI运行清单将GPU、手动GUI、干净Windows及可验收发行状态记录为未通过；
干净Windows现已移交用户发布后手测，未测字段继续保持false，最小PATH不冒充干净操作系统。

工作流仅上传带源码短 SHA 的 development 安装包和兼容性证据，不再自动创建固定
preview 标签或发布稳定版；未删除或修改现有远端回退发行版。应用哈希在卸载前读取，
报告复用该值。既有 docs/README 触发路径及离线文档回归调用保持有效。

本轮本地实际检查保存在忽略目录
`captures/software-ci-gates-20261127-d00d055b/`：YAML/门禁结构 23/23、工作流
6 段 PowerShell 解析及参数绑定/版本读取共 13/13、离线文档 39/39（16 个文件、
31 个本地链接）、打包静态审计 31/31，通过。没有运行 GitHub runner、应用、安装器、
GUI 或实际 GPU；没有重新构建、打包、提交或发布。上述本地检查不能声称远端 CI 已通过。

## 待提交内容敏感信息核对

忽略目录 `captures/brand-release-readonly-20261127/sensitivity-scan.json` 记录该次
只读快照：177 个候选路径（72 个已跟踪修改、105 个未跟踪），暂存数为 0；候选中没有
媒体/二进制路径，文本启发式未命中私有用户目录、私钥头部及指定凭据形状。仅记录数量和
脱敏命中，不保存秘密内容。这不是完整秘密扫描或私有几何认证，也不意味着后续新增内容
自动通过；提交时仍应明确选择源文件。`captures/`、构建目录、真实 bag 和视频保持忽略。

## 当前交付尚需的实际证据

本节只记录核对时状态，当前结论以
[完整验收矩阵](REBUILD_ACCEPTANCE_CN.md) 为准。最新 r5 已取得主应用原生三相机
自动合成及高分辨率输出证据；不能据此关闭手动随机跳转同步、全部相机/控件交互或
每视角 Display 隔离、原生透明输出等尚有限制的条目。真实无人机当前 601 跟踪、580 非零
alpha、21 失败的结果仍有背景残留及机体孔隙损失，非零 alpha 不等于干净前景验收。

当前源码的最终安装包、全部手动 GUI/参考图对照、完整工作流和
长时性能仍须按矩阵实测。README 继续保留 development preview 与未勾选门禁；中间
安装包和历史组件证据不能替代最终构建。核对时本地目录仍为 Planner-Replay-Studio，
相邻 uav-studio 目录不存在，本地目录/缓存迁移尚未执行。

## 提交边界与全部 Windows 工作流发行保护

2026-11-27 后续审核还发现历史 `studio-windows.yml` 的 main release job 保留了自动
发布/上传和删除旧发行版的命令，不能仅关闭 native 工作流就认为发行受控。现已移除该
旧 release job，保留历史应用回归、构建、安装 smoke 和 artifact；没有调用远端修改操作，
没有删除任何已存在的标签、发行版或资产。两个 Windows 工作流均使用只读仓库权限。

新增 `native/tools/test-release-gates.ps1`，检查当前工作流不存在发行写命令、常见发行
写 action 或仓库写权限；以九组惰性危险文本验证拒绝发布、上传、编辑、删除回退及写权限，
并验证只读查询和 artifact 可用。native CI 实际调用该回归且失败即停止；push/PR 触发路径
覆盖全部 `.github/workflows/**`，防止只改历史工作流时漏过检查。该静态保护不是任意脚本
语言的权限证明；当前验收未全部通过时继续禁止自动产品发布。

两份 README 的 TF/多机位行现已明确后端范围：独立渲染支持跟随滞后/平滑；原生 RViz
使用安装插件实际提供的相机/Target Frame 属性，暂不提供该独立后端控制。未把组件测试、
非零前景 alpha、最小 PATH 或开发 artifact 改述为核心已验收。两份仍各有23个有效本地
链接、14个未勾选交付门禁。

只读提交边界和文件 SHA-256 保存在忽略目录 `captures/core-submit-audit-20261127/`。
按源码依赖划分为137个核心 CMake/C++/QML/测试/原生宿主文件与41个文档/包装/CI门禁
文件；不是按工作区拆开共享模型。HEAD 加核心集合已覆盖主 CMake 的132个明确输入，
这仅证明路径闭合，不是重新编译。两个提交应一起完成后再推送：旧 HEAD CI 未提供新
共享 FFmpeg 开发依赖，第二组还包含打包必需的新文档，不能单独以首提交声称 CI 通过。

最新178个候选文件（72修改、106未跟踪，暂存0）未发现 bag/视频/截图/项目/几何/构建
路径或二进制签名；私有绝对路径、私钥头及指定凭据形状启发式命中0。根 LICENSE 和图标
资源已经受版本控制，不必复制或删除；THIRD_PARTY、FFmpeg构建来源、Qt/OpenCV、压缩
与 YAML 许可拷贝逻辑及可选宿主 MIT/重建源码随第二组保留。此结果不替代最终包逐文件
许可核对或完整秘密/实验几何审查。

本地新增发行保护16/16、包装静态32/32通过；YAML/工作流脚本解析及最终文件哈希另存
同目录。本轮没有编译、运行主程序/GUI、安装、stage、commit、push或改动远端发行版。
GitHub runner、最终安装包及全部核心实际验收结论保持独立。
