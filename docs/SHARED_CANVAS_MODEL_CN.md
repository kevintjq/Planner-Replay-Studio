# 共享画布模型与素材生命周期证据

2026-11-27。本文记录 C++ 模型和实际输出文件证据，不替代 GUI、真实视频解码、RViz、播放中资源释放或完整工作流验收。

## 同一模型，独立工作区状态

`CanvasLayerModel` 不依赖 `ReplayController` 实现。工作区通过 `Binding` 提供画布状态、真实资源目录、存在性、底层删除/复制、编辑事务、时长及忙状态。Replay 的兼容构造函数放在 `canvas_replay_binding.cpp`，Trajectory 使用同一公共模型和求值器。

- `rawLayers` 保留 `source`、归一化位置、默认透明度及样式等真实字段；项目 `items[id]` 叠加可编辑属性。
- `settings`、`geometry` 和 `active` 复用 `canvas_layer_state`。各属性的部分关键帧独立插值；只改变透明度的关键帧不会重置位置轨道。
- 可见区间采用左闭右开语义。未知元素、不合法数值及忙状态下的写入被忽略。
- 宿主事务从第一次 `beginEdit` 开始，最外层 `endEdit` 结束。嵌套取消恢复整个事务；宿主负责完整历史记录。Replay 在事务期间抑制各属性 mutator 的逐次历史，结束后记录一次。
- `Binding.duplicate` 仅复制底层资源，返回新 ID；不再次复制通用 `items`，也不单独创建历史。公共模型保留几何、样式、关键帧和区间，解除旧分组并偏移副本位置。
- 删除先复制所选 ID，宿主同步清理选择不会令删除对象或提示数量变成零。

## 可复用科研元素

`canvas.elements[]` 保存共享文字、图片、箭头、圆及矩形。新增类型通过 `supportedElementKinds()` 查询。标尺、图例和 Colorbar 尚未在本库实现；新增不受支持的类型返回空 ID。

公共接口：

```cpp
QString addElement(const QString &kind, const QVariantMap &properties);
void editElement(const QString &id, const QVariantMap &changes);
QVariantMap element(const QString &id, double time = -1.) const;
QVariantList copySelected() const;
QStringList pasteElements(const QVariantList &elements);
void removeElements(const QStringList &ids);
```

复制粘贴保留可编辑内容、样式、变换、关键帧和区间，创建新元素 ID，并重新分配分组 ID；多个元素粘贴或混合宿主/共享元素删除使用一次事务。跨工作区剪贴内容目前只包含这五类公共科研元素，不把三维视角或任意宿主资源伪装成可移植元素。

默认输出样式由 `canvasElementDefaults` 集中提供，不读取应用亮暗主题。`style` 包括 `fill`、`stroke`、`strokeWidth`、`color`、`fontFamily`、`fontSize`、`fontWeight`、`alignment`、`verticalAlignment`、`headSize`、`radius` 和 `imageFit`。`designWidth` 保存创建时画布宽度，保证预览与高分辨率输出的字体及描边比例一致。

文字额外支持 `textStrokeColor` / `textStrokeWidth`，以及 `textShadowColor` / `textShadowOpacity` / `textShadowOffsetX` / `textShadowOffsetY` / `textShadowBlur`。默认描边宽度与阴影不透明度均为 0；实测与旧 `drawText` 的默认像素完全相同。开启效果时使用 Qt 实际 shaped glyph runs 生成字形轮廓，保留换行、连字与中英文字体回退，不逐字符猜测字形。柔化阴影采用有限的二项式高斯近似核及透明度归一化；SVG/PDF 保留真实字形路径，不截取整层位图。外观在元素边界内预留效果空间，避免预览纹理裁掉描边或阴影。字形路径在各渲染线程独立缓存，并限制路径点总量。

`CanvasScientificItem.contentOpacity` 在公共 Painter 内应用图层透明度；科研元素的 QML 外层不再对已经合成的纹理重复淡化，背景外观单独应用相同图层透明度，选中控件保持清晰。这样实际预览与离屏 PNG/PDF/SVG 对描边、填充和阴影交叠采用相同语义。效果开关、描边色和宽度、阴影色/不透明度/柔化/水平及垂直偏移均在 Inspector 内联修改，滑块继续使用公共编辑事务。

`paintCanvasElement` 只绘制局部内容；外层共享变换负责位置、锚点、旋转、翻转及透明度。`paintCanvasElements` 提供共享元素的有序绘制。文字和形状使用原生 QPainter 图元，PDF/SVG 不经窗口截图；图片按真实位图嵌入。完整宿主输出应将公共元素与宿主视角按同一 z 顺序合并。

## 图片与项目文件

