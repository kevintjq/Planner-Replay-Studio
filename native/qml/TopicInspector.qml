import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: pane
    required property var inspector
    required property var controller
    spacing: 12

    RowLayout {
        Layout.fillWidth: true
        visible: pane.inspector.topics.length > 0
        StudioLabel {
            Layout.fillWidth: true
            text: pane.controller.position.toFixed(3) + " s"
            color: appearance.colors.secondaryText
        }
        StudioButton {
            text: "−"
            implicitWidth: appearance.metrics.controlHeight
            Accessible.name: "Previous time step · 后退 1/60 秒"
            enabled: !pane.inspector.working && !pane.controller.loading
            onClicked: { pane.controller.pause(); pane.controller.seek(pane.controller.position - 1 / 60) }
            StudioToolTip { visible: parent.hovered; text: "后退 1/60 秒（实验时间步长，非源视频帧）" }
        }
        StudioButton {
            text: "+"
            implicitWidth: appearance.metrics.controlHeight
            Accessible.name: "Next time step · 前进 1/60 秒"
            enabled: !pane.inspector.working && !pane.controller.loading
            onClicked: { pane.controller.pause(); pane.controller.seek(pane.controller.position + 1 / 60) }
            StudioToolTip { visible: parent.hovered; text: "前进 1/60 秒（实验时间步长，非源视频帧）" }
        }
    }
    StudioSlider {
        Layout.fillWidth: true
        visible: pane.inspector.topics.length > 0
        from: 0
        to: pane.controller.duration
        value: pane.controller.position
        enabled: !pane.inspector.working && !pane.controller.loading
        onPressedChanged: if (pressed) pane.controller.pause()
        onMoved: pane.controller.seek(value)
        Accessible.name: "Experiment time · 实验时间"
    }
    StudioSearchComboBox {
        id: topics
        objectName: "inspectTopicPicker"
        Layout.fillWidth: true
        sourceModel: pane.inspector.topics
        selectedText: pane.inspector.selectedTopic
        enabled: sourceModel.length > 0 && !pane.inspector.working && !pane.controller.loading
        onChosen: value => pane.inspector.inspect(value)
        Accessible.name: "Inspect Topic · 检查话题"
    }
    RowLayout {
        Layout.fillWidth: true
        StudioButton {
            objectName: "reloadTopicMessage"
            Layout.fillWidth: true
            text: "读取当前消息"
            enabled: topics.selectedText.length > 0 && !pane.inspector.working && !pane.controller.loading
            onClicked: pane.inspector.inspect(topics.selectedText, pane.inspector.selectedType)
        }
        StudioButton {
            text: "取消"
            visible: pane.inspector.working
            onClicked: pane.inspector.cancel()
        }
    }
    StudioLabel {
        Layout.fillWidth: true
        text: pane.inspector.status
        visible: text.length > 0
        wrapMode: Text.Wrap
    }
    Image {
        id: camera
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? 290 : 0
        visible: status === Image.Ready
        fillMode: Image.PreserveAspectFit
        source: pane.inspector.preview
        cache: false
    }
    RowLayout {
        Layout.fillWidth: true
        visible: pane.inspector.arrayPageCount > 1
        StudioButton {
            text: "‹"
            implicitWidth: appearance.metrics.controlHeight
            Accessible.name: "Previous array page · 上一页"
            enabled: !pane.inspector.working && pane.inspector.arrayPage > 0
            onClicked: pane.inspector.setArrayPage(pane.inspector.arrayPage - 1)
        }
        StudioLabel {
            Layout.fillWidth: true
            text: "数组 " + (pane.inspector.arrayPage + 1) + " / " + pane.inspector.arrayPageCount
            horizontalAlignment: Text.AlignHCenter
        }
        StudioButton {
            text: "›"
            implicitWidth: appearance.metrics.controlHeight
            Accessible.name: "Next array page · 下一页"
            enabled: !pane.inspector.working && pane.inspector.arrayPage + 1 < pane.inspector.arrayPageCount
            onClicked: pane.inspector.setArrayPage(pane.inspector.arrayPage + 1)
        }
    }
    StudioScrollView {
        id: fieldsScroll
        objectName: "topicFieldsScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 180
        contentWidth: availableWidth
        clip: true
        TextArea {
            palette: StudioPalette {}
            width: fieldsScroll.availableWidth
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            text: pane.inspector.fields
            font.family: "monospace"
            font.pixelSize: appearance.metrics.caption
            color: appearance.colors.text
            selectionColor: appearance.colors.selection
            selectedTextColor: appearance.colors.text
            placeholderText: pane.inspector.topics.length > 0 ? "Select a Topic to inspect its actual message.\n选择话题以读取真实消息。" : "Open a ROS Bag to inspect messages.\n打开 ROS Bag 后检查消息。"
            placeholderTextColor: appearance.colors.secondaryText
            padding: 10
            background: Rectangle { color: appearance.colors.input; radius: appearance.metrics.radius; border.color: appearance.colors.separator }
        }
    }
}
