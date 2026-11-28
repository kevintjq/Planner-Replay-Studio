import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: pane
    Layout.minimumWidth: 0
    Layout.maximumWidth: parent ? parent.width : Infinity
    required property var controller
    property real pointSize: 1.5
    property string selectedId: ""
    property var nativeClient: null
    property bool nativeEnabled: false
    property var nativeCoordinator: null
    readonly property bool nativeActive: nativeEnabled && !!nativeClient && nativeClient.connected
    readonly property var chosen: controller.cloudViews.find(view => view.id === selectedId)
                                  ?? controller.cloudViews[0] ?? null
    readonly property var choices: nativeActive ? (nativeClient.viewControllers ?? []).map(view => ({label: view.name || view.class, class: view.class, camera: 1}))
        : controller.cameraLabels.map((label, index) => ({label: label, camera: index}))
                                             .filter(item => item.camera > 0)
    readonly property var selectionChoices: {
        const layers = controller.canvas?.layers ?? []
        return controller.cloudViews.map(view => ({id: view.id,
            label: layers.find(layer => layer.id === view.id)?.name || view.label || view.id}))
    }
    readonly property string defaultTarget: "当前位姿 · Default pose"
    property string pendingTarget: chosen?.targetFrame ?? ""
    onChosenChanged: pendingTarget = chosen?.targetFrame ?? ""

    RowLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: pane.width
        StudioComboBox {
            id: available
            objectName: "viewPreset"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 0
            model: pane.choices
            textRole: "label"
        }
        StudioButton {
            objectName: "viewAdd"
            Layout.minimumWidth: implicitWidth
            text: "添加机位"
            enabled: available.currentIndex >= 0 && !pane.controller.loading
                && (!pane.nativeActive || !!pane.nativeCoordinator && !pane.nativeCoordinator.syncPending)
            onClicked: {
                if (!pane.nativeActive) { pane.controller.addCloudView(pane.choices[available.currentIndex].camera); return }
                const oldIds = pane.controller.cloudViews.map(view => view.id)
                const plugin = pane.choices[available.currentIndex]
                const canvas = pane.controller.canvas
                canvas?.beginEdit()
                try {
                    pane.controller.addCloudView(1)
                    const added = pane.controller.cloudViews.find(view => oldIds.indexOf(view.id) < 0)
                    if (added) {
                        canvas?.edit(added.id, {name: plugin.label})
                        pane.selectedId = added.id
                        pane.nativeCoordinator.setViewClass(added.id, plugin.class)
                    }
                } finally {
                    canvas?.endEdit()
                }
            }
        }
    }
    StudioComboBox {
        objectName: "viewSelection"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        visible: pane.controller.cloudViews.length > 0
        model: pane.selectionChoices
        textRole: "label"
        // Resolve the index from the displayed model as well: its canvas-name
        // refresh can reset the ComboBox after cloudViews restores an undo id.
        currentIndex: pane.selectionChoices.findIndex(view => view.id === pane.chosen?.id)
        onActivated: index => pane.selectedId = pane.selectionChoices[index].id
    }
    StudioLabel {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.Wrap
        visible: pane.chosen !== null && !pane.nativeActive
        text: "Target Frame · 跟随目标坐标系"
        color: appearance.colors.secondaryText
    }
    StudioSearchComboBox {
        objectName: "targetFramePicker"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        visible: pane.chosen !== null && !pane.nativeActive
        sourceModel: [pane.defaultTarget].concat(pane.controller.tfFrames)
        selectedText: pane.pendingTarget || pane.defaultTarget
        onChosen: value => pane.pendingTarget = value === pane.defaultTarget ? "" : value
    }
    StudioComboBox {
        id: followMode
        objectName: "viewFollowMode"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        visible: pane.chosen !== null && !pane.nativeActive
        model: ["预设跟随 · Preset", "仅位置 · Position", "位置与偏航 · Position + Yaw", "完整姿态 · Full Pose"]
        currentIndex: pane.chosen?.followMode ?? 0
    }
    GridLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        visible: pane.chosen !== null && !pane.nativeActive
        columns: pane.width < 360 ? 1 : 2
        StudioLabel { text: "机位宽度（%）" }
        StudioTextField {
            id: widthField
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen ? ((pane.chosen.width ?? .32) * 100).toFixed(0) : ""
            validator: DoubleValidator { bottom: 12; top: 150; decimals: 2; locale: "C" }
        }
        StudioLabel { text: "相机缩放" }
        StudioTextField {
            id: zoom
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen ? (pane.chosen.zoom ?? 1) : ""
            validator: DoubleValidator { bottom: .5; top: 4; decimals: 3; locale: "C" }
        }
        StudioLabel { text: "跟随滞后（s）" }
        StudioTextField {
            id: lag
            objectName: "viewLag"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen?.followLag ?? 0
            validator: DoubleValidator { bottom: 0; top: 10; decimals: 3; locale: "C" }
        }
        StudioLabel { text: "平滑窗口（s）" }
        StudioTextField {
            id: smoothing
            objectName: "viewSmoothing"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen?.smoothing ?? 0
            validator: DoubleValidator { bottom: 0; top: 10; decimals: 3; locale: "C" }
        }
        StudioLabel { text: "独立点大小（px）" }
        StudioTextField {
            id: points
            objectName: "viewPointSize"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen?.pointSize ?? pane.pointSize
            validator: DoubleValidator { bottom: .25; top: 20; decimals: 2; locale: "C" }
        }
        StudioLabel { text: "点云颜色" }
        StudioTextField {
            id: cloudColor
            objectName: "viewCloudColor"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen?.cloudColor ?? ""
            placeholderText: "#RRGGBB · 留空继承"
            validator: RegularExpressionValidator { regularExpression: /^(#[0-9a-fA-F]{6})?$/ }
        }
        StudioLabel { text: "轨迹颜色" }
        StudioTextField {
            id: pathColor
            objectName: "viewPathColor"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen?.pathColor ?? ""
            placeholderText: "#RRGGBB · 留空继承"
            validator: RegularExpressionValidator { regularExpression: /^(#[0-9a-fA-F]{6})?$/ }
        }
        StudioLabel { text: "场景背景" }
        StudioTextField {
            id: viewBackground
            objectName: "viewBackground"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen?.viewBackground ?? ""
            placeholderText: "#RRGGBB · 留空继承"
            validator: RegularExpressionValidator { regularExpression: /^(#[0-9a-fA-F]{6})?$/ }
        }
        StudioCheckBox {
            id: clear
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "透明背景"
            checked: pane.chosen?.transparent ?? false
        }
        StudioCheckBox {
            id: optical
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "Optical Frame"
            checked: pane.chosen?.opticalFrame ?? false
            StudioToolTip { visible: optical.hovered; text: "ROS 光学坐标约定：Z 向前，Y 向下。仅在目标确实使用该约定时启用。" }
        }
        StudioCheckBox {
            id: showCloud
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "显示主点云"
            checked: pane.chosen?.cloudVisible ?? true
        }
    }
    StudioLabel {
        Layout.fillWidth: true
        visible: pane.chosen !== null && !pane.nativeActive
        text: "独立显示 · 此视角可隐藏已启用的 Display"
        color: appearance.colors.secondaryText
        wrapMode: Text.Wrap
    }
    Repeater {
        id: displayChoices
        model: pane.chosen !== null && !pane.nativeActive ? pane.controller.topicDisplays : []
        delegate: StudioCheckBox {
            required property var modelData
            property string displayId: modelData.id
            objectName: "viewDisplay-" + displayId
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.maximumWidth: pane.width
            text: modelData.label || modelData.id
            checked: !(pane.chosen?.hiddenDisplays ?? []).includes(displayId)
        }
    }
    Flow {
        id: channels
        Layout.fillWidth: true
        visible: pane.chosen !== null && !pane.nativeActive
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
            delegate: StudioCheckBox {
                required property var modelData
                property string overlayKey: modelData.key
                text: modelData.label
                checked: (pane.chosen?.overlays ?? []).includes(overlayKey)
            }
        }
    }
    GridLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        columns: pane.width < 360 ? 1 : 2
        visible: pane.chosen !== null && !pane.nativeActive
        StudioButton {
            objectName: "viewApply"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "应用机位设置"
            enabled: !pane.controller.loading && widthField.acceptableInput && zoom.acceptableInput
                     && lag.acceptableInput && smoothing.acceptableInput && points.acceptableInput
                     && cloudColor.acceptableInput && pathColor.acceptableInput && viewBackground.acceptableInput
            onClicked: {
                const selected = []
                for (let i = 0; i < overlayChoices.count; i++) {
                    const choice = overlayChoices.itemAt(i)
                    if (choice.checked)
                        selected.push(choice.overlayKey)
                }
                const hidden = []
                for (let i = 0; i < displayChoices.count; i++) {
                    const choice = displayChoices.itemAt(i)
                    if (!choice.checked)
                        hidden.push(choice.displayId)
                }
                pane.controller.setCloudView(pane.chosen.id, {
                    width: Number(widthField.text) / 100, zoom: Number(zoom.text),
                    transparent: clear.checked, overlays: selected,
                    targetFrame: pane.pendingTarget, followMode: followMode.currentIndex,
                    followLag: Number(lag.text), smoothing: Number(smoothing.text), opticalFrame: optical.checked,
                    cloudVisible: showCloud.checked, cloudColor: cloudColor.text,
                    pathColor: pathColor.text, viewBackground: viewBackground.text,
                    pointSize: Number(points.text), hiddenDisplays: hidden
                })
            }
        }
        StudioButton {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "移除机位"
            enabled: !pane.controller.loading
            onClicked: pane.controller.removeCloudView(pane.chosen.id)
        }
    }
    StudioLabel {
        objectName: "nativeViewCapabilities"
        Layout.fillWidth: true
        visible: pane.nativeActive
        wrapMode: Text.Wrap
        text: "Native cameras share one real scene and experiment clock. Camera properties and Target Frame below come from the installed RViz plugin.\n原生视角共享真实场景与实验时间；以下相机及 Target Frame 属性来自已安装的 RViz 插件。\nIndependent Display visibility, point size/colors, transparent backgrounds and compatible camera smoothing are not supported by this native backend yet.\n此原生后端暂不支持按视角独立显隐、点大小/颜色、透明背景及兼容相机的平滑设置。"
        color: appearance.colors.secondaryText
        font.pixelSize: appearance.metrics.caption
    }
    StudioLabel {
        Layout.fillWidth: true
        visible: pane.nativeActive && !!pane.nativeCoordinator?.syncError.length
        text: pane.nativeCoordinator?.syncError ?? ""
        wrapMode: Text.Wrap
    }
    StudioSearchComboBox {
        objectName: "nativeInsetClass"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        visible: pane.nativeActive && pane.chosen !== null
        enabled: pane.nativeActive && !pane.nativeClient.busy && !!pane.nativeCoordinator && !pane.nativeCoordinator.syncPending
        sourceModel: pane.nativeActive ? pane.nativeClient.viewControllers.map(view => view.class) : []
        selectedText: pane.chosen && pane.nativeCoordinator ? pane.nativeCoordinator.classForId(pane.chosen.id) : ""
        onChosen: value => pane.nativeCoordinator.setViewClass(pane.chosen.id, value)
    }
    GridLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        columns: pane.width < 360 ? 1 : 2
        visible: pane.nativeActive && pane.chosen !== null
        StudioLabel { text: "Width · 宽度 (%)" }
        StudioTextField {
            objectName: "nativeInsetWidth"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.chosen ? ((pane.chosen.width ?? .32) * 100).toFixed(0) : ""
            validator: DoubleValidator { bottom: 12; top: 150; decimals: 2; locale: "C" }
            onEditingFinished: if (acceptableInput) pane.controller.setCloudView(pane.chosen.id, {width: Number(text) / 100})
        }
    }
    GridLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        columns: pane.width < 440 ? 1 : 2
        visible: pane.nativeActive && pane.chosen !== null
        StudioButton {
            objectName: "nativeInsetReset"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "Reset Camera · 重置相机"
            enabled: pane.nativeActive && !pane.nativeClient.busy
            onClicked: pane.nativeClient.resetCameraForView(pane.chosen.id)
        }
        StudioButton {
            objectName: "nativeInsetRemove"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "Remove · 移除机位"
            enabled: !pane.controller.loading
            onClicked: pane.controller.removeCloudView(pane.chosen.id)
        }
    }
    Loader {
        objectName: "nativeInsetProperties"
        Layout.fillWidth: true
        Layout.preferredHeight: active && item ? item.implicitHeight : 0
        active: pane.nativeActive && pane.chosen !== null
        visible: active
        sourceComponent: NativeRvizPanel {
            anchors.fill: parent
            embedded: true
            client: pane.nativeClient
            treeScope: "views"
            viewIdFilter: pane.chosen?.id ?? ""
            showConnection: false
            showPreview: false
            showPreviewImage: false
            previewExternallyManaged: true
        }
    }
}
