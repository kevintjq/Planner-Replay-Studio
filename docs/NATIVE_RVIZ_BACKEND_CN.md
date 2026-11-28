# 原生 RViz 宿主接口

`native/rviz_host` 是 C++ / Qt5 / ROS 1 Noetic / librviz 独立宿主。Qt6 主程序使用 `QProcess`
交换 JSON；不将 Qt5 QObject 或 OGRE 指针传入 Qt6。它复用当前 ROS 环境实际安装的插件、
Property、配置读写及三维渲染，不维护一份示例属性清单。

Qt6 客户端和内联面板使用同一个真实属性模型，支持真实插件发现、动态枚举、属性修改、按话题添加
和原生配置读写；已通过独立客户端集成测试。主画布预览及主视角精确离屏输出已接同一个
NativeRvizClient，具体接线与尚未验收的边界见下文。当前仍不表示全部第三方编辑器、完整
共享多视角、主应用完整导出工作流或 ROS 2 支持已完成。限频 framebuffer 只属于运行预览。

当前冻结宿主为 `8C87BB91B7…`，已独立复验33类插件结构及五类真实录包输出，并完成相机属性/
类型编辑的准确时间同步专项。空机位13项回归及Qt6客户端TopDown删除恢复仍属于历史
`5DA199D702…`；详细范围和版本归属见本文末尾及 `RVIZ_COMPATIBILITY_MATRIX_CN.md`。
这些专项不替代主GUI、全部真实数据、标准RViz对照、Alpha输出或性能认证。

## 构建及启动

在安装 ROS Noetic、RViz、Qt5、yaml-cpp 开发库的 Linux / WSL 环境：

```sh
source /opt/ros/noetic/setup.bash
cmake -S native/rviz_host -B build-native/rviz-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-native/rviz-host -j4
roscore
# 另一个终端。配置参数可省略；必须存在可访问的 ROS master。
build-native/rviz-host/uav-studio-rviz-host /path/to/config.rviz
```

默认不修改 ROS master 的时间模式。离线录包使用专用 master，并显式加 `--sim-time`；此参数
设置该 master 的 `/use_sim_time=true`，请不要在其他实时机器人共用的 master 上启用它。

Qt6 客户端使用幂等、异步的 `ensureStarted(config)` 管理可选后端。Windows 通过
`wsl.exe --list --quiet` 发现实际发行版，逐个运行随包封装的 `--hidden --probe`；兼容其
UTF-16LE 输出。保存的发行版/脚本失效时保留诊断并回退，Linux 直接使用 Bash。probe 核查
真实 ELF、Noetic ABI、RViz、动态库、启动命令和 DISPLAY；实际 OpenGL 在宿主真正启动时验证。
路径参数转换为选中环境可读的路径；原始配置及素材只读，只有明确保存目标会写入。

随附 Bash 启动封装可拥有独立 master，仅清理自己创建的进程组；不操作已有 master 或其他 ROS
进程。端口占用时直接拒绝，不抢占。直接命令默认端口49175，自动客户端使用 `--port auto`
选择未占用端口。应用正常关闭发送 `shutdown`，
封装等待宿主退出后清理 master，诊断日志保存在独立临时目录。

```sh
bash native/rviz_host/run_host.sh --hidden --probe
bash native/rviz_host/run_host.sh --config /path/to/config.rviz --hidden --port auto
```

Windows 用 `QProcess("wsl.exe", {"-d",discoveredDistribution,"--","bash",launcherPath,...})`
直接传参数，避免拼接不可信文件名到 shell 字符串。`--hidden` 及 `window` 命令控制原生窗口显隐；
客户端先发送 `shutdown`；超时后使用独立随机 `--session-token` 调用 `--stop-session`。封装核对
本用户的 PID、内核启动时间和 token 参数，只通知自己的 wrapper，随后清理自有宿主和 master
进程组。正常和 token 强制关闭均实测释放端口与 owner 状态；日志保留，不递归删除用户目录。

### 独立安装包与自动恢复

应用目录的 `native-rviz-backend.json` 是自动发现前的守卫。`included:false` 或
`package_profile:Standalone` 时直接报告 `native_not_packaged`，不枚举 WSL、不读取旧工作区
启动器、不启原生进程。显式包含增强的包使用 `included:true`、`OptionalNativeRviz`；源码
开发无 manifest 可自动发现。高级手动 `start(...)` 保留显式覆盖，不是普通启动前置步骤。
不自动安装 WSL/ROS，不修改系统环境；缺少增强只影响原生插件/属性/渲染，独立 C++ 核心仍可用。

`runtimeState` 为 idle/detecting/starting/connected/reconnecting/unavailable；`runtimeInfo`
保留实际探测诊断、`optional:true`、`independentBackendAvailable:true`、自有客户端 PID 和配置
快照序号。未连接时 `loadConfig`/`loadConfigYaml` 仅排队，不自行启动后端；ready 后先成功恢复
显式/排队项目，再标记 connected。`clearPendingConfig()` 清除旧队列；离线空 YAML 同样清除，
防止新空项目复活旧配置。已连接时的空 YAML 不暗改当前原生场景。

意外失连最多自动重连3次，清除旧预览、索引和同步状态。每次成功编辑响应内包含同一时刻的
完整 `configuration_yaml`；客户端先缓存后通知编辑成功，即便随即失连也可恢复最后确认的
属性、Display UUID、视图与工具。恢复配置后仍需重新索引/定位真实数据。显式 Stop 不重启；
清理仅作用于本客户端的 session token。