新增图片只接受可解码的本地文件，兼容 Windows 绝对路径与 `file:` URL。原始素材保持只读。

`ProjectDocument` 同时处理：

- `composition.canvas_layers.elements[].source`。
- `trajectory_studio.canvas.elements[].source`。
- 拖影参考背景 `trajectory_studio.background_file`。
- 既有视频、图片、音频、ROS Bag、标定及拖影视频。

运行时 getter 返回绝对路径；保存和打包重新映射相对路径。图片进入真实 `sources` 引用、受保护素材路径和 portable 复制流程；删除最后一个图层仍保留素材及引用数 0。确认移除源会删除两个工作区的相应图片实例及背景引用，不留下悬空引用。拒绝移除引用中的源不修改项目。

实时 `references`、`referenceCount`、`url`、`exists` 及文件大小不作为持久化真值；每次从当前项目图计算。保留源目录时保存其他用户元数据。替换操作按文件路径与素材类型定位，避免替换视频时误改同文件的独立音频用途。

## 已执行验证

在 MSVC 2022 / Qt 6.8.3 下独立构建并运行，以下四个可执行程序均退出 0：

| 测试 | 实际验证 |
|---|---|
| `element_lifecycle_test` | ProjectDocument 与真实 ReplayController/Canvas 操作；共享源 0/1/2 引用、无效/拒绝操作不变、音频及两工作区科研图片 SaveAs/portable/源删除、整批删除一次 Undo 恢复完整 manifest/几何/关键帧/区间、Redo、保存重开无复活、100 次增删撤销重做、全部源文件 SHA-256 不变 |
| `canvas_binding_test` | 无 ReplayController 链接的独立宿主；任意真实宿主 kind 与默认放置、嵌套取消、底层副本、同步选择清理、200 次 seek/独立属性关键帧/半开区间、锚点旋转翻转透明度、五类共享元素 CRUD、跨宿主批量复制粘贴、Windows 图片路径重验证，以及实际 PNG/SVG/PDF 文件 |
| `canvas_appearance_test` | 真正 QtQuick Rectangle 的 `grabToImage` 与公共 QPainter 像素对照；背景/内容不透明度、内边框、圆角、透明边框、无边框、背景不透明度 0、透明底；真实 CanvasScientificItem 与 CPU 的文字描边/柔化阴影在图层不透明度 1 和 .55 时逐像素 RGBA 容差 ≤3；默认文字像素保持，双语换行/连字字形轮廓、2 倍分辨率缩放及实际 SVG/PDF 文件 |
| 既有 `project_document_test` | 旧 `.slamfpv` 与 `.uavstudio`、项目路径重映射、portable 移动、RViz 设置、相机/时间编辑兼容及安全失败 |

`captures/shared-canvas-current/` 的测试产物已经打开 PNG 目检。矢量 SVG 无 `<image>`，矢量 PDF 无 `/Subtype /Image`；混合 SVG/PDF 均包含其真实图片。透明画布角点和指定图片区域像素也经过验证。测试显式加载实际 Windows 中英文字体，避免 Qt offscreen 虚拟字体影响观察。

这些产物是明确构造的模型/图形测试资料，不是真实无人机实验截图。GUI 二十项、播放中删除、解码器/GPU 释放、实际 100 次交互资源监控、全分辨率整画布输出和真实工作流仍以完整验收矩阵及主任务运行证据为准。

补充修复：公共 `loadCanvasElementImage` 使用 QImageReader 的 EXIF auto-transform，创建时的默认几何也使用显示方向后的宽高。实际 Orientation 6 的 JPEG 测试验证原始 30×20 转为显示 20×30、上下颜色正确、CPU 输出和默认宽高比正确、源文件字节不变。公共 `paintCanvasItemAppearance` 同时被独立画布输出和 Trajectory 原生层/科研层调用；默认无背景无水印。QtQuick 对完全透明边框视为无边框，公共绘制已经按实际对照结果匹配。

文字效果补充输出：`text-effects-preview.png` / `text-effects-cpu.png`、`text-effects-2x.png`、`text-effects-vector.svg` / `.pdf`。实际打开预览及 SVG 重新渲染图目检一致；重开 SVG 与 CPU 的 alpha 覆盖差异约 0.55%，SVG/PDF 无图片对象。未开启效果仍保留原生文字输出；开启效果的文字本体、描边和阴影均为字形路径。轮廓与 Qt 栅格文字存在正常字体 hinting 差异，测试按 1 像素边缘容差检查。专项测试还通过 QQmlComponent 实际加载两份相关 QML，并验证正负阴影偏移；不能把此测试写成安装包 GUI 的全文字工作流验收。
