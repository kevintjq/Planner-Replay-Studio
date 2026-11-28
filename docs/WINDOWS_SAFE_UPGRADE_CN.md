# Windows 安全覆盖升级

本文件记录 2026-11-28 的安装事务及隔离验证，分别列出事务fixture、最终完整载荷运行和完整载荷升级；这些结果不代表任意旧版、平台或手动GUI组合均通过。

## 使用与文件保全

同一产品标识、同一可信安装路径可直接安装新版本，无需先卸载。安装时应先退出 UAV Studio；安装器不会强制终止应用。遇到占用、来源无法确认或载荷校验失败时，安装器显示原因并停止。

安装前按内嵌清单检查暂存及完整备份的最长路径，超过 Windows 安装工具的 259 字符安全范围时要求选择较短目录；已有额外文件的备份路径也一起检查。此检查在提取前完成，不会先生成缺失资源的半成品安装。

默认程序目录仍为 `%LOCALAPPDATA%\Programs\UAV Studio`，实际运行文件位于其中的 `app`。升级不会将新文件直接覆盖进旧 `app`，而是执行以下步骤：

1. 检查旧安装的产品标识、当前用户卸载注册、精确安装路径、程序及所有权清单。任意占用目录不能凭文件夹名称冒充本产品。
2. 在同一安装目录下的 `.uav-staging-<随机标识>` 解包完整新载荷及新卸载器。
3. 校验内嵌清单本身的 SHA-256，并逐文件核对相对路径、完整文件集合、字节数及 SHA-256；拒绝重解析点、缺失文件、额外文件及错误的 x64 程序。
4. 再检查一次旧安装是否仍可替换，将整个旧 `app` 和卸载器移至 `previous-install-<随机标识>`，再把整个新 `app` 和卸载器移入运行位置。
5. 成功后更新快捷方式、文件打开注册及卸载信息。`last-install-transaction.json` 记录本次暂存及旧版本备份路径；该辅助报告无法写入时会明确提示，但不会把已完成的程序替换报告为失败。

旧 codec、旧可选后端及用户加进旧 `app` 的额外文件都保留在备份中，不会混进新运行目录。安装根目录中的项目、编辑记录及用户额外文件保持原位；安装事务不遍历或清理素材与用户设置目录。安装器不会自动删除旧版本备份。

发生文件替换异常时，已完成的目录移动会按相反顺序尽力恢复；恢复遇阻时保留对应位置并报告路径。这是同步异常恢复，不是断电、进程崩溃后的完整自动回滚系统。失败的暂存内容保留用于排查。首次安装解包失败后，非空目标目录不会被下一次安装自动信任或删除，可选择新的空目录。

卸载当前程序不会递归删除位于安装根目录的旧版本备份。常规卸载仍会删除当前 `app`；个人项目和素材应保存在工作目录，不能把程序载荷目录当作项目存储位置。

## 旧版识别边界

含 `uav-studio-install.json` 的旧安装必须具有同一 InstallId、名称和程序名，当前主程序的哈希也必须符合其清单。旧安装中额外的用户文件允许保留，但不会作为新载荷使用。

没有清单的 0.1.x 预览版仅在以下证据同时成立时才允许升级：卸载注册精确指向当前根目录；名称和卸载命令匹配；本产品项目关联的打开命令精确指向当前程序；GUI 和 CLI 都是 x64 PE；`native-rviz-backend.json` 与 `windows-runtime.json` 含彼此一致的已知产品 profile 和独立 Windows 运行声明。只有注册项或只有同名程序都不够。

这些检查确认本地安装所有权与载荷完整性，不把普通 SHA-256 描述为签名认证。全部历史发行包尚未逐一安装验证；不能据此声称任意旧版都已验收。完整崩溃恢复、断电恢复及同用户恶意竞争不在本次已验证范围内。

## 构建契约

正式打包从根目录 `app-metadata.json` 读取名称、版本、作者、邮箱及 GitHub 地址。NSIS 的这些宏必须由打包入口传入，不再保留另一套产品默认值。正常打包还会核对 GUI、CLI 的 PE 文件版本、产品名称与作者，避免给旧程序贴上新版本号；隔离版本 fixture 可显式使用 `-AllowVersionOverride`。

`Write-WindowsInstallManifest` 为完整已部署载荷创建所有权清单。NSIS 要求编译时传入清单 SHA-256；完整运行包隔离检查可以传入独立 fixture 清单，不修改原始打包目录中的生产身份。Linux RViz 后端仍按独立 profile 显式包含，默认包不包含该后端。

事务助手使用 Windows 自带的 Windows PowerShell 5.1 与 .NET Framework SHA-256，兼容 NSIS 的 32 位宿主，不依赖额外模块。该脚本仅由安装器调用；C++/Qt 应用运行不依赖 PowerShell、Python、ROS 或 WSL。系统策略禁止执行安装事务助手时，安装会停止，不跳过验证直接覆盖文件。

## 实际验证

隔离测试全部使用随机 InstallId、专用 HKCU 测试命名空间、仓库内安装目录和本地快捷方式；不操作用户真实安装或真实扩展名关联。fixture 仅测试安装事务，不携带完整运行依赖，也不执行所复制的程序。