没有初始配置时创建 FixedFrame=`map` 的真实 Grid/Axes，未加载 Bag 同样可修改原生属性并
生成真实预览。手动输入尚未发现的 Topic 会通过插件唯一实际 RosTopicProperty 设置并保存，
明确保持等待数据/兼容性未验证；已发现话题仍校验实际类型，歧义或不兼容拒绝。插件没有唯一
适用 Topic 属性时撤销本次添加。无源预览不能标为实验时间同步或数据渲染通过。

## 协议与身份

stdin / stdout 使用 UTF-8 JSON Lines；每个请求一个对象，以换行结尾，最大 4 MiB。
ROS、OGRE 和插件控制台输出统一重定向 stderr，避免破坏 stdout 协议。

```json
{"request_id":1,"command":"tree","include_options":false}
{"request_id":1,"ok":true,"result":{"revision":"4","fixed_frame":"camera_init","displays":{},"views":{},"tools":{}}}
```

错误仍返回同一 `request_id`，`ok:false` 和具体 `error`。启动产生 `event:ready`，模型变化产生
`event:changed` 和递增字符串 `revision`（100 ms 合并通知）。客户端收到通知后请求共享模型，
左右属性面板引用同一模型；不分别维护属性配置。

每个属性包含本进程内唯一字符串 `id`；编辑通过 `property_id` 定位。属性销毁后 ID 失效，
不能因内存地址复用而误编辑新对象。`path` 是 `{name,occurrence}` 数组，区分同名兄弟节点；
它用于显示/恢复选择，重命名、排序、载入后客户端应刷新，不能视为跨配置永久主键。

## 命令

| command | 参数及行为 |
|---|---|
| `hello` | 协议、后端和能力边界；`indexed_full_history_seek:true`，`realtime_seek_certified:false` |
| `plugins` | 当前环境真实 Display / View / Tool class、名称、包、原生说明；Display 另有实际支持的 message_types |
| `tree` | `include_options` 可选；返回显示、视角、工具三个真实 Property 根树 |
| `options` | `property_id`；创建真实原生 editor，触发同步 requestOptions，返回当前枚举选项 |
| `set` | `property_id`,`value`；由原生 Property 设置值、触发原生槽及渲染，回传实际接受/裁切后的属性 |
| `load` | `file`；原生 YAML reader 与 VisualizationManager::load，载入后返回树 |
| `load_yaml` | `yaml`；从项目内存快照使用同一个原生 reader/manager 恢复，不依赖临时路径 |
| `save` | 可选 `file`；原生 save 与按实际实例身份合并未识别字段，保留原始导入 YAML 文本；返回 yaml，有 file 时原子写入 |
| `add` | `class`,`name`,`enabled`，可选 `parent_id`；实例化实际插件，未知/失败插件明确报错 |
| `remove` | `property_id`，删除真实 Display |
| `rename` | `property_id`,`name` |
| `duplicate` | `property_id`，可选 `name`；原生 save/load 全属性复制，保持独立配置 |
| `move` | `property_id`，可选 `parent_id`,`index`；真实 Group 之间移动或排序，拒绝移动到自身/后代，index 是 Property 子节点索引，-1 为追加 |
| `reset` | `property_id`；调用 Display::reset，含义是清除运行数据；**不是**恢复属性默认值 |
| `view` | `class`；切换实际安装的原生视角控制器 |
| `views.sync` | `views` 描述符数组；以稳定 `view_id`、真实 `class` 及可选完整 `controller_yaml` 创建/保留/删除附加机位；删除前返回合并未知字段的真实Controller快照，不重建共享Scene或重新回放Bag |
| `tool` | `class`；通过实际 ToolManager 创建/选择安装的工具，返回当前工具；保存于 `.rviz` 的 `UAV Studio / Active Tool` |
| `window` | `visible` 布尔值；原生宿主窗口显隐 |
| `frame` | 可选 `max_width` 160–1280（默认960）；返回 PNG `data_base64`、尺寸、实际 ROS `clock_ns`、耗时；最短请求间隔250ms，过快返回 `throttled/retry_after_ms` |
| `render_frame` | `target_ns`,`width`,`height`；同原生 scene/camera 真正 OGRE RenderTexture 输出；严格检查已同步时间、TF/Display 就绪、GPU和内存限制，与预览接口分离 |
| `pointer` | `type`=press/move/release/wheel，归一化 `x`,`y`，Qt `button`,`buttons`,`modifiers` 及 `wheel_delta`；转发实际当前 Tool::processMouseEvent，MoveCamera 委托原生 ViewController；遵循工具 Finished 后的默认工具回退 |
| `topics` | 当前 ROS master 的真实话题及类型 |
| `bag.open` | `file`；读真实 bag 连接、消息数量和记录时间，尚不开始播放 |
| `bag.play_from_start` | 可选 `rate`（0–16，默认1）、`duration`（秒）；从头启动本宿主拥有的 rosbag play --clock |
| `bag.stop` | 停止本宿主创建的播放子进程；不操作其他 ROS 进程 |
| `bag.index` | `file`,`time_basis`=`header`/`record`，可选 `topics`、`untimed_policy`；异步建立真实消息索引，返回绝对 ns 范围和各话题统计 |
| `bag.seek` | `target_ns` 精确字符串；异步从头恢复到目标，返回实际 clock、投递数量、最后 Header、TF 插值来源、队列状态及耗时 |
| `bag.advance` | `target_ns`；仅从已同步场景向前推进，保留 Display 资源/历史，只投递新事件，更新准确 TF；回退或未就绪时明确拒绝，由客户端选择完整 seek |
| `bag.cancel` | 取消当前索引/恢复任务，取消后的画面不标为已同步 |
| `transform` | `frame`,`target_frame`,`time_ns`；查询真实 native tf2，并返回 FrameManager 当前零时间查询与相机世界位置，用于对照验收 |
| `capture` | `file`；保存原生 RenderPanel 运行预览；明确标注 `high_resolution_export:false` |
| `shutdown` | 清理本宿主播放子进程并退出 |

