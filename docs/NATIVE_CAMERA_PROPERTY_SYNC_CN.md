# 原生相机属性与准确实验历史隔离

2026-11-28，宿主 SHA256：
`8C87BB91B70B715D16511B5FDF91CBF250313E6F4DE506D9995DB426F5BC1A29`。
开发启动器为 `build-native/property-sync-launcher.sh`；实际 ELF 为
`build-native/rviz-host-property-sync/uav-studio-rviz-host`。文件名相同不代表版本相同。

## 修复范围

- `set` 按真实 Property 祖先识别主/附加 ViewController，不能由属性名或请求 view_id 冒认。
- 合法相机属性及主控制器类型切换只改变相机和配置版本，不取消已经恢复的消息历史。
- 所有类型/枚举/原生浮点范围校验在成功修改之前完成；失败 set 不清同步或配置版本。
- 共享数据就绪继续校验真实消息投递、Display Status、Fixed Frame 和数据 TF；每个相机再独立校验 Target Frame。
- 客户端分别保存共享数据同步和主视角就绪；同一准确时间的 Inspector 补 seek 只刷新预览。缺 TF 的对应视角保持未就绪且拒绝输出，其他有效视角仍能输出。
- 合法 Display Topic 修改继续使准确历史失效；不会为相机编辑启动 `rosbag play` 兜底。

## 本版本实测

`captures/rviz-camera-property-sync-8c87-20261128/report.json` 的 Qt6 客户端真实录包专项退出 0，
20 项编辑/切换涵盖主和附加 TopDown 的 Scale 19.25、Angle 0.375、有效/缺失/恢复 Target Frame，
Orbit Distance、同步后 Orbit/TopDown 切换、错误数字/枚举/Topic 类型以及合法 Topic 失效。
准确时钟/目标始终为 `1768571820557415724 ns`；相机编辑没有额外 open/index/seek/advance/play，
场景、Display、源身份及对应存活属性身份和资源保持。相机类型切换会正常创建新控制器属性身份。

9 张真实 1920×1080 PNG 已全部实际查看；TopDown 有点云，但部分构图裁切，主 Orbit 的点云较小，
`orbit-distance.png` 为纯黑。该组只认证时间/状态隔离与输出门禁，**不认证所有保存相机姿态的几何可见性或最终构图**。
逐文件哈希、尺寸、目检说明和源码哈希位于同目录 `provenance.json`。主应用 GUI 仍是独立门禁。

`captures/rviz-native-boundaries-8c87-20261128/report.json` 直接协议专项退出 0：在已同步后请求
不存在的主控制器、输入 `1e300`，均被拒绝，原值、配置版本、时钟和就绪历史保持，只有初次 index/seek。

## 保留的失败与历史证据

- 5DA1 宿主红测在首次同步后修改 Scale，失去新的同步预览；日志
  `build-native/rviz-camera-property-red-5da1.log`。原始 5DA1 ELF 和插件矩阵证据保留。
- 0DD4 的两个早期绿色专项在结尾误选只读 `StatusProperty(Topic)`，属于测试选择错误，失败日志与图片保留；
  排除 StatusProperty 后，同版本 16 项/7 图专项退出 0，见 `captures/rviz-camera-property-sync-0dd4-final-20261128/`。
- `captures/rviz-native-scalar-0dd4-20261128/` 记录 0DD4 原生 FloatProperty 把 `1e300` 钳到 FLT_MAX 后触发
  OGRE AxisAlignedBox 断言崩溃；宿主无 shutdown 回复导致探针退出 1。8C87 在修改前拒绝该输入。
- 历史5DA1的33类结构/547个真实属性及五类七图仍保留原归属。8C87已另行独立复验33类
  （32个默认插件及 `rviz_plugin_tutorials/Imu`）结构、547个属性及五类七图，见
  `RVIZ_COMPATIBILITY_MATRIX_CN.md`；这仍不代表33类全部真实数据、渲染或主GUI认证。

本修复不声称任意第三方插件已验收，不放宽缺数据/缺 TF 门禁，不授权正式发行。