| 验证项 | 实际范围 |
| --- | --- |
| 同路径 0.1.0 → 0.1.1 | 使用两套不同哈希的 x64 fixture；新目录与清单逐文件一致，整个旧目录及卸载器逐字节保留 |
| 旧 codec / 可选后端 / 用户额外文件 | 仅出现在旧版备份，不混入新运行目录 |
| 任意占用目录 / 不同产品标识 | 拒绝，原文件逐字节不变 |
| 被锁定的旧程序 | 明确拒绝，不强制结束进程，不修改旧载荷 |
| 可信无清单旧版 / 缺少历史识别文件 | 前者可升级并完整备份；证据不足者拒绝 |
| 新载荷被篡改 | 提交前拒绝，旧程序及卸载器不变 |
| 自定义目录或旧额外文件的备份路径过长 | 提取前拒绝；全新目录不创建暂存载荷，已有安装逐字节保留 |
| 中途文件移动失败 | 通过锁定新卸载器触发；反向移动恢复旧程序及卸载器，保留完整新暂存载荷 |
| 独立 fixture 清单覆盖 | 安装器使用指定外部清单；原生产载荷清单哈希不变 |
| 安装器图标 / 快捷方式 / 注册 | 使用独立 branding fixture 检查；较早安装的卸载器不移除较新安装的注册和快捷方式 |
| 暂存清单及静态打包检查 | 验证确定性、路径/字节数/哈希、重解析点拒绝、脚本解析及 CMake/QML 完整性 |

测试入口为 `native/tools/test-windows-upgrade.ps1`、`new-brand-installer-check.ps1`、`test-windows-staging-input.ps1` 和 `check-windows-packaging.ps1`。本地报告保存在 `captures/`，不随源码提交。完整真实发行包的本机安装后运行、真实 GUI 和安全覆盖升级应继续按总验收矩阵执行，不由上述 fixture 替代。按2026-11-28最新用户安排，干净Windows安装/运行由用户正式发布后手测，不再作为v1发行前置门槛；移交不等于本机已通过，完整依赖与安全升级仍是硬要求。

2026-11-28 已收集的本地结果如下，全部 `success=true`，检查项均通过：

| 本地报告 | 通过项 |
| --- | ---: |
| `captures/windows-upgrade-20261128-release-check/report.json` | 38 |
| `captures/branding-upgrade-20261128-frozen/installer-check.json` | 44 |
| `captures/windows-staging-manifest-20261128-final-pass/report.json` | 31 |
| `captures/windows-packaging-static-upgrade-20261128-frozen/report.json` | 36 |
| `captures/windows-package-metadata-20261128/report.json` | 3 |
| `captures/upgrade-pathguard-20261128/report.json` | 43 |
| `captures/branding-pathguard-20261128/installer-check.json` | 44 |
| `captures/packaging-pathguard-20261128/report.json` | 36 |
| `captures/documentation-pathguard-20261128/report.json` | 44 |

另已本地解析 Windows 工作流 YAML，并通过其三段内嵌 PowerShell 的语法解析。GitHub 托管 CI 未在本步骤重新运行。

完整运行包检查入口为 `native/tools/new-package-runtime-check.ps1`，使用完整载荷和独立 fixture 清单。`-Offscreen` 仅验证离屏可用部分：安装与文件哈希、解码及编辑、软件透明图片、H.264/AAC 和卸载等；Windows Qt 离屏插件不提供所需 OpenGL，CLI 三维 PNG 门禁明确跳过，报告的 `runnable_distribution_verified` 与 `opengl_output_verified` 保持 `false`。三维输出必须另用 Windows 原生窗口检查，不能以解码成功或软件布局图替代。最小 PATH 及清理环境变量也不等于已移除开发机上的 WSL/ROS/Python，不能代替干净 Windows 系统验收。

最终1.0.0载荷的后续本机原生窗口检查已通过：`captures/rtv1final-native-window/runtime-check.json`
为57/57，1481个安装文件SHA匹配，实际透明PNG、H.264/AAC、真实视频解码编辑、CLI OpenGL三维PNG及卸载成功，
`runnable_distribution_verified`与`opengl_output_verified`均为`true`。它用同一冻结载荷和NSIS源码，
但采用隔离产品身份/注册/快捷方式，不操作用户生产安装。安全同路径升级的当前43/43报告为
`captures/uga1fc3/report.json`，仍属于上述事务fixture。
最终安装器哈希和应用版本见[实际验收记录](VALIDATION.md#final-windows-100-payload-installation-and-runtime-check)。

同日晚另完成完整载荷同路径升级36/36：`captures/uf2e8e/full-payload-upgrade.json`，
SHA-256 `667009DBFFA4E410E012D35AB43BFA2AEA8ED8D763199BE4CE2E76892FBF6389`。
使用已有真实0.1.1开发版A1FC二进制及1469文件完整载荷，升级到最终1.0的567A二进制及1481文件完整载荷，
不是只更换版本标签。测试安装器仅改隔离产品身份/注册/快捷方式，采用同一当前NSIS源码和真实冻结载荷。
新旧安装均逐文件SHA匹配；整个旧app、测试附加文件及旧卸载器备份逐字节不变，旧附加文件未混入新app；
项目、配置及编辑历史哨兵在升级与卸载后均保留。升级后以最小PATH、清理外部Qt/ROS/Python环境，
Normal窗口运行布局检查成功并生成可解码980×640 PNG；此布局检查不另认证OpenGL。
最终卸载仅清除隔离活动程序/注册/快捷方式，保留备份与用户哨兵；生产注册、关联及真实应用设置摘要前后不变。
这补齐一次完整实际历史载荷成功升级路径，不外推所有旧版或断电/崩溃恢复。