`frame` 在宿主 GUI / OGRE 线程完成真实 framebuffer 读回，Qt6 通过异步 QProcess 收取后显示；
它是限频原生预览。只有完成显式 `bag.seek`，且目标时钟相等、原生回调队列排空、已索引 TF
具有完整目标时间支撑时，才标记 `experiment_playhead_synchronized:true`；重新配置、
更换数据或独立 record-clock 播放会撤销同步标记。同步标记指明实验播放头和消息历史，不代表
第三方插件异步工作全部完成、未选择的 TF 链均可用或主应用已通过端到端验收。缺少 TF 支撑时
仍返回准确目标与实际时钟，并明确标记未同步；不把无 TF 的画面当作通过。不能用于高分辨率导出。
客户端最多保留一个待处理 frame 请求，恢复历史期间清除旧预览，避免旧帧被当作新目标。

`tree` 节点包含 `native_type`、`value_type`、原生 `description`、`value`、`display_value`、
`hidden`、`read_only`、`enabled`、`editable`、`checkable`、`children`；Topic 属性另有实际 `message_type`；数值属性附原生 min/max；
枚举附 `allow_custom` 和可选 `options`；Display 附 plugin_class、display_enabled、plugin_failed；
StatusProperty 附原生 status/status_level（0/1/2）。这些字段如实反映插件状态，不把未接收数据
或 TF 错误改写为 OK。主界面可补充中文说明，但必须保留内部名称、原文说明及 class。

`set.value` 使用 JSON 标量；Color 用 `#RRGGBB` / `#AARRGGBB`；Vector 用 `[x,y,z]`；Quaternion
用 `[x,y,z,w]`。只读/禁用状态拒绝编辑，固定枚举拒绝不在真实选项中的值；浮点属性由原生
FloatProperty 的范围语义处理，响应提供最终值。状态树不能编辑。

保存仍会使用原生 writer 格式化活动配置。每个 Display 增加持久 UUID，并按当前真实 Group 树
保存；已识别字段使用实际原生 save 值，未知 map/list 属性按实例合并，缺插件继续作为原生
FailedDisplay 保留原始 payload。顶层 `UAV Studio / Original Imported YAML` 另外保存首次导入的
完整文本（含注释），避免未知字段或格式信息因版本差异消失；这不声称活动 YAML 逐字节不变。
`unrecognized_config_paths` 明确报告原生 save 不认识的字段。复制 Group 为其自身及后代分配新
UUID，移动保留 UUID，删除后保存不会复活源列表里的对象。Config 使用 deep copy，不反写源文。

节点新增 `instance_id`、`parent_instance_id`、`scene_id`、`source_id`、`source_binding` 和
`native_resource_scope`。当前每个真实 RViz Display 拥有独立资源、属性与历史，但全宿主共享
一个 scene 和当前数据源，节点明确返回 `view_scope:shared-native-scene`，tree 明确返回
`independent_view_visibility:false`、`simultaneous_sources:false`；不能据此宣称多场景、多来源或
每视角独立可见对象已经完成。`bag.open` 返回全部连接、类型、MD5、连接 ID、消息定义和数据源，
包含未选择/无 Header 的类型，不改变已有 Display；live `topics` 使用实际 master 动态发现。

## 验收及当前限制

开发专用 `native/rviz_host/verify_host.py` 启动独立 ROS master，只清理自己创建的进程，验收：
真实插件及消息类型、完整属性树、动态枚举、颜色编辑、复制独立属性、错误 ID/枚举拒绝、
原生 save/load，以及真实录包从头播放和运行截图。它不进入应用运行依赖。

```sh
python3 native/rviz_host/verify_host.py \
  --host "$PWD/build-native/rviz-host/uav-studio-rviz-host" \
  --config /path/to/config.rviz --bag /path/to/recording.bag \
  --output "$PWD/captures/rviz-host-current"
```

未完成门禁：主应用完整 GUI 工作流、全部原生自定义编辑器、共享原生多视角及主画布离屏合成的完整手动验收、
全素材时间语义覆盖、连续播放/反向逐帧性能、源选择无重载性能、属性恢复默认、
所有工具的实时/离线约束。ROS2 和更多第三方扩展按 Roadmap 跟踪；原生默认插件、核心操作、
离线配置和同步输出仍属 v1 核心门禁。`bag.play_from_start` 是实际原生对照验收入口，
不是 Header 时间播放头；Header 随机定位必须使用下面的索引接口。

2026-11-27 本地实测：发现 33 个 Display、6 个 View、9 个 Tool 插件，实际配置读取 226 个属性。
真实录包从头播放 8 秒后，原生点云/里程计插件收到消息，TF 状态正常；真实 OGRE framebuffer
已打开检查。隐藏窗口 640×360 PNG 预览单次渲染/编码耗时 18 ms（不是 P95/P99 认证），与可见
1280×720 帧缩小后 RGB MAE 为 0.257/255，实际点云内容保持。证据在本地忽略目录
`captures/rviz-host-current`，不提交私人实验画面。独立 master 启动/JSON/退出清理烟测通过。

同一录包的记录时间与 Header 时间存在显著差异，因此 `rosbag --clock` 不代表实验时间验收。
现有 `bag.seek` 实际清理 Display/TF、预热静态 TF、按明确时间顺序重放全部选定历史并排空原生
回调队列；`ros::Time::setNow` 在每个消息实验时刻推进，最终发布精确目标 `/clock`。索引和重放
使用 Qt 定时器切片，JSON 管道保持响应。载荷仍由 bag 按需读取，不常驻复制整包。

