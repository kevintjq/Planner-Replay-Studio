# UAV Studio 品牌与安装迁移

产品名称为 **UAV Studio — All-in-One Robotics Research Studio**，中文为
**UAV Studio 无人机科研工作站**。CMake 目标使用 `UAVStudio`，程序文件为
`uav-studio.exe`，Windows 安装位置默认为当前用户的 `Programs/UAV Studio`。
新项目后缀为 `.uavstudio`，已有 `.slamfpv` 保持兼容；后缀不能替代项目内容版本校验。

## 安装入口与兼容

Windows 安装器为当前用户安装，不要求管理员权限。开始菜单入口始终创建，桌面快捷方式
继续由用户勾选，静默安装可用 `/DESKTOPSHORTCUT=1` 或 `0`。快捷方式显式使用安装程序
文件内的专属图标，并保留正确工作目录。安装器与卸载器同样嵌入该图标。

安装器为 `.uavstudio`、`.slamfpv` 注册 `UAVStudio.Project` 和 Open With 入口；已有默认
应用保持，新安装不会抢占默认程序。打开命令分别引用带引号的程序路径和 `"%1"` 参数，
支持含空格或中文的项目路径。Windows 的用户默认程序选择仍以系统设置为准。

卸载只删除属于该安装路径的 ProgID、Open With 及快捷方式，不删除用户项目、设置或
其他应用的默认关联。原先单独安装的旧版保留；本轮不删除历史发行版或旧安装目录。
Linux 的 MIME 定义已经包含两个后缀，桌面入口使用 `uav-studio %f`。

## 可复现检查

`native/tools/new-brand-installer-check.ps1` 编译独立测试安装器，使用随机产品 ID、工作区
内安装/快捷方式目录和专用测试注册表根；不改生产文件关联。调用示例：

```powershell
./native/tools/new-brand-installer-check.ps1 `
  -NsisCompiler ./build-native/deps/nsis-3.0.4.1/Bin/makensis.exe `
  -ApplicationExecutable ./build-native/current/Release/uav-studio.exe
```

报告保存在新建的 `captures/branding-installer-*`。本轮实际检查安装、重装、快捷方式的
目标/图标/工作目录、桌面选项、双后缀、保留已有默认、旧安装卸载时保留新安装入口、
卸载清理，共 35 项通过。
验收期间发现 NSIS `/ifempty` 不足以保护只含值的注册表键，现改为枚举子键和注册表值
后清理；实测确认旧关联在卸载后保留。

这只是安装器集成夹具：只复制待检程序，不包含运行依赖，不作为可运行体验发行版。
完整安装包仍需实际核验 Qt/OpenCV/FFmpeg 部署、双后缀打开、GUI 和完整工作流。

`native/tools/new-brand-icon-check.ps1` 使用 Windows Shell 从真实应用和安装器提取 16/32
像素图标，并核对全部八种 PNG 尺寸。本轮应用与安装器均找到一组图标，实际提取成功；
资源中 ICO 有六种尺寸，ICNS 头部长度与文件长度一致。已查看 256 像素图标和从安装器
提取的 32 像素图标；桌面、任务栏及 macOS 安装包的运行外观尚未据此认证。

## 完整开发安装包

`native/installer/package.ps1` 保持 CI 默认参数，同时可显式指定 `QtDirectory`、
`QtLicenseDirectory`、`VcpkgDirectory`、`YamlCppLicenseFile` 和 `NsisCompiler`，
适应本地手工构建与 vcpkg 混合安装的依赖布局。`Version` 保持数字资源版本，
`DisplayVersion` 可使用开发预览标签，避免把未验收构建标为正式发行版。
脚本校验共享 FFmpeg 开发包、主程序与 CLI、TIFF 插件和依赖许可；Qt 部署后重新放入
编译时选用的 FFmpeg DLL，保留原始构建 README、二进制版本与 buildconf。
OpenCV 安装树中的第三方许可也随包保留。

可选参数 `NativeRvizHostBinary` 显式指定真正的 Linux x86_64 ELF；未传入时使用 Standalone
载荷，不搜索本地源码或构建目录。显式选择后复制到 `app/rviz_host`，包含相邻
`run_host.sh`、英文/中文说明、MIT 许可、CMake、`main.cpp` 和 `bag_replay` 头/源文件。
打包前验证 ELF64、小端、x86_64 和可执行类型，拒绝 Windows PE；`backend.json` 记录
实际二进制 SHA-256 与外部运行要求。Bash 脚本统一 LF/UTF-8 无 BOM，优先定位相邻 ELF，
不要求用户机器存在原源码工作区。没有宿主时明确记录未包含，不伪装成已部署原生后端。

该宿主需要另外安装兼容 Ubuntu 20.04、ROS Noetic/RViz、Qt5、OGRE/OpenGL 环境，
Windows 自动检测适用的 WSL 发行版，底层覆盖参数仅位于高级设置；它不是 Windows 原生插件包，也没有包含整套
ROS。开发验收 Python 脚本不进入该运行载荷。完整包验证另外检查宿主、启动器、重建资料、
ELF/哈希、外部依赖标记和换行；静态部署检查不等于外部 ROS 环境已通过运行验收。

