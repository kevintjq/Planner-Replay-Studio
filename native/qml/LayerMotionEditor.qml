import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: editor
    required property var controller
    spacing: 5

    readonly property var targets: {
        const items = [{ label: "主点云", kind: "main", layerId: "" }]
        if (controller.speedGauge.visible)
            items.push({ label: "速度表", kind: "speed", layerId: "" })
        for (const view of controller.cloudViews)
            items.push({ label: view.label, kind: "view", layerId: view.id })
        const videos = controller.videoLayers
        for (let index = 0; index < videos.length; ++index)
            items.push({ label: "实拍 " + (index + 1), kind: "video", layerId: videos[index].id })
        const layers = controller.sceneLayers
        for (let index = 0; index < layers.length; ++index) {
            const layer = layers[index]
            const image = layer.kind === "images"
            items.push({ label: (image ? "图片 " : "文字 ") + (index + 1),
                         kind: image ? "image" : "text", layerId: layer.id })
        }
        return items
    }

    StudioButton {
        id: expand
        Layout.fillWidth: true
        text: checked ? "收起缩窗动画" : "缩窗动画"
        checkable: true
    }
    GridLayout {
        Layout.fillWidth: true
        visible: expand.checked
        columns: 2

        StudioLabel { text: "图层" }
        StudioComboBox {
            id: target
            Layout.fillWidth: true
            model: editor.targets
            textRole: "label"
        }
        StudioLabel { text: "常态宽度 (%)"; visible: normalWidth.visible }
        SpinBox { palette: StudioPalette {}

            id: normalWidth
            from: 12
            to: 100
            value: 100
            editable: true
            visible: target.currentIndex === 0
        }
        StudioLabel { text: "放大起点（秒）" }
        StudioTextField {
            id: begin
            Layout.fillWidth: true
            text: "0.0"
            validator: DoubleValidator { bottom: 0 }
        }
        StudioLabel { text: "缩窗时长（秒）" }
        StudioTextField {
            id: duration
            Layout.fillWidth: true
            text: "0.4"
            validator: DoubleValidator { bottom: .1; top: 5 }
        }
        StudioLabel { text: "放大宽度 (%)" }
        SpinBox { palette: StudioPalette {}
 id: largeWidth; from: 5; to: 100; value: 100; editable: true }
        StudioLabel { text: "放大位置 (%)" }
        RowLayout {
            SpinBox { palette: StudioPalette {}
 id: largeX; from: 0; to: 100; value: 50; editable: true; Accessible.name: "横向位置" }
            SpinBox { palette: StudioPalette {}
 id: largeY; from: 0; to: 100; value: 50; editable: true; Accessible.name: "纵向位置" }
        }
        StudioButton {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            text: "在播放头缩窗"
            enabled: target.currentIndex >= 0 && begin.acceptableInput && duration.acceptableInput
            onClicked: {
                const selected = editor.targets[target.currentIndex]
                editor.controller.addLayerMotion(selected.kind, selected.layerId, Number.fromLocaleString(begin.text),
                    Number.fromLocaleString(duration.text), largeWidth.value / 100, largeX.value / 100,
                    largeY.value / 100, normalWidth.value / 100)
            }
        }
    }

    Repeater {
        model: expand.checked ? editor.controller.layerMotions : []
        delegate: RowLayout {
            required property int index
            required property var modelData
            Layout.fillWidth: true
            StudioButton {
                Layout.fillWidth: true
                text: {
                    const item = editor.targets.find(t => t.kind === modelData.kind && t.layerId === (modelData.layer_id ?? ""))
                    return (item?.label ?? modelData.kind) + " · " + Number(modelData.time).toFixed(2) + " s"
                }
                onClicked: editor.controller.seek(Number(modelData.time))
            }
            StudioButton { text: "删除"; onClicked: editor.controller.removeLayerMotion(index) }
        }
    }
    Connections {
        target: editor.controller
        function onRecordingChanged() { normalWidth.value = Math.round(editor.controller.mainView.width * 100) }
    }
}