## Qt6 接入及时间语义

`native/src/native_rviz_client.*` 提供 `NativeRvizClient`，`native/qml/NativeRvizPanel.qml` 使用应用
统一外观 tokens。多个面板通过同一个 `client` 共享选择、树和编辑状态；`showConnection`、
`showTree`、`showInspector`、`showPreview` 可分别控制展示，不创建另一个属性配置副本。
`treeScope` 按真实根的 `native_type` 分为 `all`、`displays`、`views`、`tools`；Main Camera
使用 `views`，只显示同一原生 ViewController 树，不显示 Display 添加/删除或连接设置。
范围过滤保留共享选择；当前范围外的选择不进入该面板的编辑器。组件测试
`tst_native_property_scope.qml` 覆盖分区、原生属性转发、选择、删除隔离及260px窄面板。

```cpp
client.indexBag(bagUrl, "header", "reject", {
    {"topics", QStringList{cloudTopic, odometryTopic, "/tf"}}
});
// 收到 commandFinished("bag.index", true, {}) 且 indexed() 后：
client.seekAbsolute(QString::number(recording.stampAt(pausedSeconds)));
```

所有绝对时间通过十进制 ns **字符串**传递，不通过 QML double 转换 epoch 纳秒。
主程序的 `Recording::stampAt(seconds)` 以首点云 Header 为原点，可以直接传入。`replayResult`
在索引后提供 `begin_ns`/`end_ns` 及各话题范围；`busy` 表示索引/恢复中，`seekAbsolute` 在 busy
期间只保留最新待处理目标。`cancelReplay()` 可以取消。`synchronized`、`targetNs`、`clockNs`、
`previewTargetNs` 和 `previewChanged` 提供完成状态；`commandFinished` 传递真实错误。
暂停播放头就绪还要求目标、实际 clock 和预览图片时间三者严格相等且场景就绪、不在 busy 状态；
数据已到目标但图片仍旧或相差1ns都不能标为匹配。连续实时播放性能尚未认证。
`synchronized` 要求精确目标等于实际 ROS clock、`native_queues_drained:true`，并且
`tf_coverage_complete:true`、`required_tf_complete:true`、`native_scene_ready:true`。另核验实际
Fixed/Target/启用 Display 的 TfFrameProperty、投递消息 Header.frame_id 到 Fixed Frame 的真实
依赖、实际 Topic 投递及原生 Status。MessageFilter 首次等待缺 TF 时可能尚无 Error Status，
因此不能只看状态树。首帧或边界时刻没有左右 TF 支撑时，客户端保留实际覆盖信息并明确
显示未同步，不用目标时钟正确来替代 TF 验收。

`header` 基准识别真实消息定义首字段 `Header`/`std_msgs/Header`，不按未知布局猜偏移；
TFMessage 和 MarkerArray 使用各子元素 Header，逐项发布原生对应消息。零 Header 或无 Header
默认 `reject`，明确指出话题；`skip` 是用户明确排除，`map_record` 必须显式提供
`anchor_record_ns`、`anchor_header_ns`，采用该偏移映射未定时元素。不会自动推测映射，也不会
改写非零原始 Header。原 bag `/clock` 不重放。不同话题的非零 Header 时钟仍须用户明确一致，
各话题范围会如实返回；不自动把混合 epoch 校正为看似正确。

`topics` 是明确选择的数据源集合，应包含实际 Display、动态 TF 和需要的静态 TF；不匹配的
topic 不进入重放。真实样本中的 `/rosout` 有零 Header，默认策略确实拒绝；验收使用四个实际
显示/TF 话题，不把排除诊断日志说成全包 Header 覆盖。

目标位于两条动态 TF 采样之间时，用单独的原生 tf2 BufferCore 对真实左右采样执行平移插值/
四元数 SLERP；最终渲染 buffer 只保留目标以前历史和这个目标采样，避免未来原始 TF 成为
原生 TF Display 的 latest 位姿。`tf_target_samples` 明确列出左右 Header、方法和目标变换；
`tf_coverage_complete` 如实指出是否存在缺失左右支撑，缺失时拒绝外推，不伪造单位变换。
静态 TF 按显式实验时刻替换，回退后移除未来替换。第三方插件私有线程、外部节点及自定义
编辑器不受普通 ROS 队列排空保证覆盖，结果始终含 `plugin_worker_completion_certified:false`。

Noetic `BufferCore::clear()` 会保留同名 child 帧的静态/动态缓存类型。跨源把同一个 child 从静态
改为动态（或相反）可能导致原生静默使用错误的缓存语义，因此索引会明确拒绝这种已知冲突，
要求仅重启原生后端后重新索引；不会伪造可用状态，也不操作 tf2 私有内存。普通同 schema
随机跳转复用场景；主题切换不触发此类重启。单个来源内部混用同名静态/动态 child 同样拒绝。

开发验证：`native/rviz_host/verify_seek.py` 使用实际录包进行 0→8→2→8 秒 Header 定位、消息
数量/最后 Header、实际 ROS clock、原生状态及重复目标像素对照；另外清楚标为**控制夹具**的
bag 验证 Marker lifetime、DELETE、静态 TF 映射/替换、动态 TF 插值及相机 Target Frame。
私人真实画面和生成夹具保存在被忽略的 `captures/rviz-seek-current`；不提交素材。

2026-11-27 历史阶段验证共 **91 项通过**，包含缺少 TF 右采样时原生 lookup 拒绝外推、首个真实点云
时刻因缺少 TF 不标记同步、同目标重复恢复像素一致及跨源静态/动态 TF 缓存冲突拒绝。实际录包
索引耗时 973 ms，0→8→2→8 秒恢复分别为 621/1932/909/1892 ms；这些是该次运行的耗时，
不表示连续实时播放或 P95/P99 性能通过。报告为 `captures/rviz-seek-current/report.json`。

