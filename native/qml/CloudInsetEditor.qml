import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: pane
    required property var controller
    property string selectedId: ""
    readonly property var chosen: controller.cloudViews.find(view => view.id === selectedId)
                                  ?? controller.cloudViews[0] ?? null
    readonly property var choices: controller.cameraLabels.map((label, index) => ({label: label, camera: index}))
                                             .filter(item => item.camera > 0 &&
                                                     !controller.cloudViews.some(view => view.camera === item.camera))

    RowLayout {
        Layout.fillWidth: true
        ComboBox {
            id: available
            Layout.fillWidth: true
            model: pane.choices
            textRole: "label"
        }
        Button {
            text: "添加机位"
            enabled: available.currentIndex >= 0 && !pane.controller.loading
            onClicked: pane.controller.addCloudView(pane.choices[available.currentIndex].camera)
        }
    }
    ComboBox {
        Layout.fillWidth: true
        visible: pane.controller.cloudViews.length > 0
        model: pane.controller.cloudViews
        textRole: "label"
        currentIndex: Math.max(0, pane.controller.cloudViews.findIndex(view => view.id === pane.chosen?.id))
        onActivated: index => pane.selectedId = pane.controller.cloudViews[index].id
    }
    GridLayout {
        Layout.fillWidth: true
        visible: pane.chosen !== null
        columns: 2
        Label { text: "机位宽度（%）" }
        TextField {
            id: widthField
            Layout.fillWidth: true
            text: pane.chosen ? (pane.chosen.width * 100).toFixed(0) : ""
        }
        Label { text: "相机缩放" }
        TextField {
            id: zoom
            Layout.fillWidth: true
            text: pane.chosen ? pane.chosen.zoom : ""
        }
        CheckBox {
            id: clear
            text: "透明背景"
            checked: pane.chosen?.transparent ?? false
        }
    }
    Flow {
        id: channels
        Layout.fillWidth: true
        visible: pane.chosen !== null
        spacing: 1
        Repeater {
            id: overlayChoices
            model: [
                {key: "actual", label: "实飞"},
                {key: "plan", label: "规划"},
                {key: "goal", label: "目标"},
                {key: "corridor", label: "走廊"},
                {key: "scan", label: "雷达"},
                {key: "camera", label: "视场"}
            ]
            delegate: CheckBox {
                required property var modelData
                property string overlayKey: modelData.key
                text: modelData.label
                checked: (pane.chosen?.overlays ?? []).includes(overlayKey)
            }
        }
    }
    RowLayout {
        visible: pane.chosen !== null
        Button {
            text: "应用机位设置"
            enabled: !pane.controller.loading
            onClicked: {
                const selected = []
                for (let i = 0; i < overlayChoices.count; i++) {
                    const choice = overlayChoices.itemAt(i)
                    if (choice.checked)
                        selected.push(choice.overlayKey)
                }
                pane.controller.setCloudView(pane.chosen.id, {
                    width: Number(widthField.text) / 100, zoom: Number(zoom.text),
                    transparent: clear.checked, overlays: selected
                })
            }
        }
        Button {
            text: "移除机位"
            enabled: !pane.controller.loading
            onClicked: pane.controller.removeCloudView(pane.chosen.id)
        }
    }
}