完整载荷使用 `native/tools/new-package-runtime-check.ps1` 验证，独立于前面的安装器
小夹具。该工具编译同一载荷的专用隔离安装器，使用随机产品和注册表根，不改用户
默认关联、真实桌面或开始菜单；逐文件比较安装前后 SHA-256，再将 PATH 限制为
安装应用目录与 Windows 系统目录，清除 Qt 插件/导入覆盖，实际运行 CLI、QML 窗口、
透明 PNG、H.264/AAC 导出、OpenGL 项目持久化及可选的真实源视频解码。
最后卸载隔离副本，保留输出 PNG、截图、日志与 JSON；它不代表手动 GUI 全流程验收。

```powershell
./native/tools/new-package-runtime-check.ps1 `
  -AppDirectory ./captures/development-package-current/app `
  -NsisCompiler ./build-native/deps/nsis-3.0.4.1/Bin/makensis.exe `
  -VideoFile 'D:/read-only-experiment/source.mp4'
```

`captures/development-package-current` 为本轮中间构建的完整开发包，不发布到 GitHub。
首轮载荷为 1,447 个文件、297,845,038 字节，安装器为 87,855,816 字节；
`captures/package-runtime-current/runtime-check.json` 的 31 项实际检查均通过，
全部安装文件哈希一致，最小 PATH 下真实 HEVC 解码和各项导出成功。
持久化 PNG 实际解码为 1920×1080；布局截图已打开查看。该轮检查的 EXE SHA-256 为
`CE4DD19DCC051FBDA1B953E8D048434F7969A87450AF20BD2E06F7DB14A94B9E`。
主线集成继续变化时，最终体验版必须重新同步编译产物并完成对应验证，不能用旧安装包
的成功结果证明新代码已经验收。

## 仓库迁移及本地目录状态

2026-11-27 使用现有账号的管理员权限，将同一个 GitHub 仓库原地重命名为
[`kevintjq/uav-studio`](https://github.com/kevintjq/uav-studio)，没有删除或新建仓库。
Description 精确设置为原需求全文：

> UAV Studio — All-in-One Robotics Research Studio. A C++/Qt toolkit for ROS visualization, UAV flight analysis, scientific video editing, and publication-quality figures.

稳定仓库 ID `1471925949`、Node ID `R_kgDOV7vOvQ` 保持不变。迁移前后 2 个分支的
名称/提交/保护状态、10 个标签的名称/提交、3 个发行版的 ID/名称/标签/预发布状态/时间，
以及附件的 ID/名称/尺寸/digest 均一致；Issue/PR 基线为 0。默认分支仍为 `main`，
迁移时提交仍为 `69f8a4f`。未改发行版内容，也未发布或删除发行版。

`origin` 已更新为 `https://github.com/kevintjq/uav-studio.git`，新地址的 `git ls-remote`
成功；迁移保留检查共 9 项通过。原始前后基线与报告在本地忽略目录
`captures/branding-repository-current/{before,after,migration-check}.json`，Git 引用在
`ls-remote-after.txt`。仓库未启用 GitHub Pages，工作区没有 `.gitmodules`；Actions 的
仓库目标使用 `GITHUB_REPOSITORY`，不写死旧地址。

README、克隆命令、Actions、历史下载与指南链接已改为新仓库地址；下载锁定具体历史
标签，避免把旧工作台误认为最新原生重构。旧发行版名称、安装产物、Python 分发名及
兼容模块继续保留，历史截图明确标注，不能盲目替换。

本地工作区仍为 `Planner-Replay-Studio`，本轮没有移动；现有构建和 WSL 仍在使用该路径。
待这些进程退出并保存改动后，再迁移目录并重新配置 CMake；绝对路径构建缓存不能
直接作为移动后构建成功的证据。应用设置已有品牌与旧偏好迁移，完整设置、缓存、日志
迁移及完整手动安装 GUI 验收仍需逐项核实。

### 本地目录迁移实施顺序（尚未执行）

当前只有一个普通 Git 工作区，`.git` 为目录；目标同级目录 `uav-studio` 尚不存在。
为保留正在进行的 GUI、Windows 与 WSL 构建及并行修改，不能在进程仍使用旧目录时改名。
迁移按以下顺序执行：

1. 完成当前验收、保存修改与证据，记录 HEAD、工作区状态及 remotes；保留忽略目录中的
   依赖、构建和私有验收素材，不提交用户原始素材。
2. 停止应用、构建、测试和 WSL 宿主，结束仍以旧目录为工作目录的命令会话，并协调所有
   代理退出该目录。从同级父目录的新会话将现有目录原地改名，保留同一 `.git` 及历史。
3. 重新打开项目并更新工作目录。旧构建只保留为迁移前证据；在新根目录创建新的 CMake
   构建目录，不能直接复用含绝对路径的 `CMakeCache.txt`。
4. 重新定位 Qt、OpenCV、yaml-cpp、vcpkg 和 FFmpeg。检查依赖安装导出及 WSL 缓存中的
   旧绝对路径，必要时重新构建相应依赖；不能盲目替换二进制或缓存中的路径字符串。
5. 重新构建 Windows 应用和 Linux RViz 宿主，核对 Git 身份与 remotes、项目相对素材路径、
   相邻 ELF 启动器及完整打包流程，再记录迁移后结果。

不使用目录联接掩盖旧绝对路径。上述步骤是协调实施方案，不能作为本地迁移或迁移后
构建已经完成的证据。