`native/tests/native_rviz_client_test.cpp` 在 Qt6 中加载亮暗面板，实际启动 WSL 宿主，验证真实
插件、动态枚举、数值修改、Unicode 配置保存、真实 By Topic、PNG 和异步最新目标合并后退出。
该阶段实际集成退出码为0，收到59个成功响应；额外核验首帧 TF 不完整时客户端不标记同步。
日志为 `build-native/native-rviz-client-integration.log`。退出后实际检查独立 master 端口已释放。
这属于程序化组件/后端集成，不替代完整主应用 GUI 人工工作流与性能验收。

## 当前严格回归与离屏输出（2026-11-27）

旧91项/59响应及此段156项均为历史阶段证据。`captures/rviz-readiness-current/report.json` 的 **156 项检查
通过**，包括真实录包、明确标记的控制夹具、缺失 Fixed Frame/Target Frame/整条消息 TF 链及
无源 Topic 的拒绝。原用户配置确实有不存在的 `global` Target Frame 和无录制源 `/path`，
严格门禁正确拒绝。命名验收副本只把 Target Frame 改为实际 `camera_init` 并禁用该无源 Path；
原配置 SHA256 不变，mapping 及两个原生点云均保留。真实点云画面已打开检查。

实际录包 `render_frame` 已生成 1920×1080、3840×2160、7680×4320 PNG，文件尺寸和非空内容
均检查。它直接在共享场景用实际相机创建对应大小 RenderTexture，临时 aspect/viewport 状态
完成后恢复，不调整窗口再放大。GPU最大 texture/renderbuffer 为 16384；4K使用实际4×MSAA，
8K在资源预算内使用0×，结果如实返回 `fsaa_samples`。单次 render+PNG 实测约82/278/1009 ms，
属于特定一次运行，不是性能分位验收。限制为64 MP、单轴16384并再次核对实际GPU上限，超限
明确拒绝，分块渲染尚未实现。当前原生输出明确 `opaque:true`、`alpha_supported:false`，使用
独立原生场景背景，`background_color` 为实际颜色；不冒充透明或纯矢量。第三方插件私有异步
工作仍未认证。私人真实图片在忽略目录，不提交仓库。

Qt6 `prepareOutputFrame(targetNs,width,height)` 始终先返回独立非空 jobId，异步发送准确 seek
（满足条件时增量 advance）再 render；`outputFramePrepared(jobId,success,targetNs,QImage,
metadata,error)` 只在时间、就绪、尺寸及 PNG 内容验证后成功。metadata 不包含大幅 base64。
输出期间阻断其他 native 配置和时间修改，`cancelOutputFrame(jobId)` 只取消匹配任务。
`snapshotConfig()` / `configSnapshotReady(...)` 提供同一实际 native save 的项目快照，
`loadConfigYaml(yaml)` 恢复；主项目接线及完整 GUI 往返须单独验收。

### 历史主画布与合成输出接线边界

本小节保留2026-11-27早期主视角接线记录。后续r5自动工作流已覆盖真实三机位合成及
4K/8K输出，见兼容矩阵；历史5DA1插件专项没有重新认证r5的整套主应用工作流。

`ReplayCanvas.qml` 在原生客户端连接时停用主视角的旧 `CloudView`，由 `nativeMainView` 显示
同一客户端的真实原生预览，鼠标拖动/滚轮转发给原生相机。暂停就绪提示使用上述严格播放头
匹配条件。这是当前代码接线；它不等于主窗口在所有播放、跳转及编辑状态下均已实际验收。
附加 `CloudInsets` 仍使用旧渲染路径，不能据此宣称原生多视角已完成。

`main.cpp` 已为 `MovieExporter` 设置同一 `NativeRvizClient`。每个输出帧先定位 Replay 实验
时间并应用该时刻的可见性/布局，再按最终画布缩放后的主视角真实像素尺寸调用
`prepareOutputFrame`；原生 `RenderTexture` 结果进入合成画布后才执行 QtQuick
`grabToImage`。因此原生三维内容不是把运行预览或窗口截图放大。导出器核验 job、精确 ns、
TF/场景就绪、scene revision、实际图片尺寸及GPU上限，并等待 Image Ready；失败、断连、
来源/场景配置/尺寸改变即停止，不回退到旧场景或旧预览。

主合成输出当前支持路径为原生主视角及其画布叠加层；选层后不包含主视角时可仅导出二维层。
可见的旧附加三维机位会明确拒绝，不能作为原生多视角输出的替代。包含原生主视角时拒绝透明
输出及与当前 RViz 背景不同的覆盖色。主合成静图限制单边8192、40MP、36–2400DPI；视频
限制3840×2160、1–120fps及1×/2×超采样，另受GPU上限约束。这些校验上限不代表已通过
相应主应用实测，也不等于宿主自身的16384/64MP能力。无分块渲染。

`writeStillImage` 对完整合成的 PDF/SVG 嵌入位图并写明 raster composition，未实现把完整
QtQuick 合成中的轨迹、文字和图例自动拆为矢量；独立科研图绘制器的矢量能力不能算入此路径。
主画布精确单帧、视频对应帧、多图层、4K/8K、批量及项目恢复的真实 GUI 工作流仍需逐项验收。

