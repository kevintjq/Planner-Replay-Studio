# 速度仪表共享矢量绘制与内联 Inspector

本模块补充 `SPEED_METER_BACKEND_CN.md`，不替换已有数据、时间、画布及工程系统。
`SpeedGaugeItem` 接受 `SpeedMeterModel::frame()` 的真实快照；没有当前有效样本时显示 N/A，
不把缺数转换成 0，也不自行读取 Bag 或修改播放头。

## 接线

- 在现有 QML 模块注册 `SpeedGaugeItem`，编入 `speed_gauge_item.cpp/.h`，链接 `Qt6::Quick`、`Qt6::Svg`、`Qt6::Concurrent`。
- `dataMap` 接受 `frame(id, exactStampNs, settings)`；`styleMap` 只保存外观。
- QQuickPaintedItem 的 `textureSize` 在高分辨率整画布导出时应按真实输出比例设置，
  防止放大预览纹理。独立导出始终在请求尺寸上重新执行共享绘制，不截窗口。
- `SpeedMeterInspector.qml` 的必需参数为 `controller`、`model`、`meterId`；另传
  `topicInspector`、`sourceMap`、`settings`、`styleMap`。Inspector 仅发出补丁信号，工程保存和撤销仍由现有 Controller/Canvas 管理。
- `sourceEdited` 返回完整的来源草稿；调用方保存来源并调用模型 `loadSource`。
  `settingsEdited` 与 `styleEdited` 返回单次补丁；`presetRequested(name)` 由调用方获取
  `presetStyle(name)` 后只替换外观，不覆盖来源、单位、统计区间、位置或同步设置。
- `exportElementRequested(format)`、`exportCsvRequested()`、样式复制/粘贴均发给宿主；
  宿主选择文件路径并调用实际导出，不由 Inspector 创建无必要的配置弹窗。
- Transform/Visibility 复用现有 `CanvasTransformEditor`，避免第二套变换及可见性状态。

## 绘制契约

共享 `drawGauge(QPainter&, QSizeF, dataMap, styleMap)` 以 600×520 设计坐标等比适配。
默认 250° 开放圆弧，Marker，透明背景，最大值橙红、当前值最大字号、平均值青蓝。
`current/maximum/average/rangeMinimum/rangeMaximum` 是模型已经转换后的显示单位数值；
`currentValid` 明确控制 N/A。数值超量程仍显示实际值，同时显示超量程说明。

渐变节点支持 `position:0..1` 或物理 `valueSI` 与 `color`。节点先映射到真实数值量程，
再沿角度分布；`valueSI` 在切换 km/h 时保持物理位置。渐变由细分的实色矢量路径组成，
不依赖可能被 SVG 后端位图化的角度渐变画刷。

可配置属性包含：

- 圆弧：`startAngle/sweepAngle/radius/arcWidth/arcOpacity`，`showArc/showInnerRing/showOuterRing`，`gradientStops`。
- 刻度：`showTicks/showTickLabels/majorTickCount/minorTickCount/majorTickInterval`，
  `majorTickLength/minorTickLength/tickWidth/tickColor/tickLabelColor/tickLabelSize/tickDecimals`。
- 指示：`indicatorStyle` 为 `marker/needle/dot/progress/none`，以及颜色、宽度、不透明度。
- 文字：`fontFamily/decimalPlaces/showUnit/title/subtitle/textColor/maxColor/averageColor/titleColor`。
  `currentText/maxText/averageText/titleText/unitText/tickText/subtitleText/warningText` 子对象支持
  字体、字号、权重、颜色、不透明度、描边、矢量偏移阴影；统计文字另支持 x/y/width/对齐。
  最大/当前/平均标题可通过 `maximumLabel/currentLabel/averageLabel` 修改。
- 背景：`backgroundMode` 为 `transparent/translucent/solid`；背景颜色/不透明度、边框、圆角、内边距、面板阴影独立。
- 图形、文字、指示和整体不透明度分别配置。颜色与软件 Light/Dark/System 主题无隐式依赖。
- 六种外观预设为 `gradient-gauge/minimal-gauge/digital-hud/speed-curve/publication/custom`。
  `speed-curve` 只绘制宿主明确传入 `dataMap.plot` 的同源真实样本，没有样本不会生成虚构曲线。
  样本使用模型的整数 `stampNs/value/valid/breakBefore`。

## 独立导出

`exportElement(QUrl destination, QSize pixelSize, int dpi, QString format)` 立即返回 jobId，
后台处理完成后发出 `exportFinished(jobId, success, metadata, error)`。
执行期间的输入是不可变快照，后续播放或样式修改不会污染本次结果。另一导出正在运行或 URL 非本地时，
排队发出明确失败；调用方可先保存 jobId。实际源文件与不匹配的文件扩展名受到保护。

支持透明 PNG/TIFF、真正路径 SVG/PDF，300/600/1200 DPI 及其他 36–2400 DPI。
SVG/PDF 使用同一圆弧、刻度与字形路径；文本为可缩放字形轮廓，不冒充可编辑搜索文本。
PDF 页面由阅读器呈白色不代表写入了白色仪表底板。仅显式背景模式会绘制底板。
输出上限 100 MP、单边 32768 px；该纯矢量仪表不创建 GPU 纹理。

## 验证界限

隔离验证项目位于本地忽略目录 `build-native/speed-gauge-check`，产物位于
`captures/speed-gauge-vector-current`，不提交用户实验素材。
`speed_gauge_test.cpp` 检查真正 SVG/PDF 路径、PNG/TIFF Alpha/DPI、4K/8K 尺寸、
准确 ns、单位切换不移动物理指示、原始来源保护、后台导出响应、快照一致性以及双主题 Inspector。
这些是确定性测试夹具；独立速度表参考图尚未找到，不宣称已逐像素对照。
主应用中的实际飞行、拖缩、多仪表、复杂背景、视频、工程重开和整画布高分辨率仍需宿主真实 GUI 验收。
