import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

RowLayout {
    id: pane
    required property var editor
    property rect selection: Qt.rect(0, 0, 0, 0)
    property string rangeId: ""
    property bool exportSettingsVisible: false
    spacing: 16
    function configure() {
        if (interval.acceptableInput && count.acceptableInput)
            editor.configure(Number(interval.text), opacity.value, strategy.currentIndex, Number(count.text))
    }
    FileDialog {
        id: frameDialog
        title: "导出轨迹工作区当前画面"
        fileMode: FileDialog.SaveFile
        defaultSuffix: selectedNameFilter.extensions[0] ?? "png"
        nameFilters: ["PNG (*.png)", "JPEG (*.jpg)", "TIFF (*.tiff)", "PDF — embedded raster (*.pdf)", "SVG — embedded raster (*.svg)"]
        onAccepted: pane.editor.exportFrame(selectedFile, {width: Number(exportWidth.text), height: Number(exportHeight.text), dpi: Number(exportDpi.text),
            background: exportBackground.currentIndex === 0 ? "transparent" : exportBackground.currentIndex === 1 ? appearance.outputColors().matteWhite : exportBackground.currentIndex === 2 ? appearance.outputColors().matteBlack : exportColor.text})
    }
    FileDialog {
        id: openDialog
        title: "打开固定机位视频"
        nameFilters: ["视频 (*.mp4 *.mov *.mkv *.avi *.webm)", "所有文件 (*)"]
        onAccepted: { pane.selection = Qt.rect(0, 0, 0, 0); pane.editor.open(selectedFile) }
    }
    FileDialog {
        id: saveDialog
        title: "保存原分辨率拖影图"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["PNG 图像 (*.png)"]
        onAccepted: pane.editor.save(selectedFile)
    }
    Shortcut { sequences: [StandardKey.Undo]; enabled: pane.visible && pane.editor.canUndo && !pane.editor.working; onActivated: pane.editor.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: pane.visible && pane.editor.canRedo && !pane.editor.working; onActivated: pane.editor.redo() }
    Shortcut { sequence: "Ctrl+Shift+E"; enabled: pane.visible && pane.editor.image.toString().length > 0 && !pane.editor.working; onActivated: frameDialog.open() }
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
        }
    }
    Rectangle {
        Layout.preferredWidth: 300
        Layout.fillHeight: true
        color: appearance.colors.panel
        radius: appearance.metrics.panelRadius
        border.color: appearance.colors.border
    ScrollView {
        ScrollBar.vertical: StudioScrollBar {}
        ScrollBar.horizontal: StudioScrollBar {}
        id: sourceScroll
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            width: sourceScroll.availableWidth
            spacing: 9
            StudioLabel { text: "Trajectory Studio · 轨迹图"; font.pixelSize: appearance.metrics.section; font.weight: Font.DemiBold }
            StudioButton { text: "打开固定机位视频"; enabled: !pane.editor.working; onClicked: openDialog.open() }
            StudioLabel { text: pane.editor.fileName; elide: Text.ElideMiddle; Layout.fillWidth: true }
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
            StudioLabel { text: pane.editor.status; Layout.fillWidth: true; wrapMode: Text.Wrap }
            ProgressBar { visible: pane.editor.working; Layout.fillWidth: true; value: pane.editor.progress }
            StudioButton { text: "取消处理"; visible: pane.editor.working; onClicked: pane.editor.cancel() }
            ColumnLayout {
                visible: pane.exportSettingsVisible; enabled: !pane.editor.working; Layout.fillWidth: true
                StudioLabel { text: "Export Frame · 独立画面导出"; font.bold: true }
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
                StudioLabel { text: "背景设置作用于画布边缘，不自动移除视频背景。PDF/SVG 当前嵌入高分辨率位图。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
                StudioButton { text: "Save Frame… · 保存当前画面"; Layout.fillWidth: true; enabled: exportWidth.acceptableInput && exportHeight.acceptableInput && exportDpi.acceptableInput && pane.editor.image.toString().length > 0; onClicked: frameDialog.open() }
            }
            StudioLabel { visible: pane.editor.previewing; text: "正在更新预览…" }
            ColumnLayout {
                visible: pane.editor.hasResult
                enabled: !pane.editor.working
                Layout.fillWidth: true
                StudioLabel { text: "Sampling（采样策略）"; font.bold: true }
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
                RowLayout {
                    StudioButton { text: "排除选中帧"; onClicked: pane.editor.editSelectedFrames(true) }
                    StudioButton { text: "恢复选中帧"; onClicked: pane.editor.editSelectedFrames(false) }
                }
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
                    StudioButton { text: "重新框选"; onClicked: pane.editor.resetPreview() }
                    StudioButton { text: "保存 PNG"; onClicked: saveDialog.open() }
                }
            }
        }
    }
    }
    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Rectangle {
            color: appearance.colors.preview; radius: 8; clip: true
            Layout.fillWidth: true; Layout.fillHeight: true
            StudioButton {
                anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 12; z: 5
                text: "📷"; Accessible.name: "Export Frame · 导出当前画面"
                enabled: pane.editor.image.toString().length > 0 && !pane.editor.working
                onClicked: pane.exportSettingsVisible = !pane.exportSettingsVisible
                StudioToolTip { visible: parent.hovered; text: "Export Frame · 完整质量画面 · Ctrl+Shift+E" }
            }
            Column {
                anchors.centerIn: parent; spacing: 12; visible: pane.editor.image.toString().length === 0
                StudioLabel { text: "Trajectory Studio"; font.pixelSize: appearance.metrics.title; color: appearance.colors.text; anchors.horizontalCenter: parent.horizontalCenter }
                StudioLabel { text: "打开固定机位视频，选择起始时间并框选无人机\n保留原始视频；采样和排除记录随项目保存"; horizontalAlignment: Text.AlignHCenter; color: appearance.colors.secondaryText }
            }
            Image {
                id: picture
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
                    enabled: !pane.editor.working && !pane.editor.hasResult && picture.status === Image.Ready
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
        }
        TrajectoryTimeline { id: timeline; editor: pane.editor; visible: pane.editor.hasResult; Layout.fillWidth: true }
    }
}