`advanceAbsolute(ns)` 对已同步且向前的目标使用 `bag.advance`，否则完整 seek；保留最近实际
PNG时必须使用 `previewTargetNs` 核对图片对应实验时刻，不能只看数据目标。真实录包
8→8.033333333→8.066666667→8.1秒及控制夹具的 lifetime/DELETE 已通过，8.1秒结果与同目标
完整恢复逐像素相同，消息投递计数一致。三个增量实测191/174/191 ms，**尚未达到实时播放
性能**，限频预览仍为最多4Hz。连续原生渲染与性能门禁继续保留。

`verify_displays.py` 新增明确生成的控制夹具，在非预设名称 Topic 上实际创建3 PointCloud2、
2 Path、4 MarkerArray、Odometry、TF、Grid共12个适用 Display、2个Group及1个disabled缺插件。
**41 项检查通过**：独立样式、Group移动/复制/删除、UUID往返、未知嵌套字段及缺插件payload/
首次原文精确保留、完整bag catalog、真实消息/TF到达、实际RenderTexture、原生ViewController
wheel后时钟/历史不变、内存快照恢复。原生截图已打开。它使用librviz真实load/save与渲染，
仍不替代单独启动标准 RViz GUI 打开导出文件的对照验收或主应用交互验收。

## 原生元素生命周期（2026-11-27）

`bag.close` 可在索引/恢复或真实 `rosbag play` 中执行。它取消本宿主任务、停止自有播放，
保存真实 YAML 后按 RenderTexture→Replay→VisualizationManager→RenderPanel 顺序释放并重建
空场景，再恢复同一组 Display/Group UUID、属性和相机。这样连 tf2 静态/动态 cache 类型及插件
私有消息历史一起释放；普通主题切换及时间跳转不触发这个明确的源移除操作。原始素材只读。
保留的配置通过真实 root disabled 撤销 Display Topic 订阅，并增加原生 Source Warning；
不会把它称为数据就绪。配置保存保留用户请求的 Enabled 值，`Source Detached` 元数据使全新
进程重新打开仍维持无源状态。之后显式 `bag.open`/`bag.index` 才重新绑定，绝不从历史自动开 bag。
新空管理器仍有 RViz 本身的空闲 TF/clock listener，资源报告单独列出实际 ROS 图订阅。

`remove_many(property_ids)` 先验证整个选择，归并已选 Group 覆盖的后代，实际 delete 原生对象，
只产生一个历史事务。`undo`/`redo` 由实际 native save 的 YAML 快照恢复，最多40条、总64 MiB；
覆盖 Display 添加/批删/属性/移动/复制/重命名/视图/配置导入以及原生相机拖动和滚轮。
By Topic 的 add→set 使用同一 history_group，一次撤销。它只管理 native 配置，不替代主项目的
素材、画布和时间轴历史；恢复配置后必须重新索引/定位，不能把旧预览当作同步画面。

Qt6 提供 `closeBag()`、`removeDisplays(QStringList)`、`undoNative()`、`redoNative()`、
`canUndo`、`canRedo`、`sourceAvailable`、`nativeResources` 与 `refreshResources()`。
关闭立即清除旧预览和 queued seek，完成后清除客户端 Topic/clock/index/ready 状态。
`resources` 来自真实 C++ Display/Property/Replay 容器、OGRE 场景节点与可移动对象、tf2 frame
列表、`/proc/self/status` RSS 及 ROS master 本节点图，不声称测得显存或第三方插件全部私有分配。

历史 `captures/rviz-lifecycle-current/report.json` 的 **405项检查通过**，其中每条协议成功也计一项，
不能解读为405种功能。明确生成的控制夹具验证：Group祖先/后代去重批删、一次完整撤销/重做、
保存重开不复活、100次真实 PointCloud2 创建→配置订阅→删除。在0/1/25/50/75/100轮，Display15、
Scene Node159、Entity44保持基线，单次进程 RSS219640→219884kB，历史容量有界；这不是性能
分位或显存泄漏认证。源关闭实测 history/observer/publisher/bag/TF为零，输出纹理释放、真实
播放进程终止、in-flight seek得到取消响应，ROS master证实旧 Display Topic订阅消失。
新进程恢复源已移除的快照仍无源且无旧TF，bag SHA256不变。这仍不替代主 GUI 二十项验收。

历史 Qt6 实际录包客户端日志 `build-native/native-rviz-client-lifecycle.log` 运行退出0，
117个成功响应，增加了批删/Undo/Redo/持久UUID/关闭源及实际资源检查；旧98响应为上一轮证据。
资源刷新测试以独立成功响应计数等待，避免同一轮 Qt 事件中 tree 响应覆盖 lastCommand 造成误超时；
观察回调绑定独立 QObject 生命周期，失败展开栈后不会由客户端析构触发悬空捕获。
逐类原生插件兼容性与后续更新见 `RVIZ_COMPATIBILITY_MATRIX_CN.md`。

实际插件矩阵33类均通过真实创建/Property/YAML往返/删除/OGRE计数归基线；真实录包单独启用
PointCloud2两个Topic、Odometry两个Topic、TF、Axes、Grid输出的7张1920×1080 PNG已实际打开
检查。第三方Imu已收到1594条消息，但缺少`livox_frame` TF，原生Status Error并拒绝输出。
不能用缺数据时某些插件自身的Topic OK状态代替实际接收与渲染认证。

原生ViewManager及ToolManager属性与Displays使用同一真实树；Qt6 `properties`包括
`displays`、`views`、`tools`三个根，保留native_type/path/id/原生枚举选项与编辑标志。
`Current View`子节点可用于Camera Inspector，不应另建硬编码相机属性样例。

