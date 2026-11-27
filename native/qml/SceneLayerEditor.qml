import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ColumnLayout {
    id: editor
    required property var controller
    property string selectedId: ""
    readonly property int selectedIndex: controller.sceneLayers.findIndex(layer => layer.id === selectedId)
    readonly property var selectedLayer: controller.sceneLayers[Math.max(0, selectedIndex)] ?? null

    FileDialog {
        id: imageDialog
        title: "添加图片"
        nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp *.bmp)"]
        onAccepted: editor.controller.addSceneImage(selectedFile)
    }

    RowLayout {
        Layout.fillWidth: true
        StudioButton { text: "添加图片"; enabled: !editor.controller.loading; onClicked: imageDialog.open() }
        StudioButton { text: "添加文字"; enabled: !editor.controller.loading; onClicked: addText.visible = !addText.visible }
    }

    RowLayout {
        id: addText
        visible: false
        Layout.fillWidth: true
        StudioTextField { id: newText; Layout.fillWidth: true; placeholderText: "叠加文字" }
        StudioButton {
            text: "加入"
            enabled: newText.text.trim().length > 0 && !editor.controller.loading
            onClicked: {
                editor.controller.addSceneText(newText.text)
                addText.visible = false
                newText.clear()
            }
        }
    }

    StudioComboBox {
        Layout.fillWidth: true
        model: editor.controller.sceneLayers
        visible: count > 0
        currentIndex: Math.max(0, editor.selectedIndex)
        textRole: "label"
        onActivated: (index) => editor.selectedId = editor.controller.sceneLayers[index].id
    }

    ColumnLayout {
        id: fields
        Layout.fillWidth: true
        visible: editor.selectedLayer !== null
        enabled: !editor.controller.loading
        readonly property bool picture: editor.selectedLayer?.kind === "images"

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            StudioLabel { text: "开始（秒）" }
            StudioTextField { id: start; Layout.fillWidth: true; text: editor.selectedLayer?.start.toString() ?? "" }
            StudioLabel { text: "结束（秒）" }
            StudioTextField { id: end; Layout.fillWidth: true; text: editor.selectedLayer?.end.toString() ?? "" }
            StudioLabel { text: "图片宽度（%）"; visible: fields.picture }
            StudioTextField { id: widthPercent; Layout.fillWidth: true; text: ((editor.selectedLayer?.width ?? .28) * 100).toFixed(1); visible: fields.picture }
            StudioLabel { text: "文字"; visible: !fields.picture }
            StudioTextField { id: textValue; Layout.fillWidth: true; text: editor.selectedLayer?.text ?? ""; visible: !fields.picture }
            StudioLabel { text: "字号"; visible: !fields.picture }
            StudioTextField { id: size; Layout.fillWidth: true; text: editor.selectedLayer?.size.toString() ?? ""; visible: !fields.picture }
            StudioLabel { text: "文字颜色"; visible: !fields.picture }
            StudioTextField { id: color; Layout.fillWidth: true; text: editor.selectedLayer?.color ?? ""; visible: !fields.picture }
        }

        StudioCheckBox {
            id: transparent
            text: "透明背景"
            checked: editor.selectedLayer?.transparent ?? true
        }

        RowLayout {
            StudioButton {
                text: "应用"
                onClicked: {
                    const settings = {start: Number(start.text.trim() || NaN), end: Number(end.text.trim() || NaN),
                                    transparent: transparent.checked}
                    if (editor.selectedLayer.kind === "images")
                        settings.width = Number(widthPercent.text) / 100
                    else {
                        settings.text = textValue.text
                        settings.size = Number(size.text)
                        settings.color = color.text.trim()
                    }
                    editor.controller.editSceneLayer(editor.selectedLayer.id, settings)
                }
            }
            StudioButton {
                text: "跳到图层"
                onClicked: {
                    editor.controller.pause()
                    editor.controller.seek(editor.selectedLayer.start)
                }
            }
            StudioButton { text: "移除"; onClicked: editor.controller.removeSceneLayer(editor.selectedLayer.id) }
        }
    }
}
