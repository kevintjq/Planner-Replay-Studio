import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: pane
    required property var inspector
    required property var controller
    spacing: 12

    Label {
        text: "播放位置 " + pane.controller.position.toFixed(3) + " s"
        visible: pane.inspector.topics.length > 0
    }

    RowLayout {
        Layout.fillWidth: true
        visible: pane.inspector.topics.length > 0
        Button {
            text: "← 1 帧"
            enabled: !pane.inspector.working && !pane.controller.loading
            onClicked: pane.controller.seek(pane.controller.position - 1 / 60)
        }
        Slider {
            Layout.fillWidth: true
            from: 0
            to: pane.controller.duration
            value: pane.controller.position
            enabled: !pane.inspector.working && !pane.controller.loading
            onMoved: pane.controller.seek(value)
        }
        Button {
            text: "1 帧 →"
            enabled: !pane.inspector.working && !pane.controller.loading
            onClicked: pane.controller.seek(pane.controller.position + 1 / 60)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        ComboBox {
            id: topics
            Layout.fillWidth: true
            model: pane.inspector.topics
            enabled: count > 0 && !pane.inspector.working && !pane.controller.loading
            displayText: count > 0 ? currentText : "打开 ROS1 bag 后选择话题"
        }
        Button {
            text: "读取当前消息"
            enabled: topics.currentIndex >= 0 && !pane.inspector.working && !pane.controller.loading
            onClicked: pane.inspector.inspect(topics.currentText)
        }
        Button {
            text: "取消"
            visible: pane.inspector.working
            onClicked: pane.inspector.cancel()
        }
    }
    Label {
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
        Button {
            text: "上一页"
            enabled: !pane.inspector.working && pane.inspector.arrayPage > 0
            onClicked: pane.inspector.setArrayPage(pane.inspector.arrayPage - 1)
        }
        Label {
            Layout.fillWidth: true
            text: "数组 " + (pane.inspector.arrayPage + 1) + " / " + pane.inspector.arrayPageCount
        }
        Button {
            text: "下一页"
            enabled: !pane.inspector.working && pane.inspector.arrayPage + 1 < pane.inspector.arrayPageCount
            onClicked: pane.inspector.setArrayPage(pane.inspector.arrayPage + 1)
        }
    }
    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        TextArea {
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            text: pane.inspector.fields
            font.family: "monospace"
        }
    }
}
