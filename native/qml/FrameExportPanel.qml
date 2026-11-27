import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel
    required property var controller
    signal saveRequested(var options, double time)
    signal batchRequested(var options)
    property var layerIds: []
    readonly property var viewChoices: [{id: "main", label: "Main · 主视角"}].concat(controller.cloudViews)
    readonly property var layers: [{id: "main", label: "Main · 主视角"}, {id: "speed", label: "Speed · 速度表"}].concat(controller.cloudViews, controller.videoLayers, controller.sceneLayers)
    function options() {
        return {width: Number(pixelWidth.text), height: Number(pixelHeight.text), dpi: Number(dpi.text),
            scope: ["canvas", "view", "layers", "view"][scope.currentIndex], viewId: viewChoices[view.currentIndex]?.id ?? "main",
            allViews: scope.currentIndex === 3, currentTime: controller.position,
            layerIds: layerIds, includeText: textLayers.checked, includeGauge: gauge.checked,
            background: background.currentIndex === 0 ? controller.sceneBackground.toString() : background.currentIndex === 1 ? appearance.outputColors().matteWhite : background.currentIndex === 2 ? appearance.outputColors().matteBlack : background.currentIndex === 3 ? "transparent" : customBackground.text}
    }
    function batchOptions() {
        return Object.assign(options(), {mode: ["current", "manual", "interval", "markers", "samples"][timeSource.currentIndex],
            times: times.text, begin: Number(begin.text), end: Number(end.text), interval: Number(interval.text), sampleFps: Number(sampleFps.text),
            pattern: pattern.text, format: ["png", "tiff", "jpg"][batchFormat.currentIndex],
            figure: figure.checked, columns: columns.value, figureFormat: ["pdf", "svg", "png", "tiff"][figureFormat.currentIndex]})
    }
    StudioLabel { text: "Export Frame · 独立画面导出"; font.bold: true }
    StudioLabel { text: "使用当前实验时间和完整源视频帧；输出配色独立于软件主题。"; color: appearance.colors.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
    StudioComboBox { id: scope; Layout.fillWidth: true; model: ["Export Canvas · 完整画布", "Selected View · 单独视角", "Selected Layers · 选中图层", "All Views Separately · 各视角分别导出"] }
    StudioComboBox { id: view; visible: scope.currentIndex === 1; Layout.fillWidth: true; model: panel.viewChoices; textRole: "label" }
    ColumnLayout {
        visible: scope.currentIndex === 2; Layout.fillWidth: true
        Repeater {
            model: panel.layers
            delegate: StudioCheckBox {
                required property var modelData
                text: modelData.label ?? modelData.id
                onToggled: {
                    const ids = panel.layerIds.filter(id => id !== modelData.id)
                    if (checked) ids.push(modelData.id)
                    panel.layerIds = ids
                }
            }
        }
    }
    StudioComboBox {
        Layout.fillWidth: true
        model: ["1080P · 1920 × 1080", "2K · 2560 × 1440", "4K · 3840 × 2160", "8K · 7680 × 4320", "Custom · 自定义比例"]
        onActivated: {
            if (currentIndex < 4) {
                pixelWidth.text = [1920, 2560, 3840, 7680][currentIndex]
                pixelHeight.text = [1080, 1440, 2160, 4320][currentIndex]
            }
        }
    }
    GridLayout {
        columns: 2; Layout.fillWidth: true
        StudioLabel { text: "Width · 像素宽" }
        StudioTextField { id: pixelWidth; text: "1920"; Layout.fillWidth: true; validator: IntValidator { bottom: 16; top: 8192 } }
        StudioLabel { text: "Height · 像素高" }
        StudioTextField { id: pixelHeight; text: "1080"; Layout.fillWidth: true; validator: IntValidator { bottom: 16; top: 8192 } }
        StudioLabel { text: "DPI" }
        StudioTextField { id: dpi; text: "300"; Layout.fillWidth: true; validator: IntValidator { bottom: 36; top: 2400 } }
    }
    RowLayout {
        Repeater { model: [300, 600, 1200]; delegate: StudioButton { required property int modelData; text: modelData; onClicked: dpi.text = modelData } }
    }
    StudioComboBox { id: background; Layout.fillWidth: true; model: ["Scene · 当前场景", "White · 纯白", "Black · 纯黑", "Transparent · 透明", "Custom · 自定义颜色"] }
    StudioTextField { id: customBackground; visible: background.currentIndex === 4; placeholderText: "#RRGGBB"; Layout.fillWidth: true }
    StudioCheckBox { id: textLayers; text: "Text Layers · 文字图层"; checked: true }
    StudioCheckBox { id: gauge; text: "Speed Meter · 速度表"; checked: true }
    StudioLabel { text: "PDF / SVG 当前嵌入高分辨率合成位图；矢量图层输出尚待验收。超出 GPU 上限会明确报错。"; color: appearance.colors.secondaryText; wrapMode: Text.Wrap; Layout.fillWidth: true }
    StudioButton {
        visible: scope.currentIndex !== 3
        text: "Save Frame… · 保存当前画面"; Layout.fillWidth: true
        enabled: pixelWidth.acceptableInput && pixelHeight.acceptableInput && dpi.acceptableInput && (scope.currentIndex !== 2 || panel.layerIds.length > 0)
        onClicked: { panel.controller.pause(); panel.saveRequested(panel.options(), panel.controller.position) }
    }
    StudioLabel { text: "Keyframes · 批量关键帧"; font.bold: true }
    StudioComboBox { id: timeSource; Layout.fillWidth: true; model: ["Current · 当前时间", "Manual Times · 手动时间列表", "Interval / Range · 时间区间与间隔", "Event Markers · 科研事件标记", "Sample Indices · 指定采样帧"] }
    StudioTextField { id: times; visible: timeSource.currentIndex === 1 || timeSource.currentIndex === 4; Layout.fillWidth: true; placeholderText: timeSource.currentIndex === 4 ? "0, 30, 60 · 采样帧序号" : "1.25, 3.125, 7.21 · 秒" }
    GridLayout {
        visible: timeSource.currentIndex === 2; columns: 2; Layout.fillWidth: true
        StudioLabel { text: "Begin · 起始秒" }
        StudioTextField { id: begin; text: "0"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; notation: DoubleValidator.StandardNotation } }
        StudioLabel { text: "End · 结束秒" }
        StudioTextField { id: end; text: panel.controller.duration.toString(); Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; notation: DoubleValidator.StandardNotation } }
        StudioLabel { text: "Interval · 间隔秒" }
        StudioTextField { id: interval; text: "1"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0.000000001; notation: DoubleValidator.StandardNotation } }
    }
    RowLayout {
        visible: timeSource.currentIndex === 4
        StudioLabel { text: "Sampling FPS · 采样率" }
        StudioTextField { id: sampleFps; text: "30"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0.001; top: 10000 } }
    }
    StudioLabel { visible: timeSource.currentIndex === 4; text: "实验时间 = 序号 ÷ 采样率；这是实验采样网格，不是视频原始 PTS。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
    StudioTextField { id: pattern; text: "frame_{index}_{time}_{view}"; Layout.fillWidth: true; placeholderText: "Filename · 文件命名规则" }
    StudioLabel { text: "{index} 序号 · {time} 实验秒 · {view} 视角。每批创建独立目录和清单，最多 1000 张；颜色、字体和尺寸沿用同一组设置。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
    StudioComboBox { id: batchFormat; Layout.fillWidth: true; model: ["PNG · 无损", "TIFF · 无损", "JPEG"] }
    StudioCheckBox { id: figure; text: "Scientific Figure · 同时生成多联科研图" }
    RowLayout {
        visible: figure.checked
        StudioLabel { text: "Columns · 列数" }
        SpinBox { id: columns; from: 1; to: 12; value: 2; palette: StudioPalette {} }
        StudioComboBox { id: figureFormat; model: ["PDF", "SVG", "PNG", "TIFF"]; Layout.fillWidth: true }
    }
    StudioLabel { visible: figure.checked; text: "自动添加 (a)、(b)… 及 9 位小数实验时间。PDF/SVG 编号为矢量文字，场景合成为嵌入位图；最多 4000 万像素。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
    StudioButton {
        text: "Export Batch… · 批量导出"; Layout.fillWidth: true
        enabled: pixelWidth.acceptableInput && pixelHeight.acceptableInput && dpi.acceptableInput && (scope.currentIndex !== 2 || panel.layerIds.length > 0)
        onClicked: panel.batchRequested(panel.batchOptions())
    }
    StudioLabel { text: frameBatch.message; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; color: appearance.colors.secondaryText }
}
