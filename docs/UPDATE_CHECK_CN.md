# 异步更新检测与提醒

更新：2026-11-28。本文仅记录已实现及实际取得的组件证据，不表示 v1.0.0 已发布或主 GUI 已完成验收。

## 实现范围

`native/src/update_manager.cpp/.h` 为 C++/Qt 后端，使用 `AppInfo::metadata()` 提供的
`version` 和 `repositoryFullName`。不另设产品版本或仓库常量。当前产品仍为
`0.1.1 · development`，不会为了更新界面提前改为 1.0.0。

- `QNetworkAccessManager` 异步读取本仓库 Releases 列表，每页 100 条；按 GitHub `Link` 逐页读取，
  完整检查后才发布结果。只接受严格 `vX.Y.Z`、无前导零、非 draft、非 prerelease 的正式标签；
  `studio-*`、`native-*`、缺少 `v`、预发行和 build 后缀均不进入当前稳定通道。
- 按三个十进制分量比较版本，支持超过机器整数范围的合法分量；不按字符串、发布时间或 API 返回顺序选择。
  空正式发行列表显示“尚无已发布的 UAV Studio 稳定版”，不冒称已有稳定发行。
- 构造不联网。普通 GUI 启动调用 `startAutomaticCheck()`，默认延迟 2 秒且幂等；
  测试、批处理及验收入口由主程序跳过此调用。`automaticCheck` 默认开启，可持久关闭。
  手动检查覆盖自动提醒抑制，多个并发检查共用一个请求；手动检查取消尚未开始的重复启动检查。
- `skipVersion()` 持久跳过该版本的自动通知；后续新版本仍可提醒。
  `remindLater()` 持久抑制该版本 24 小时。二者只隐藏通知，保留 `updateAvailable`、版本及说明供 About 查看。
- 整次检测的默认总期限为 15 秒，覆盖所有分页；同时设置 Qt 请求超时。
  断网、HTTP 错误、GitHub 限流、无效响应及超时终止 busy 状态并暴露中英错误说明。
  已发现的可信版本信息在下一次检查失败时仍保留，但不弹出更新通知。
- 记录最近完成检查的 UTC 时间 `lastChecked`，包括失败检查；与自动偏好及提醒状态一起使用 QSettings 保存。
  不修改项目、播放头、数据源或编辑状态。

## 来源边界与后续下载

请求固定使用配置仓库的 `https://api.github.com/repos/{owner}/{repo}/releases`，
`Accept: application/vnd.github+json`、`X-GitHub-Api-Version: 2022-11-28`。
使用 `ManualRedirectPolicy` 拒绝重定向，不携带或保存 Cookie。
下一页只能是同仓库 HTTPS API 的精确下一页，不能转移主机、仓库或跳过分页。
最多 100 页；每页 8 MiB、整次 32 MiB，超过边界作为检查失败，不输出不完整的“最新”结论。

`html_url` 必须与元信息仓库和选定标签的 HTTPS Release URL 完全一致。
`openReleasePage()` 只打开已验证的正式 Release 页；尚无选定版本时打开同仓库 Releases 列表。
更新说明按原文传给 UI，About 必须以纯文本显示；不会执行远端 HTML 或脚本。

本轮按 `RELEASE_UPDATE_REQUIREMENTS_CN.md` 允许的范围提供可信网页下载入口。
**应用内下载安装、下载进度、资产 SHA256 验证、保存退出后调用安装器仍为后续独立 PR。**
更新后端不下载安装包、不自行退出、不启动安装器，也没有声称已完成升级安装验收。
安全同路径覆盖安装由安装器专项实现并独立验收。

接口与 GitHub 协议依据：

- [GitHub REST Releases](https://docs.github.com/en/rest/releases/releases?apiVersion=2022-11-28)
- [Qt QNetworkRequest](https://doc.qt.io/qt-6/qnetworkrequest.html)

## 集成接口

构造为 `UpdateManager(const QVariantMap &, QObject *parent = nullptr)`；QML context 名为 `updater`。
只读属性为 `status`、`busy`、`updateAvailable`、`latestVersion`、`releaseNotes`、`releaseUrl`、
`notificationVisible`、`errorString`、`lastChecked`；`automaticCheck` 可读写。
方法为 `checkForUpdates(bool manual = true)`、`startAutomaticCheck()`、`skipVersion()`、
`remindLater()` 和 `openReleasePage()`。

后端依赖 Qt6::Gui 和 Qt6::Network，不依赖 Python、WSL、ROS 或 QtTest。
用于离线测试的 `Services` 重载可借用 QNetworkAccessManager、QSettings、时钟及测试超时，
不改变生产的两参数构造。借用对象的生命周期必须长于 UpdateManager。

## 已实际执行的组件测试

2026-11-28 在本机 Windows / MSVC 19.42 / Qt 6.8.3 Release 实际编译并运行
`native/tests/update_manager_test.cpp`：**113 assertions，0 failures，退出码 0**。
网络层全部为明确注入的离线 fixture，未向外网发送请求，也未伪造真实新 Release。

覆盖：

- 混合旧产品、draft、prerelease、缺失/错误标记、尾换行和非法语义标签过滤；
  跨分页乱序选择 `v1.10.0` 大于 `v1.9.0`，超大十进制分量不溢出。
- 同仓库 URL、API 请求头、拒绝重定向、外部/跳页 Link、无效 JSON、非列表响应和 8 MiB 超限拒绝。
- skip、新版本恢复通知、24 小时稍后、手动覆盖抑制、manager 重建读取设置；
  实际 INI 文件中自动偏好和提醒状态存在，最近完成检查时间恢复。
- 自动延迟与幂等、关闭自动、不重复手动/自动请求、网络等待期间 Qt 事件 heartbeat 继续运行。
- HTTP 403、断网、总超时 abort、超时后重试、跨分页总期限、失败不发布局部候选、析构取消请求。
- 用 `QDesktopServices` URL handler 捕获并核对 Releases 及选定 Release 网页入口，未实际启动浏览器。

证据：`build-native/update-manager-check/offline-test.log`。
隔离可执行文件：`build-native/update-manager-check/build/Release/update-manager-test.exe`。
运行时设置 Qt bin 到 PATH，并设置 `QT_FORCE_STDERR_LOGGING=1` 可保留断言计数日志。

| 文件 | SHA256 |
| --- | --- |
| `native/src/update_manager.cpp` | `0CF937A9E4F0664F105ECAEFA4891EDE812AB4B2CF6A8EAB28ACFCC1B98D292C` |
| `native/src/update_manager.h` | `ED10738A6F8D161F1F7D2184BBB0A79F8C9AE880DED046A3BCAEF860FE64909F` |
| `native/tests/update_manager_test.cpp` | `18936121771E61F28381EE70432A805EEE62C8498F97884ABC641F55F3E5F0F4` |
| 隔离 Release 测试 EXE | `81C1D4789B82E806F230CCBCACF0E02E557032F1119DB14E36097CFFBF9ACC2D` |

尚待主程序验收：主构建与 About 绑定、真实 GitHub 连通性和真实版本结果、双主题通知/说明可读性、
实际应用退出重启后的偏好、编辑/播放过程中手动检查，以及当前最终包本机安装运行和覆盖升级。
干净Windows安装/运行按最新用户安排交由正式发布后的用户手测，不再作为v1前置门槛，也不记为本机通过。
上述组件 heartbeat 不等同完整 GUI 或性能认证。