宿主设置`quitOnLastWindowClosed=false`，由shutdown/EOF管理生命周期，避免删除Camera/Image
附属窗口时整个协议进程退出。所有宿主顶层窗在Polish/Show时设置ShowWithoutActivating，
隐藏模式设置DontShowOnScreen；真实RenderTexture输出在此版本仍通过。
上述33类结构、7张真实输出和117响应对应历史ELF：
`AFE176761709A1BF64F94ECCCDBFF04BA393BD1ED5A14EA26E8B55E2EA50CE7D`。
标准RViz同Bag/同配置副本的核心点云GUI目检已完成：处理其自带EOL模态后，实际Windows
computer-use观察到真实彩色点云、GlobalStatus/FixedFrame及mapping内独立Size/Alpha/Decay
属性，与宿主输出的几何/视角目检吻合。未修改GUI截图保留于本机
`captures/rviz-vanilla-no-modal/standard-rviz-observed.png`。不代表逐像素一致、全部默认插件
或主GUI工作流已验收，详情见兼容矩阵。

## 历史AC3E版本增量证据与发行边界

当时宿主 `build-native/rviz-host/uav-studio-rviz-host` SHA256 为
`AC3E76EAAAF9CDECFBBB7F306A7E27E6FF5BA5DA092C2F00895F035D9D426193`。
不能将前述旧ELF的33类/7张真实输出自动标记为此版本全量回归。

2026-11-27 已对该实际ELF独立重跑原有 `verify_plugins.py`，使用独立隐藏会话及端口49343，
未改动主应用、主GUI或历史报告。`captures/rviz-plugins-ac3e-20261127/report.json` 实测
33/33真实创建、Property、YAML往返、删除及OGRE/Display资源归基线通过；23次枚举编辑及
17次条件属性变化与旧结构基线一致。同一只读真实Bag的2个PointCloud2、2个Odometry、TF、
Axes、Grid共7张1920×1080原生RenderTexture输出均已用 `view_image` 打开检查。目标均为
`1768571820557415724 ns`；IMU收到1594条后仍缺`livox_frame`，保留错误且不输出。
报告的 `actual_sensor_rendering_verified:false` 是脚本预留的人工门禁，未被自动改为认证；
另存 `inspection.json` 记录七张图的实际内容观察、尺寸、哈希和有限范围，未认证全插件/
全时间/像素等价或主GUI。`provenance.json` 记录ELF、脚本、启动器、报告及只读Bag前后哈希；
ELF与Bag均未变化，专项进程退出0且自有会话正常关闭。逐类记录见兼容矩阵。

本版本 `captures/rviz-autostart-control/report.json` 与 `build-native/rviz-tool-control.log`
实测405项通过（包含协议响应，并非405种功能），来源明确是生成的多Display控制夹具。
覆盖生命周期、真实订阅/TF/RenderTexture、资源删除和配置恢复；报告明确
`main_application_gui_verified:false`、`vanilla_rviz_gui_verified:false`、
`independent_view_visibility_verified:false`、`simultaneous_sources_verified:false`。
该405项包含协议响应，仅覆盖Grid、Group、MarkerArray、Odometry、Path、PointCloud2、TF
共7类的多实例及1个缺插件占位，未覆盖所有33类；不能把它替代上述独立逐类复验。

Windows Qt6 独立客户端 `--auto-runtime` 实测退出0，日志为
`build-native/native-runtime-automatic.out`，stderr为空。覆盖缺少可选依赖、Standalone
manifest禁止探测、真实WSL发行版发现与旧偏好回退、显式初始文件、启动中配置排队、离线清空、
无Bag预览、编辑后受控重连，以及在rename成功回调中立即终止自有WSL PID后恢复最后确认的
配置。新默认Grid/Axes和实际MoveCamera/Select/FocusCamera/Measure工具发现与激活通过，
MoveCamera wheel实际响应通过；完整空间选择/测量GUI手感尚未验收。测试未占用主应用GUI。
Linux仅测试实际Noetic宿主；本机无Qt6开发库，未声称独立Linux Qt6客户端已编译。

`--manual-topic` 专项同样实际退出0，日志为 `build-native/native-rviz-manual-topic.out`，
stderr为空。真实空源PointCloud2的手动Topic、原生属性、YAML保存/恢复及实例UUID保持通过；
未发现类型明确等待/未验证，已发现不兼容类型拒绝，无Topic属性插件真实删除回滚通过。

Windows独立渲染的13类支持子集与原生33类结构基线是两条不同实现，见兼容矩阵。
主应用最终构建的四工作区GUI、原生完整合成/视频、原生多视角、透明/分块及性能门禁仍按
`V1_CORE_RELEASE_SCOPE_CN.md` 和总验收矩阵追踪，以上证据不授权发布稳定版。

## 历史5DA1宿主专项（2026-11-28）

本轮实际测试的ELF为 `build-native/rviz-host-multiview/uav-studio-rviz-host`，SHA256：
`5DA199D7027396797395283C3CD6B1ABDE21F90A3BCC3D80378FD598006C70C4`。
开发默认路径或安装包复制该宿主后，应按实际哈希识别版本，不能按同名路径借用旧证据。

### 插件结构、属性与真实录包输出

`captures/rviz-plugins-5da1-20261128/report.json` 来自现有 `verify_plugins.py`、独立端口49353
及自有隐藏宿主：33/33真实实例创建、Property读取、YAML往返、删除及OGRE/Display资源归基线
全部通过，错误列表为空，`plugin_contract_passed:true`。共547个原生属性节点，23/23次枚举
写入和实际值回读通过，17次真实条件属性结构/可见性变化；factory发现6个View及9个Tool。
属性数包含原生状态和根节点，不是固定属性清单，也不是全部编辑器/工具的GUI操作认证。

