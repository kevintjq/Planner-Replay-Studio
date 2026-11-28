# 源视频 PTS 与预览/导出同步

视频解码继续在 C++ 中实现，直接使用 libavformat、libavcodec、libswscale。桌面预览、视频
合成、独立源帧及轨迹处理共用 `VideoReader`；不再用 OpenCV 的帧号/FPS 推算显示时间。

每一帧保留整数 PTS 和有理数 time_base。视频局部时间零点是第一张实际解码显示帧的 PTS；
`source_time = (PTS - origin_PTS) × time_base`。时间轴先应用已有偏移/两个同步锚点/
保留区间，得到局部源时间，再选择不晚于该时间的最后一张显示帧。B 帧按显示顺序解码，
跳转先定位关键帧再解码到目标；不会用请求时间改写实际帧时间。

源帧缺少原始 PTS 但解码器能够恢复时，显式标记 `decoder_recovered`；两者都缺少则拒绝
精确同步。重复或倒退的显示时间戳明确报错。未建立索引的随机跳转可能不知道帧序号，
此时序号为 unknown/null；实际 PTS 仍然有效。旋转元数据用于源像素方向。

解码读取 AVFrame/stream 的有效 sample aspect ratio（SAR），先经同一次 libswscale 转换为
方形像素显示尺寸，再应用显示旋转。例：144×96、SAR 4/3 的源帧显示为 192×96；旋转
90° 后显示为 96×192。预览、视频、轨迹处理及源帧导出共用此路径，整数 PTS 不变。
导出来源同时保留 coded_size、sample_aspect_ratio、display_rotation_degrees、display_size，
并明确记录 `square_pixels_then_display_rotation`；这些输出是经过显示几何归一化的源图，
不声称是未变换的编码像素。无有效 SAR 时采用 1/1；归一化后单边超过 32768 或超过
268435456 像素则明确报错，不静默压缩尺寸。当前仅处理 0/90/180/270° 显示旋转。

预览 `ClipPreview` 在工作线程解码，暂停时以最新请求准确落帧；连续播放可暂时呈现落后
请求少于 250 ms 的已完成帧，以免较慢解码导致画面一直不更新。此阈值是呈现策略，
不是性能认证。离屏导出重新按准确请求时间和完整源质量解码，不使用该预览临时图像。
换源时清空旧画面，失败会显示真实错误。

取消谓词在分配 FFmpeg 上下文后、打开源前即安装，覆盖打开、流探测和第一帧解码；
单次 select 的取消不会清除读取器的生命周期取消。预览禁用、换源及销毁都会请求取消，
被中断的读取器在恢复时重新打开，避免复用保留 AVERROR_EXIT 的 AVIO 上下文。谓词
只持有独立 atomic 标志，不反向持有读取器的拥有者。视频音轨预检在工作线程完成，
使用同一实际解码起点；主线程不再同步等待 FFprobe/打开/首帧解码。预检失败或取消
仍恢复画布选项、原播放头和播放状态，不发布不完整目标文件。

轨迹源图批量清单同时记录 requested_time、output_time、整数 PTS/origin_PTS、time_base、
纳秒源时间和 timestamp kind。未知帧号写 null。单张源图片的描述保留实际源时间和 PTS；
合成画布时间仍使用实验播放头时间。容器时长只用于有效区间，不能代替逐帧时间戳；
封装转换可能改变末帧时长，因此回归按各文件的独立 FFprobe 结果核对。

`video_reader_test` 使用独立 FFprobe 时间戳与 FFmpeg 像素，对照可变帧率、非零起点、
B 帧、随机跳转、末帧排空、旋转、Matroska、缺少 PTS 及取消。`clip_preview_test` 核对
异步预览的 PTS/像素、请求合并、换源、错误及事件循环响应。这些可控回归不替代长视频、
真实音轨、多相机漂移和整套 GUI 工作流验收。

新增定向回归还覆盖真正带 B 帧源的随机 PTS/独立解码像素、SAR 与旋转顺序、构造及
生命周期取消、预览禁用/恢复和 SAR 批量输出来源。`movie_prepare_test` 验证音轨预检异步、
无音轨失败/立即取消及编辑状态恢复；成功编码链继续由 `--smoke-mp4` 核对。新增用例
定向实测中 `clip_preview`、`movie_prepare`、`video_reader` 已通过。`trajectory_export` 最初
因源码与旧 object/library 时间戳相同，增量构建沿用缺少新来源字段的库而失败；保留该
失败记录后，显式重新编译对应源文件，完整几何及 SAR 断言通过（1.26 s；记录
`build-native/trajectory-geometry-test.log`）。最终安装包必须使用干净构建，避免打入旧库。
`--smoke-mp4` 还新增实际 PCM 的 RMS/440 Hz 核对，结果单独记录；上述可控回归不代表
所有真实素材、长时间音画漂移及 GUI 工作流已验收。

Windows 源码构建额外需要 FFmpeg 的开发/共享包（include、lib、bin），通过
`-DFFmpeg_ROOT=...` 指定。构建、Qt 部署和安装器必须使用相同 ABI 的 DLL；运行时不需要
Python。外部 FFmpeg/FFprobe 继续负责 MP4 编码和独立测试探测。
