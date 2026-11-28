# 速度仪表数据后端

`speed_meter_data` 与 `speed_meter_model` 是共享数据和统计层，使用 C++/Qt，无 Python 运行依赖。
仪表外观、自由画布变换和项目持久化由现有界面/工程系统负责；本文件不把数据测试当作完整 GUI、矢量输出或参考图验收。

## 数据来源及单位

`visitBagTopic` 复用已有 ROS1 Bag 解压和连接索引，完整单次遍历指定 Topic/type，回调的序列化视图仅在回调期间有效。
不调用用于显示的 `sampleBagTopic`，不经过其 20 万点上限或降采样。

`loadSpeedSeries(path, source, transforms)` 的 source 支持：

- `topic`、可选 `type`；同名多消息类型不混用。
- `timeBasis` 必须显式为 `header` 或 `storage`，不猜测时钟偏移。所有对外绝对时间戳是整数纳秒字符串。
- `fields` 为三个实际数值字段；`fieldPath` 为一个自定义数值字段。缺省仅对实际标准消息推断字段。
- `inputUnit` 为 `m/s` 或 `km/h`。标准 Twist/Odometry 的标准线速度字段可使用其 SI 定义；自定义字段必须显式给单位。
- `sourceFrame` 为明确覆盖，`targetFrame` 为请求的输出坐标系；没有覆盖时，Odometry Twist 使用真实 `child_frame_id`，Stamped 消息使用真实 `header.frame_id`。
- `maximumGapSeconds` 默认 0.5 s。该值控制插值和积分，不按消息频率自动扩大。
- `scalarIsSpeed` 仅用于用户明确声明自定义标量是非负速率时；普通有符号数值不冒充可积分的飞行距离。
- `method: position-difference` 对真实位置字段进行中心割线差分，段端点单边差分；标准位置字段单位为 m，自定义位置字段必须显式 `inputUnit: m`。结果带 `estimated: true` 和算法说明。

已实现标准 Odometry、Twist、TwistStamped、TwistWithCovarianceStamped 等类型以及真实记录定义中的自定义字段。
需要 TF 时按每条消息的真实 Header 时间查询；缺失 Header/Frame/TF 会返回明确诊断及无效样本，不能以原始错误坐标系或 0 替代。
线速度 TF 表达的是原参考点同一速度向量的坐标轴旋转，不声称完成参考点平移、运动参考系速度相减或完整空间 Twist 变换。
位置差分在请求坐标系中先变换位置，再求相对位置导数。

内部速度始终保存为 float64 m/s，显示 `km/h` 严格乘 3.6，距离保持 m，时长保持 s。
旧 `Recording` 的既有直接速率缓存可经 `useRecording` 继续使用，但其原有 float32 精度明确写入来源；
完整标准/自定义字段应走真实 Topic 读取，不能从旧标量速率反推出三轴分量。

## 精确时间与统计定义

`SpeedIndex` 对当前值二分查找；只在相邻有效样本、相同坐标系及允许间隔内做线性插值，不向源区间外外推。
无效数值保留为插值隔断；缺少 Header 的消息在下一有效消息前标记断点；冲突的重复时间戳变为无效隔断。

模式为 `total`、`horizontal`、`vertical`、`x`、`y`、`z`、`custom`。三维/水平合成非负；垂直及方向分量保留符号。
区间使用精确 `beginNs/endNs` 字符串，先合并重叠，再扣除排除区间。时间积分使用真实时间戳下的梯形法，
边界线性插值，缺数、换 Frame 及长间隔不积分。时间平均为该积分除以实际有效时长，不是样本算术平均。
最大/最小仅取有效区间内的原始有效样本，不把插值边界称为原始最大值；排除区间的边界样本也排除。
距离是三维速率或明确声明标量速率的积分；方向分量积分不标为距离。

索引使用前缀积分和分块极值；切换单位、统计区间、量程及外观不重读 Bag。曲线显示使用保峰降采样，
原始索引及 CSV 保留全部有效/无效样本。单数据源最多缓存五百万条，超过资源限制明确失败，不降采样冒充全量统计。
`filteredSpeedSeries` 提供按真实 dt 的指数低通，遇到缺数、长间隔或换 Frame 重置；返回新副本及滤波定义，原始数据保持。
该函数尚须由 Inspector 明确选择、区分原始/滤波统计后接入，不能因函数存在就宣称 GUI 滤波已验收。

## 最小接线

`SpeedMeterModel` 不依赖 Controller 或 ProjectDocument。工程保存仪表配置；运行时只管理数据缓存。

1. Bag/TF 数据源变化时，C++ 调用 `setTransforms(recording->transforms)`，随后按工程配置注册各仪表。
2. `loadSource(id, bagUrl, sourceMap)` 异步读取与建索引；多个 id 的相同 sourceMap 共享缓存。
3. `sourceState(id)` 返回 `working/ready/error/provenance`。`ready` 表示缓存完成，具体时间仍须检查 `currentValid`。
4. 每个准确播放头调用 `frame(id, replay.experimentStampNs(), settings)`。不可把视频倍速乘入速度值。
5. `removeMeter(id)` 保持其他仪表独立；`clear()` 取消旧 generation，迟到结果不得写入新工程。

settings 的 `unit` 为 `m/s` 或 `km/h`；`mode` 为上述速度模式。
`statisticsMode` 支持 `entire/elapsed/custom/multiple/current_clip`，后面三种必须提供 `includeRanges`；
`excludeRanges` 始终为精确实验纳秒区间。`elapsed` 使用传入播放头，其他范围与当前值独立。
量程用 `rangeMode: auto/manual`、`lockRange`、`rangeMinimumSI/rangeMaximumSI` 保存物理值。
结果同时提供 `current/maximum/minimum/average` 的显示单位值和对应 `*SI`，以及范围、指示比例和明确 overflow 提示。
N/A 当前值不返回 `current` 数字，不能用 `value || 0` 隐去缺数。

`plot(id, settings, maxPoints)` 返回精确时间、SI/显示值及断线标志，仅供曲线显示。
`exportCsv(id, destination, settings)` 后台原子写入完整曲线及统计；同一 CSV 以 `kind=sample/statistic` 区分行，保留来源、时钟和计算定义 JSON。
Elapsed 曲线/CSV 另外提供精确 `playheadNs`，不读取 UI 的隐式时间。

## 独立验证

2026-11-27 已用本机 MSVC 19.42、Qt 6.8.3 独立构建 `build-native/speed-check/out`，
`speed_meter_test` 首轮运行退出 0：实际 ROS1 Bag 格式中的标准和自定义消息、真实 Header 时间、
Odometry child Frame 与实际 TF、缺失 TF 的 N/A、不规则采样时间加权、重叠/排除区间、单位与量程转换、
多仪表异步缓存、取消后的新 generation 及完整 CSV 均通过。

200 Hz 测试夹具完整保留 200,001 样本，孤立最大值 50 m/s，完整时间平均 1.000245 m/s；
显示曲线为 334 点，完整读取/建索引/核查首轮 3,917 ms，最终缓存修复后 4,005 ms。
最终组回归包含既有 Recording 缓存后台索引、重复设置不重读、失效新源不保留陈旧数值，运行退出 0；
结果保存在本地忽略目录 `build-native/speed-check/speed-meter-results.txt`。
这些是明确的测试夹具，不是用户真实飞行数据或 GUI 性能认证；实际速度仪表 GUI、参考图及图片/视频/矢量输出验收另行完成。