同一只读真实Bag的8个案例生成7张1920×1080 RenderTexture PNG，全部已用 `view_image`
实际打开检查，并解码核对尺寸。两组Odometry可见短红箭头，两组PointCloud2可见彩色点云，
TF可见小型Frame标记，Axes可见RGB三轴，Grid可见透视网格；只覆盖五类Display。
所有成功输出的 `target_ns`、`clock_ns` 均为精确字符串 `1768571820557415724`，原生TF/场景
就绪。`rviz_plugin_tutorials/Imu` 的 `/livox/imu` 实际投递并观察到1594条消息，但缺少
`livox_frame` TF，保持原生Error及 `native_scene_ready:false`，没有图片；不补造TF或将收到
数据等同渲染通过。`actual_sensor_rendering_verified:false` 及
`plugin_worker_completion_certified:false` 保留，不能从七图推定所有插件/时段/worker均完成。

同目录 `inspection.json` 保存每图内容观察、解码尺寸、准确时间元数据与SHA256；
`provenance.json` 记录宿主/相关源码/脚本/启动器及只读Bag前后哈希不变。专项退出0，stderr
为空，自有宿主和master正常关闭。其冗余图片哈希映射仅按既有inspection作过字段整理并逐图
重新核验，历史report、inspection和图像未改写或重跑。用户素材和真实几何截图均仅留本机，
这些相对 `captures/` 路径是验收索引，不把私有图像纳入Git。

### 空附加机位配置的失败与修复

F974宿主的 `captures/rviz-plugins-f974-20261128/report.json` 保留33次YAML往返失败：原生
writer写出的 `UAV Studio / Render Views: []` 经RViz Config读取后丢失空集合类型，被宿主拒绝。
旧脚本退出0或 `completed:true` 只表示探针执行结束，不能据此称检查通过。
定向红测 `captures/rviz-empty-views-red-f974-20261128/empty-view-configuration.json` 为10/13通过，
程序自身的空机位快照、显式空列表、YAML null三项失败，失败记录不覆盖。

最终宿主使用yaml-cpp检查原始 `UAV Studio / Render Views`，只允许缺省、null或sequence；
只有已验证的空sequence/null才规范化为空Config List，未放宽任意空节点。最终
`captures/rviz-empty-views-normalized-20261128/empty-view-configuration.json` 的13/13通过：
上述合法输入恢复成功，字符串、数值、boolean、空mapping及非法列表成员继续拒绝；拒绝时
已有真实Orbit/TopDown机位、Scene及资源不变。`--view-config-only` 的
`plugin_contract_passed:null` 明确不代表33类插件检查；完整探针结构失败现在返回非零。

### 客户端机位归档与真实TopDown恢复

删除附加机位前，宿主保存真实Controller的完整YAML并合并未知字段；Qt6的
`NativeViewArchive` 仅缓存文本及class，不保留GPU/相机对象，最多4096个ID、64MiB UTF-16
YAML。超预算条目保留class tombstone，恢复不完整快照时明确失败，不静默替换为默认Orbit。
同项目自动重连保留最后确认的归档，新项目清理；首轮同步前的快速添加/撤销/重做也保留
有界待确认class。具体实现使用同一原生机位及项目历史，不另建相机属性表。

`captures/native-view-lifecycle-20261128/report.json` 与 `run.log` 的Qt6客户端实测退出0。
真实TopDownOrtho参数Scale=19.25、Angle=0.375、X=2.5、Y=-4.75、Target Frame=`body`，
连同未知嵌套YAML，在两次删除/恢复循环后完整保持。Display/Scene/source身份、原生资源、
实验索引结果与同步状态不变，目标及clock均为 `1768571820557415724 ns`；恢复过程没有
`bag.open`、`bag.index`、`bag.seek` 或 `bag.advance` 新请求。最终清理两个附加机位后Camera
7→3、Scene Node164→160，Display13、Group2保持，实际camera/panel资源释放。
`build-native/native-view-archive-20261128.log` 另覆盖文本归档替换/FIFO、预算tombstone、
非法输入及清理；这些不是显存测量或长期泄漏证明。

该客户端报告明确 `main_application_gui_verified:false`。完整主GUI、所有插件真实数据、
最终ELF的标准RViz对照、每视角Display显隐/材质隔离、Alpha/独立背景覆盖/分块及30fps、
P95/P99性能均未由本轮认证；早期标准RViz点云目检与r5自动合成保留其原版本归属。
最终GUI、完整工作流及发行门禁仍以总验收矩阵为准，本专项不授权稳定版发布。

## 当前8C87宿主专项（2026-11-28）

当前实际ELF为 `build-native/rviz-host-property-sync/uav-studio-rviz-host`，SHA256：
`8C87BB91B70B715D16511B5FDF91CBF250313E6F4DE506D9995DB426F5BC1A29`。
`captures/rviz-plugins-8c87-20261128/report.json` 独立复验33类（32个默认插件及
`rviz_plugin_tutorials/Imu`）的创建、547个真实属性节点、YAML往返、删除及资源恢复；
23次枚举编辑和17次条件属性变化通过，自有宿主/master正常关闭。报告SHA256为
`2DA385F6CA1B7A06AB985511B6A6D00A15A57DF90D0DE19BFE8C1AD7ACD3170C`。

同次真实Bag输出为五类Display的七张1920×1080图片，逐图检查与准确时间记录见同目录
`inspection.json`；IMU虽实际投递1594条消息，仍因缺少 `livox_frame` TF保持未就绪，未输出图片。
20项相机属性/类型编辑与九张输出属于另一个专项，详见 `NATIVE_CAMERA_PROPERTY_SYNC_CN.md`。
这些检查不把历史5DA1空机位/删除恢复或r5主应用合成自动归为本版本重跑；全部传感器渲染、
完整主GUI、标准RViz对照、每视角Display隔离、Alpha/分块及性能门禁仍未完成。
