import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: editor
    required property var controller
    required property int videoIndex
    readonly property var modes: ["mix", "video", "cloud", "backdrop", "inset"]
    readonly property var names: ["当前合成", "实拍全屏", "点云全屏", "实拍作底片", "实拍缩成小窗"]
    readonly property var chosenCue: controller.layoutCues[cues.currentIndex] ?? null

    StudioComboBox {
        id: mode
        Layout.fillWidth: true
        model: editor.names
        enabled: !editor.controller.loading
    }
    StudioLabel {
        Layout.fillWidth: true
        visible: mode.currentIndex === 1 || mode.currentIndex >= 3
        text: editor.controller.videoLayers[editor.videoIndex]?.label ?? "先选择一路实拍视频"
        elide: Text.ElideMiddle
    }
    RowLayout {
        Layout.fillWidth: true
        visible: mode.currentIndex === 4
        StudioLabel { text: "过渡（秒）" }
        StudioTextField {
            id: transition
            Layout.fillWidth: true
            text: "0.4"
            validator: DoubleValidator { bottom: 0; top: 5; decimals: 3; locale: "C" }
        }
    }
    StudioButton {
        Layout.fillWidth: true
        text: "在播放头设置布局"
        enabled: !editor.controller.loading && editor.controller.duration > 0
                 && (mode.currentIndex === 0 || mode.currentIndex === 2 || editor.videoIndex >= 0)
                 && (mode.currentIndex !== 4 || transition.acceptableInput)
        onClicked: editor.controller.setLayoutCue(editor.modes[mode.currentIndex], editor.videoIndex, Number(transition.text))
    }

    StudioComboBox {
        id: cues
        Layout.fillWidth: true
        visible: count > 0
        model: editor.controller.layoutCues.map(cue => cue.time.toFixed(3) + " s · "
                                               + (editor.names[editor.modes.indexOf(cue.mode)] ?? cue.mode))
        onActivated: (index) => {
            editor.controller.pause()
            editor.controller.seek(editor.controller.layoutCues[index].time)
        }
    }
    StudioButton {
        Layout.fillWidth: true
        visible: editor.chosenCue !== null
        text: "移除选中布局"
        enabled: !editor.controller.loading
        onClicked: editor.controller.removeLayoutCue(editor.chosenCue.time)
    }
}
