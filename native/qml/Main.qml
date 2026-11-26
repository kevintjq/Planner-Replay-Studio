import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import PlannerReplay

ApplicationWindow {
    id: window
    visible: !smokeTest
    width: 1370
    height: 840
    minimumWidth: 980
    minimumHeight: 640
    title: "Planner Replay Studio"
    color: "#0d1420"

    readonly property color panel: "#151f30"
    readonly property color thin: "#2c3b51"
    readonly property color ink: "#e8f1fc"
    readonly property color muted: "#9aafc8"
    readonly property color accent: "#82d9fd"
    palette.windowText: ink
    palette.text: ink
    palette.buttonText: ink
    palette.button: "#253850"
    palette.base: panel
    palette.highlight: "#3075b0"
    palette.highlightedText: "white"

    function clock(seconds) {
        const mins = Math.floor(seconds / 60)
        return mins.toString().padStart(2, "0") + ":" + (seconds % 60).toFixed(2).padStart(5, "0")
    }

    FileDialog {
        id: bagDialog
        title: "打开 ROS1 bag"
        nameFilters: ["ROS1 bag (*.bag)"]
        onAccepted: replay.openBag(selectedFile)
    }
    Shortcut { sequence: StandardKey.Open; enabled: !replay.loading; onActivated: bagDialog.open() }
    Shortcut { sequence: "Space"; onActivated: replay.togglePlayback() }
    Shortcut { sequence: "Left"; onActivated: { replay.pause(); replay.seek(replay.position - 1 / 60) } }
    Shortcut { sequence: "Right"; onActivated: { replay.pause(); replay.seek(replay.position + 1 / 60) } }

    component PanelTitle: Label {
        color: window.ink
        font.pixelSize: 15
        font.bold: true
    }
    component Caption: Label {
        color: window.muted
        font.pixelSize: 12
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 14

        Rectangle {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            color: window.panel
            border.color: window.thin
            radius: 10

            ScrollView {
                anchors.fill: parent
                anchors.margins: 15
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: parent.width
                    spacing: 9

                    Label {
                        text: "Planner Replay"
                        font.pixelSize: 25
                        font.bold: true
                        color: window.ink
                        Layout.fillWidth: true
                    }
                    Caption { text: "原生预览  /  ROS1 BAG"; Layout.bottomMargin: 10 }
                    Button {
                        text: "打开 ROS1 bag"
                        Layout.fillWidth: true
                        enabled: !replay.loading
                        highlighted: true
                        onClicked: bagDialog.open()
                    }
                    Button {
                        text: "使用模拟数据"
                        Layout.fillWidth: true
                        enabled: !replay.loading
                        onClicked: replay.openDemo()
                    }
                    Caption {
                        Layout.fillWidth: true
                        text: replay.name
                        elide: Text.ElideMiddle
                        Layout.bottomMargin: 8
                    }

                    PanelTitle { text: "数据来源" }
                    Caption { text: "点云话题" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: replay.cloudTopics
                        currentIndex: Math.max(0, model.indexOf(replay.cloudTopic))
                        enabled: !replay.loading && !replay.demo
                        onActivated: replay.setCloudTopic(currentText)
                    }
                    Caption { text: "里程计 / 速度话题" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: replay.odometryTopics
                        currentIndex: model.indexOf(replay.odometryTopic)
                        enabled: !replay.loading && !replay.demo && count > 0
                        onActivated: replay.setOdometryTopic(currentText)
                    }
                    CheckBox {
                        text: "整图替换"
                        checked: replay.replacing
                        enabled: !replay.loading && !replay.demo
                        onClicked: replay.setReplacing(checked)
                    }

                    PanelTitle { text: "视口"; Layout.topMargin: 8 }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["地图总览", "跟随机体"]
                        onActivated: cloud.camera = currentIndex
                    }
                    CheckBox {
                        text: "显示已飞轨迹"
                        checked: true
                        onToggled: cloud.showPath = checked
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Caption { text: "点大小" }
                        Slider {
                            Layout.fillWidth: true
                            from: 1; to: 6; value: 1.5; stepSize: .25
                            onValueChanged: cloud.pointSize = value
                        }
                        Caption { text: cloud.pointSize.toFixed(1) + " px" }
                    }
                    Button {
                        text: "重置视角"
                        Layout.fillWidth: true
                        onClicked: cloud.resetCamera()
                    }
                    Caption {
                        text: replay.status
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        visible: text.length > 0
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: replay.loading
                        value: replay.progress
                    }
                    Button {
                        text: "取消读取"
                        Layout.fillWidth: true
                        visible: replay.loading
                        onClicked: replay.cancel()
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                PanelTitle { text: "点云回放" }
                Caption { text: "左键旋转 · 右键平移 · 滚轮缩放"; Layout.fillWidth: true }
                Caption { text: replay.demo ? "模拟数据" : "ROS1 录制"; color: window.accent }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#08101a"
                border.color: window.thin
                radius: 8
                clip: true

                CloudView {
                    id: cloud
                    anchors.fill: parent
                    anchors.margins: 1
                    controller: replay
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        property real lastX: 0
                        property real lastY: 0
                        onPressed: function(mouse) { lastX = mouse.x; lastY = mouse.y }
                        onPositionChanged: function(mouse) {
                            if (!pressed) return
                            if (pressedButtons & Qt.RightButton)
                                cloud.pan(mouse.x - lastX, mouse.y - lastY)
                            else
                                cloud.orbit(mouse.x - lastX, mouse.y - lastY)
                            lastX = mouse.x; lastY = mouse.y
                        }
                        onWheel: function(wheel) {
                            cloud.zoom(wheel.angleDelta.y / 120)
                            wheel.accepted = true
                        }
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 16
                    width: readings.implicitWidth + 25
                    height: readings.implicitHeight + 20
                    color: "#172235"
                    border.color: window.thin
                    opacity: .94
                    radius: 6
                    Column {
                        id: readings
                        anchors.centerIn: parent
                        spacing: 4
                        Label {
                            text: replay.speed + " m/s"
                            color: window.ink
                            font.pixelSize: 24
                            font.bold: true
                        }
                        Caption { text: Number(replay.pointCount).toLocaleString(Qt.locale(), "f", 0) + " 点" }
                    }
                }
                Caption {
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                    anchors.margins: 15
                    text: "机体距离渐变"
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                radius: 8
                border.color: window.thin
                color: window.panel
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 15
                    Button {
                        text: replay.playing ? "暂停" : "播放"
                        highlighted: true
                        enabled: !replay.loading && replay.duration > 0
                        onClicked: replay.togglePlayback()
                    }
                    ColumnLayout {
                        spacing: 3
                        Layout.fillWidth: true
                        Slider {
                            id: timeline
                            Layout.fillWidth: true
                            from: 0
                            to: replay.duration
                            live: true
                            enabled: !replay.loading && replay.duration > 0
                            value: replay.position
                            onPressedChanged: if (pressed) replay.pause()
                            onMoved: replay.seek(value)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Caption { text: window.clock(replay.position) }
                            Item { Layout.fillWidth: true }
                            Caption { text: window.clock(replay.duration) }
                        }
                    }
                }
            }
        }
    }
}
