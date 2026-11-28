import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

RowLayout {
    id: pane
    objectName: "trajectoryPane"
    required property var editor
    property alias canvas: trajectoryCanvas
    readonly property bool textInputFocused: Window.window?.activeFocusItem instanceof TextInput || Window.window?.activeFocusItem instanceof TextEdit
    property rect selection: Qt.rect(0, 0, 0, 0)
    property string rangeId: ""
    property bool exportSettingsVisible: false
    property bool maskEditorVisible: false
    property bool layoutEditing: false
    property url backgroundFile: ""
    spacing: 16
    function configure() {
        if (interval.acceptableInput && count.acceptableInput)
            editor.configure(Number(interval.text), opacity.value, strategy.currentIndex, Number(count.text))
    }
    function frameOptions() {
        return {width: Number(exportWidth.text), height: Number(exportHeight.text), dpi: Number(exportDpi.text),
            scope: exportScope.currentIndex === 0 ? "canvas" : "layers",
            layerIds: exportScope.currentIndex === 0 ? [] : pane.editor.canvas.selection,
            background: exportBackground.currentIndex === 0 ? "transparent" : exportBackground.currentIndex === 1 ? appearance.outputColors().matteWhite : exportBackground.currentIndex === 2 ? appearance.outputColors().matteBlack : exportColor.text}
    }
    function configureBackground() { editor.setBackgroundReference(backgroundFile, backgroundTime.text, registerCamera.checked) }
    FileDialog {
        id: backgroundDialog
        title: "选择无无人机的同尺寸背景 / Clean background reference"
        nameFilters: ["Images (*.png *.jpg *.jpeg *.tiff *.bmp)"]
        onAccepted: { pane.backgroundFile = selectedFile; pane.configureBackground() }
    }
    FileDialog {
        id: frameDialog
        title: "导出轨迹工作区当前画面"
        fileMode: FileDialog.SaveFile
        defaultSuffix: selectedNameFilter.extensions[0] ?? "png"
        nameFilters: ["PNG (*.png)", "JPEG (*.jpg)", "TIFF (*.tiff)", "PDF — vector and images (*.pdf)", "SVG — vector and images (*.svg)"]
        onAccepted: pane.editor.exportFrame(selectedFile, pane.frameOptions())
    }
    FolderDialog {
        id: batchFolder
        title: "批量画面输出文件夹 / Batch output folder"
        onAccepted: {
            const options = pane.frameOptions()
            options.scope = "canvas"
            options.layerIds = []
            options.content = batchContent.currentIndex === 0 ? "source" : "trajectory"
            options.mode = ["manual", "interval", "samples", "tracked"][batchMode.currentIndex]
            options.times = batchTimes.text
            options.sampleFps = Number(batchFps.text)
            options.begin = Number(batchBegin.text)
            options.end = Number(batchEnd.text)
            options.interval = Number(batchInterval.text)
            options.format = ["png", "jpg", "tiff"][batchFormat.currentIndex]
            options.pattern = batchPattern.text
            options.figure = batchFigure.checked
            options.figureFormat = ["pdf", "svg", "png", "tiff"][figureFormat.currentIndex]
            options.columns = Number(figureColumns.text)
            pane.editor.exportFrames(selectedFolder, options)
        }
    }
    FileDialog {
        id: openDialog
        title: "打开固定机位视频"
        nameFilters: ["视频 (*.mp4 *.mov *.mkv *.avi *.webm)", "所有文件 (*)"]
        onAccepted: { pane.layoutEditing=false; pane.selection = Qt.rect(0, 0, 0, 0); pane.editor.open(selectedFile) }
    }
    FileDialog {
        id: saveDialog
        title: "保存原分辨率拖影图"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["PNG 图像 (*.png)"]
        onAccepted: pane.editor.save(selectedFile)
    }
    Shortcut { sequences: [StandardKey.Undo]; enabled: pane.visible && !pane.textInputFocused && pane.editor.canUndo && !pane.editor.working; onActivated: pane.editor.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: pane.visible && !pane.textInputFocused && pane.editor.canRedo && !pane.editor.working; onActivated: pane.editor.redo() }
    Shortcut { sequence: "Ctrl+Shift+E"; enabled: pane.visible && pane.canSaveFrame && !pane.editor.working; onActivated: frameDialog.open() }
    readonly property bool canSaveFrame: exportScope.currentIndex === 0 ? pane.editor.canExport : pane.editor.canvas.selection.length > 0
    Connections {
        target: pane.editor
        function onChanged() {
            const s = pane.editor.settings
            if (!start.activeFocus) start.text = (s.start ?? 0).toFixed(3)
            if (!end.activeFocus) end.text = s.end ?? ""
            if (!interval.activeFocus) interval.text = (s.interval ?? .5).toFixed(3)
            if (!count.activeFocus) count.text = String(s.count ?? 12)
            if (!opacity.pressed) opacity.value = s.opacity ?? .9
            strategy.currentIndex = s.mode ?? 0
            showLine.checked = s.edits?.show_line ?? false
            lineOnly.checked = s.edits?.line_only ?? false
            skipInvalid.checked = s.edits?.skip_invalid ?? false
            registerCamera.checked = s.register_camera ?? false
            if (!backgroundTime.activeFocus) backgroundTime.text = (s.background_seconds ?? -1) < 0 ? "" : Number(s.background_seconds).toFixed(3)
        }
    }
    Rectangle {
        Layout.preferredWidth: 300
        Layout.fillHeight: true
        color: appearance.colors.panel
        radius: appearance.metrics.panelRadius
        border.color: appearance.colors.border
    StudioScrollView {
        id: sourceScroll
        objectName: "trajectorySourceScroll"
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            width: sourceScroll.availableWidth
            spacing: 9
            StudioLabel { text: "Trajectory Studio · 轨迹图"; font.pixelSize: appearance.metrics.section; font.weight: Font.DemiBold }
            CanvasLayers { Layout.fillWidth:true;controller:pane.editor;onLayerSelected:id=>trajectoryCanvas.forceActiveFocus() }
            StudioButton { text: "打开固定机位视频"; enabled: !pane.editor.working; onClicked: openDialog.open() }
            StudioLabel { text: pane.editor.fileName; elide: Text.ElideMiddle; Layout.fillWidth: true }
            RowLayout {
                StudioButton { text: "移除视频"; enabled: !pane.editor.working && pane.editor.fileName.length > 0; onClicked: pane.editor.removeVideo() }
                StudioButton { text: "撤销"; enabled: !pane.editor.working && pane.editor.canUndo; onClicked: pane.editor.undo() }
                StudioButton { text: "重做"; enabled: !pane.editor.working && pane.editor.canRedo; onClicked: pane.editor.redo() }
            }
            GridLayout {
                columns: 2
                enabled: !pane.editor.working && pane.editor.fileName.length > 0
                StudioLabel { text: "开始时间（秒）" }
                StudioTextField { id: start; text: "0.000"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } }
                StudioLabel { text: "结束时间（秒）" }
                StudioTextField { id: end; placeholderText: "留空至视频结束"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } }
            }
            RowLayout {
                StudioButton {
                    text: "查看起始帧"
                    enabled: !pane.editor.working && pane.editor.fileName.length > 0 && start.acceptableInput
                    onClicked: { pane.selection = Qt.rect(0, 0, 0, 0); pane.editor.preview(Number(start.text)) }
                }
                StudioButton {
                    text: "生成拖影"
                    enabled: !pane.editor.working && !pane.editor.hasResult && pane.selection.width > 0 && pane.selection.height > 0
                    onClicked: pane.editor.track(pane.selection, end.text)
                }
            }
            ColumnLayout {
                visible: !pane.editor.hasResult
                enabled: !pane.editor.working
                Layout.fillWidth: true
                StudioLabel { text: "Background · 背景参考"; font.bold: true }
                StudioLabel { text: "默认从完整跟踪区间建立真实背景。目标长期停留时请选择不含无人机的背景帧或图片。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
                RowLayout {
                    StudioTextField { id: backgroundTime; placeholderText: "背景源时间（秒，可留空）"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } onEditingFinished: pane.configureBackground() }
                    StudioButton { text: "图片…"; onClicked: backgroundDialog.open() }
                }
                StudioLabel { visible: pane.backgroundFile.toString().length > 0; text: pane.backgroundFile.toString().split('/').pop(); elide: Text.ElideMiddle; Layout.fillWidth: true }
                StudioButton { visible: pane.backgroundFile.toString().length > 0; text: "清除背景图片"; onClicked: { pane.backgroundFile = ""; pane.configureBackground() } }
                StudioCheckBox { id: registerCamera; text: "配准轻微相机抖动（平移）"; onClicked: pane.configureBackground() }
            }
            StudioLabel { text: pane.editor.status; Layout.fillWidth: true; wrapMode: Text.Wrap }
            ProgressBar { visible: pane.editor.working; Layout.fillWidth: true; value: pane.editor.progress }
            StudioButton { text: "取消处理"; visible: pane.editor.working; onClicked: pane.editor.cancel() }
            ColumnLayout {
                visible: pane.exportSettingsVisible; enabled: !pane.editor.working; Layout.fillWidth: true
                StudioLabel { text: "Export Frame · 独立画面导出"; font.bold: true }
                StudioComboBox {
                    id: exportScope; Layout.fillWidth: true
                    model: ["Export Canvas · 完整画布", "Export Selected Layers · 选中图层"]
                }
                StudioLabel {
                    visible: exportScope.currentIndex === 1
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "Selected: " + pane.editor.canvas.selection.length + " · 仅输出选中图层，保留画布坐标。单独轨迹线或文字不代表无人机拖影验收通过。"
                    color: appearance.colors.secondaryText
                }
                StudioLabel { text: "读取完整源画面和当前编辑快照，保留原有预览与编辑。视频位图分辨率受源素材限制；提高输出尺寸不会增加源视频细节。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
                StudioComboBox {
                    Layout.fillWidth: true; model: ["1080P", "2K", "4K", "8K", "Custom · 自定义比例"]
                    onActivated: if (currentIndex < 4) { exportWidth.text = [1920, 2560, 3840, 7680][currentIndex]; exportHeight.text = [1080, 1440, 2160, 4320][currentIndex] }
                }
                GridLayout {
                    columns: 2; Layout.fillWidth: true
                    StudioLabel { text: "Width · 像素宽" }
                    StudioTextField { id: exportWidth; text: "1920"; Layout.fillWidth: true; validator: IntValidator { bottom: 16; top: 8192 } }
                    StudioLabel { text: "Height · 像素高" }
                    StudioTextField { id: exportHeight; text: "1080"; Layout.fillWidth: true; validator: IntValidator { bottom: 16; top: 8192 } }
                    StudioLabel { text: "DPI" }
                    StudioTextField { id: exportDpi; text: "300"; Layout.fillWidth: true; validator: IntValidator { bottom: 36; top: 2400 } }
                }
                RowLayout { Repeater { model: [300, 600, 1200]; delegate: StudioButton { required property int modelData; text: modelData; onClicked: exportDpi.text = modelData } } }
                StudioComboBox { id: exportBackground; Layout.fillWidth: true; model: ["Transparent · 透明边缘", "White · 纯白边缘", "Black · 纯黑边缘", "Custom · 自定义边缘颜色"] }
                StudioTextField { id: exportColor; visible: exportBackground.currentIndex === 3; text: "#FFFFFF"; Layout.fillWidth: true }
                StudioLabel { text: "背景设置作用于画布边缘，不自动移除视频背景。单幅 PDF/SVG 保留轨迹、标记及科研文字矢量；真实视频背景与无人机前景嵌入位图。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
                StudioButton { text: "Save Frame… · 保存当前画面"; Layout.fillWidth: true; enabled: exportWidth.acceptableInput && exportHeight.acceptableInput && exportDpi.acceptableInput && pane.canSaveFrame; onClicked: frameDialog.open() }
                StudioLabel {
                    visible: !!pane.editor.frameExportReport.scope
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: pane.editor.frameExportReport.success
                        ? "Exported · 已输出 " + pane.editor.frameExportReport.scope + " · UAV instances: " + (pane.editor.frameExportReport.composedUavInstances ?? 0)
                        : (pane.editor.frameExportReport.error || "")
                    color: appearance.colors.secondaryText
                }
                StudioLabel { text: "Export Keyframes · 批量画面"; font.bold: true; Layout.topMargin: 8 }
                StudioComboBox { id: batchContent; Layout.fillWidth: true; model: ["Source Frames · 源视频帧", "Trajectory Snapshots · 累积轨迹图"] }
                StudioLabel {
                    text: batchContent.currentIndex === 0 ? "以源视频秒数选取完整解码帧；清单区分请求时间与实际源帧时间。" : "保留当前背景、采样与编辑，仅合成截至各指定时刻的轨迹；不包含未来拖影。"
                    wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText
                }
                StudioComboBox { id: batchMode; Layout.fillWidth: true; model: ["Manual Times · 手动时间点", "Time Interval · 时间区间", "Sampling Grid · 采样序号", "Tracked Samples · 当前轨迹采样帧"] }
                StudioTextField { id: batchTimes; visible: batchMode.currentIndex === 0 || batchMode.currentIndex === 2; text: "0, 1"; placeholderText: batchMode.currentIndex === 0 ? "秒：0, 1.25, 3.5" : "序号：0, 30, 60"; Layout.fillWidth: true }
                StudioButton { text: "Add Current Time · 添加预览时间"; visible: batchMode.currentIndex === 0; onClicked: batchTimes.text += (batchTimes.text.trim().length > 0 ? ", " : "") + pane.editor.startTime.toFixed(9) }
                RowLayout {
                    visible: batchMode.currentIndex === 2
                    StudioLabel { text: "Grid FPS · 采样率" }
                    StudioTextField { id: batchFps; text: "30"; Layout.fillWidth: true; validator: DoubleValidator { bottom: .001; top: 10000; locale: "C" } }
                }
                StudioLabel { visible: batchMode.currentIndex === 2; text: "序号 ÷ 采样率为请求秒数，不保证与源视频 PTS 序号相同。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
                GridLayout {
                    visible: batchMode.currentIndex === 1; columns: 2; Layout.fillWidth: true
                    StudioLabel { text: "Begin · 秒" }
                    StudioTextField { id: batchBegin; text: "0"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } }
                    StudioLabel { text: "End · 秒" }
                    StudioTextField { id: batchEnd; text: "1"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } }
                    StudioLabel { text: "Interval · 秒" }
                    StudioTextField { id: batchInterval; text: "0.5"; Layout.fillWidth: true; validator: DoubleValidator { bottom: .000001; locale: "C" } }
                }
                StudioComboBox { id: batchFormat; Layout.fillWidth: true; model: ["PNG", "JPG", "TIFF"] }
                StudioTextField { id: batchPattern; text: "frame_{index}_{time}"; Layout.fillWidth: true; placeholderText: "{index} · {time} · {view}" }
                StudioCheckBox { id: batchFigure; text: "Scientific Figure · 生成多联图" }
                RowLayout {
                    visible: batchFigure.checked
                    StudioComboBox { id: figureFormat; Layout.fillWidth: true; model: ["PDF", "SVG", "PNG", "TIFF"] }
                    StudioLabel { text: "列数" }
                    StudioTextField { id: figureColumns; text: "2"; Layout.preferredWidth: 42; validator: IntValidator { bottom: 1; top: 32 } }
                }
                StudioLabel { visible: batchFigure.checked; text: "统一尺寸、配色及编辑快照，自动编号和时间标签。多联图最多 4000 万像素；PDF/SVG 编号及时间为矢量文字，视频与合成为位图。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
                StudioButton {
                    text: "Export Batch… · 批量导出"; Layout.fillWidth: true
                    enabled: pane.editor.image.toString().length > 0 && exportWidth.acceptableInput && exportHeight.acceptableInput && exportDpi.acceptableInput &&
                        (batchContent.currentIndex === 0 || pane.editor.canExport) && (batchMode.currentIndex !== 3 || pane.editor.hasResult) &&
                        (batchMode.currentIndex !== 2 || batchFps.acceptableInput) && (batchMode.currentIndex !== 1 || (batchBegin.acceptableInput && batchEnd.acceptableInput && batchInterval.acceptableInput)) &&
                        (!batchFigure.checked || figureColumns.acceptableInput)
                    onClicked: batchFolder.open()
                }
                StudioLabel { text: "每次创建独立子文件夹；取消时保留已完成图片及 manifest.json。科研事件来源尚未接入该工作区。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
            }
            StudioLabel { visible: pane.editor.previewing; text: "正在更新预览…" }
            ColumnLayout {
                visible: pane.editor.hasResult
                enabled: !pane.editor.working
                Layout.fillWidth: true
                StudioLabel { text: "Sampling（采样策略）"; font.bold: true }
                StudioLabel {
                    text: {
                        const c = pane.editor.foregroundCounts
                        return "跟踪 " + (c.tracked ?? 0) + " · 采样 " + (c.sampled ?? 0) + "\n自动提取成功 " + (c.extracted ?? 0) + " / 失败 " + (c.extractionFailed ?? 0) +
                            "\n采样有效 " + (c.successful ?? 0) + " / 失败 " + (c.failed ?? 0) + " / 过滤 " + (c.filtered ?? 0) + "\n最终可见无人机 " + (c.composed ?? 0)
                    }
                    wrapMode: Text.Wrap; Layout.fillWidth: true
                    color: pane.editor.canExport ? appearance.colors.text : appearance.colors.excluded
                }
                StudioCheckBox { id: skipInvalid; text: "跳过无效帧（仍须有可见无人机）"; onClicked: pane.editor.setSkipInvalid(checked) }
                StudioButton { text: pane.maskEditorVisible ? "收起蒙版编辑" : "原图 / 蒙版 / 前景 · 画笔修正"; Layout.fillWidth: true; onClicked: pane.maskEditorVisible = !pane.maskEditorVisible }
                StudioComboBox {
                    id: strategy
                    Layout.fillWidth: true
                    model: ["固定间隔", "固定数量", "仅手动选帧", "原有自动间距"]
                    onActivated: pane.configure()
                }
                RowLayout {
                    visible: strategy.currentIndex === 0
                    StudioLabel { text: "Interval（秒）" }
                    StudioTextField {
                        id: interval; text: "0.500"; Layout.fillWidth: true
                        validator: DoubleValidator { bottom: .1; locale: "C" }
                        onEditingFinished: pane.configure()
                        StudioToolTip { visible: interval.hovered; text: "最小 0.1 秒；采用不早于目标时刻的实际跟踪帧，不生成虚构源帧。" }
                    }
                }
                StudioSlider {
                    visible: strategy.currentIndex === 0
                    Layout.fillWidth: true
                    from: .1; to: 10; stepSize: .1
                    value: Math.min(10, Number(interval.text))
                    onMoved: { interval.text = value.toFixed(1); settingsTimer.restart() }
                }
                RowLayout {
                    visible: strategy.currentIndex === 1
                    StudioLabel { text: "Count（数量）" }
                    StudioTextField { id: count; text: "12"; Layout.fillWidth: true; validator: IntValidator { bottom: 1; top: 1000000 } onEditingFinished: pane.configure() }
                }
                StudioLabel { text: "Global Opacity（全局不透明度）" }
                RowLayout {
                    StudioSlider { id: opacity; from: 0; to: 1; stepSize: .01; value: .9; Layout.fillWidth: true; onMoved: settingsTimer.restart() }
                    StudioLabel { text: Math.round(opacity.value * 100) + "%" }
                }
                Timer { id: settingsTimer; interval: 60; onTriggered: pane.configure() }
                StudioLabel { text: "最终不透明度 = 全局 × 单帧"; font.pixelSize: 11 }
                StudioCheckBox { id: showLine; text: "显示跟踪轨迹线"; onClicked: pane.editor.setShowLine(checked) }
                StudioCheckBox { id: lineOnly; text: "仅轨迹线（明确不输出无人机）"; onClicked: pane.editor.setLineOnly(checked) }
                RowLayout {
                    StudioButton { text: "排除选中帧"; onClicked: pane.editor.editSelectedFrames(true) }
                    StudioButton { text: "恢复选中帧"; onClicked: pane.editor.editSelectedFrames(false) }
                }
                StudioButton { text: "删除选中采样帧 · Delete"; Layout.fillWidth: true; onClicked: pane.editor.deleteSelectedFrames() }
                StudioLabel { text: "Frame Opacity（选中帧不透明度）" }
                RowLayout {
                    StudioSlider { id: frameAlpha; from: 0; to: 1; stepSize: .01; value: 1; Layout.fillWidth: true; onMoved: frameTimer.restart() }
                    StudioLabel { text: Math.round(frameAlpha.value * 100) + "%" }
                }
                Timer { id: frameTimer; interval: 60; onTriggered: pane.editor.editSelectedFrames(false, frameAlpha.value) }
                RowLayout {
                    StudioTextField { id: manual; placeholderText: "源时间（秒）"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } }
                    StudioButton { text: "手动加帧"; enabled: manual.acceptableInput; onClicked: pane.editor.addManualFrame(Number(manual.text)) }
                }
                StudioLabel { text: "Excluded Ranges（多段排除）"; font.bold: true }
                StudioLabel { text: "左闭右开 [入点, 出点)，重叠按类型求并集"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                GridLayout {
                    columns: 2
                    StudioLabel { text: "入点（秒）" }
                    StudioTextField { id: rangeBegin; text: "0.000"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } }
                    StudioLabel { text: "出点（秒）" }
                    StudioTextField { id: rangeEnd; text: "1.000"; Layout.fillWidth: true; validator: DoubleValidator { bottom: 0; locale: "C" } }
                }
                StudioComboBox { id: parts; Layout.fillWidth: true; model: ["仅排除拖影帧（保留线）", "仅排除轨迹线（保留帧）", "同时排除帧和线"] }
                RowLayout {
                    StudioButton { text: "取时间轴选区"; onClicked: { rangeBegin.text = timeline.inTime.toFixed(3); rangeEnd.text = timeline.outTime.toFixed(3) } }
                    StudioButton { text: "添加区间"; enabled: rangeBegin.acceptableInput && rangeEnd.acceptableInput; onClicked: pane.editor.addExcludedRange(Number(rangeBegin.text), Number(rangeEnd.text), parts.currentIndex + 1) }
                }
                ListView {
                    id: ranges
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(170, contentHeight)
                    clip: true
                    model: pane.editor.excludedRanges
                    ScrollBar.vertical: StudioScrollBar {}
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ranges.width
                        text: modelData.begin.toFixed(3) + " – " + modelData.end.toFixed(3) + " s · " + ["", "帧", "线", "帧+线"][modelData.parts]
                        highlighted: pane.rangeId === modelData.id
                        onClicked: {
                            pane.rangeId = modelData.id; rangeBegin.text = modelData.begin.toFixed(3)
                            rangeEnd.text = modelData.end.toFixed(3); parts.currentIndex = modelData.parts - 1
                            timeline.focusTime(modelData.begin)
                        }
                    }
                }
                RowLayout {
                    enabled: pane.rangeId.length > 0
                    StudioButton { text: "修改"; onClicked: pane.editor.editExcludedRange(pane.rangeId, Number(rangeBegin.text), Number(rangeEnd.text), parts.currentIndex + 1) }
                    StudioButton { text: "分割"; onClicked: pane.editor.splitExcludedRange(pane.rangeId, timeline.cursor) }
                    StudioButton { text: "恢复"; onClicked: { pane.editor.removeExcludedRange(pane.rangeId); pane.rangeId = "" } }
                }
                RowLayout {
                    StudioButton { text: "设为生成范围"; onClicked: pane.editor.setGenerationRange(Number(rangeBegin.text), Number(rangeEnd.text)) }
                    StudioButton { text: "重置范围"; onClicked: pane.editor.setGenerationRange(-1, -1) }
                }
                RowLayout {
                    StudioButton { text: "撤销"; enabled: pane.editor.canUndo; onClicked: pane.editor.undo() }
                    StudioButton { text: "重做"; enabled: pane.editor.canRedo; onClicked: pane.editor.redo() }
                    StudioButton { text: "恢复全部排除"; onClicked: pane.editor.restoreFrames() }
                }
                RowLayout {
                    StudioButton { text: "重新框选"; onClicked: { pane.layoutEditing=false; pane.editor.resetPreview() } }
                    StudioButton { text: "保存 PNG"; enabled: pane.editor.canExport; onClicked: saveDialog.open() }
                }
                StudioButton { text: "清空生成结果（可撤销）"; Layout.fillWidth: true; onClicked: pane.editor.clearGenerated() }
            }
        }
    }
    }
    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Rectangle {
            id: previewArea
            color: appearance.colors.preview; radius: 8; clip: true
            Layout.fillWidth: true; Layout.fillHeight: true
            StudioButton {
                anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 12; z: 5
                text: "📷"; Accessible.name: "Export Frame · 导出当前画面"
                enabled: pane.editor.image.toString().length > 0 && !pane.editor.working
                onClicked: pane.exportSettingsVisible = !pane.exportSettingsVisible
                StudioToolTip { visible: parent.hovered; text: "Export Frame · 完整质量画面 · Ctrl+Shift+E" }
            }
            StudioButton {
                anchors.top:parent.top;anchors.left:parent.left;anchors.margins:12;z:5
                visible:!pane.editor.hasResult
                text:pane.layoutEditing ? "Select UAV · 框选无人机" : "Layout · 编辑画布"
                onClicked:pane.layoutEditing=!pane.layoutEditing
            }
            Column {
                anchors.centerIn: parent; spacing: 12; visible: pane.editor.image.toString().length === 0 && pane.editor.canvas.layers.length === 0
                StudioLabel { text: "Trajectory Studio"; font.pixelSize: appearance.metrics.title; color: appearance.colors.text; anchors.horizontalCenter: parent.horizontalCenter }
                StudioLabel { text: "打开固定机位视频，选择起始时间并框选无人机\n保留原始视频；采样和排除记录随项目保存"; horizontalAlignment: Text.AlignHCenter; color: appearance.colors.secondaryText }
            }
            Image {
                id: picture
                visible: !pane.editor.hasResult && !pane.layoutEditing
                anchors.fill: parent; anchors.margins: 8
                source: pane.editor.image
                fillMode: Image.PreserveAspectFit
                cache: false
                readonly property real contentLeft: (width - paintedWidth) / 2
                readonly property real contentTop: (height - paintedHeight) / 2
                Rectangle {
                    visible: !pane.editor.hasResult && pane.selection.width > 0 && pane.selection.height > 0
                    x: picture.contentLeft + pane.selection.x * picture.paintedWidth
                    y: picture.contentTop + pane.selection.y * picture.paintedHeight
                    width: picture.paintedWidth * pane.selection.width
                    height: picture.paintedHeight * pane.selection.height
                    color: appearance.colors.selection; border.color: appearance.colors.accent; border.width: 2
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !pane.editor.working && !pane.editor.hasResult && !pane.layoutEditing && picture.status === Image.Ready
                    cursorShape: enabled ? Qt.CrossCursor : Qt.ArrowCursor
                    property point origin
                    function pointAt(x, y) {
                        return Qt.point(Math.max(0, Math.min(1, (x - picture.contentLeft) / picture.paintedWidth)),
                                        Math.max(0, Math.min(1, (y - picture.contentTop) / picture.paintedHeight)))
                    }
                    onPressed: mouse => { origin = pointAt(mouse.x, mouse.y); pane.selection = Qt.rect(origin.x, origin.y, 0, 0) }
                    onPositionChanged: mouse => {
                        if (!pressed) return
                        const current = pointAt(mouse.x, mouse.y)
                        pane.selection = Qt.rect(Math.min(origin.x, current.x), Math.min(origin.y, current.y), Math.abs(origin.x - current.x), Math.abs(origin.y - current.y))
                    }
                }
            }
            TrajectoryCanvas {
                id: trajectoryCanvas
                objectName: "trajectoryEditableCanvas"
                anchors.centerIn: parent
                width: Math.max(1,Math.min(parent.width-16,(parent.height-16)*controller.canvas.outputWidth/controller.canvas.outputHeight))
                height: width*controller.canvas.outputHeight/controller.canvas.outputWidth
                controller: pane.editor
                visible: pane.editor.hasResult || pane.layoutEditing || pane.editor.image.toString().length === 0
            }
        }
        Rectangle {
            visible: pane.editor.hasResult && pane.maskEditorVisible
            Layout.fillWidth: true; Layout.preferredHeight: 250
            color: appearance.colors.panel; radius: appearance.metrics.panelRadius; border.color: appearance.colors.border
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 8; spacing: 5
                RowLayout {
                    StudioComboBox { id: maskView; model: ["Original · 原始裁剪", "Mask · 黑白蒙版", "Foreground · 透明前景"]; Layout.preferredWidth: 190 }
                    StudioComboBox { id: maskTool; model: ["画笔 · 保留", "橡皮 · 移除"]; Layout.preferredWidth: 115 }
                    StudioLabel { text: "画笔" }
                    StudioSlider { id: brushRadius; from: .005; to: .2; value: .035; Layout.preferredWidth: 120 }
                    StudioButton { text: "恢复自动蒙版"; enabled: !pane.editor.working; onClicked: pane.editor.resetMask() }
                    Item { Layout.fillWidth: true }
                }
                StudioLabel {
                    id: maskStatus
                    readonly property var selectedMask: pane.editor.maskImages
                    text: {
                        if (!selectedMask.source) return "在下方缩略图选择一帧，再检查或绘制蒙版；白色保留，黑色移除。"
                        const origin = selectedMask.manuallyEdited ? "已手动修改 / Manually edited" : "自动结果 / Automatic result"
                        const current = !selectedMask.currentMaskAvailable ? "当前蒙版不可读取 / Current mask unavailable" :
                            selectedMask.currentAlphaPixels > 0 ? "当前蒙版含可见像素，需检查边缘 / Visible mask pixels; inspect edges" : "当前蒙版为空 / Current mask empty"
                        return Number(selectedMask.seconds).toFixed(3) + " s · " + origin + "\n" + current +
                            (selectedMask.reason ? "\n原自动提取原因 / Original automatic reason: " + selectedMask.reason : "")
                    }
                    wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText
                }
                StudioLabel {
                    text: "先单击下方缩略图选择蒙版帧；画布图层选择独立。仅在实际裁剪图内起笔，拖出边缘即结束。 / Select a frame below; canvas selection is separate."
                    wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText
                }
                Image {
                    id: maskPicture
                    Layout.fillWidth: true; Layout.fillHeight: true
                    source: pane.editor.maskImages[["source", "mask", "sprite"][maskView.currentIndex]] ?? ""
                    fillMode: Image.PreserveAspectFit; cache: false; asynchronous: true
                    readonly property real leftEdge: (width-paintedWidth)/2
                    readonly property real topEdge: (height-paintedHeight)/2
                    MouseArea {
                        id: maskPaintArea
                        anchors.fill: parent; enabled: !pane.editor.working && maskPicture.status === Image.Ready
                        cursorShape: Qt.CrossCursor
                        property var stroke: []
                        property bool drawing: false
                        function insideImage(x, y) {
                            return maskPicture.paintedWidth > 0 && maskPicture.paintedHeight > 0 &&
                                x >= maskPicture.leftEdge && x <= maskPicture.leftEdge+maskPicture.paintedWidth &&
                                y >= maskPicture.topEdge && y <= maskPicture.topEdge+maskPicture.paintedHeight
                        }
                        function pointAt(x, y) {
                            return Qt.point((x-maskPicture.leftEdge)/Math.max(1, maskPicture.paintedWidth),
                                            (y-maskPicture.topEdge)/Math.max(1, maskPicture.paintedHeight))
                        }
                        function exitPoint(point) {
                            const previous = stroke[stroke.length-1]
                            const dx = point.x-previous.x, dy = point.y-previous.y
                            let amount = 1
                            if (point.x < 0 && dx < 0) amount = Math.min(amount, -previous.x/dx)
                            if (point.x > 1 && dx > 0) amount = Math.min(amount, (1-previous.x)/dx)
                            if (point.y < 0 && dy < 0) amount = Math.min(amount, -previous.y/dy)
                            if (point.y > 1 && dy > 0) amount = Math.min(amount, (1-previous.y)/dy)
                            return Qt.point(Math.max(0, Math.min(1, previous.x+amount*dx)),
                                            Math.max(0, Math.min(1, previous.y+amount*dy)))
                        }
                        function finishStroke(point) {
                            if (!drawing) return
                            stroke.push(point)
                            const points = stroke
                            stroke = []; drawing = false
                            pane.editor.paintMask(points, brushRadius.value, maskTool.currentIndex === 1)
                        }
                        onPressed: mouse => {
                            stroke = []; drawing = insideImage(mouse.x, mouse.y)
                            if (!drawing) { mouse.accepted = false; return }
                            stroke = [pointAt(mouse.x, mouse.y)]
                        }
                        onPositionChanged: mouse => {
                            if (!pressed || !drawing) return
                            const point = pointAt(mouse.x, mouse.y)
                            if (insideImage(mouse.x, mouse.y)) stroke.push(point)
                            else finishStroke(exitPoint(point))
                        }
                        onReleased: mouse => {
                            if (!drawing) return
                            const point = pointAt(mouse.x, mouse.y)
                            finishStroke(insideImage(mouse.x, mouse.y) ? point : exitPoint(point))
                        }
                        onCanceled: { stroke = []; drawing = false }
                    }
                }
            }
        }
        TrajectoryTimeline { id: timeline; editor: pane.editor; visible: pane.editor.hasResult; Layout.fillWidth: true }
    }
    Rectangle {
        Layout.preferredWidth: 280; Layout.fillHeight:true
        color:appearance.colors.panel;radius:appearance.metrics.panelRadius;border.color:appearance.colors.border
        StudioScrollView {
            id: layerInspector
            objectName: "trajectoryLayerScroll"
            anchors.fill:parent;anchors.margins:12;contentWidth:availableWidth;clip:true
            ColumnLayout {
                width:layerInspector.availableWidth;spacing:9
                StudioLabel { text:"Inspector · 图层属性";font.pixelSize:appearance.metrics.section;font.bold:true }
                CanvasTransformEditor { Layout.fillWidth:true;controller:pane.editor;canvas:trajectoryCanvas }
                ScientificElementInspector { Layout.fillWidth:true;controller:pane.editor;onElementAdded:pane.layoutEditing=true }
                TrajectoryStyleInspector { Layout.fillWidth:true;controller:pane.editor }
                StudioLabel { text:"Trajectory Elements · 轨迹元素";font.bold:true }
                StudioButton { Layout.fillWidth:true;text:"Add Trajectory Line · 添加轨迹线";enabled:pane.editor.hasResult;onClicked:pane.editor.setShowLine(true) }
                StudioButton { Layout.fillWidth:true;text:"Add Start · 添加起点";enabled:pane.editor.hasResult;onClicked:pane.editor.setEndpointStyle(false,{enabled:true}) }
                StudioButton { Layout.fillWidth:true;text:"Add Goal · 添加终点";enabled:pane.editor.hasResult;onClicked:pane.editor.setEndpointStyle(true,{enabled:true}) }
            }
        }
    }
}
