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

    property int selectedVideo: 0

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
    FileDialog {
        id: rvizDialog
        title: "导入 RViz 显示配置"
        nameFilters: ["RViz 配置 (*.rviz *.yaml *.yml)"]
        onAccepted: replay.openRviz(selectedFile)
    }
    FileDialog {
        id: projectDialog
        title: "打开 Planner Replay 项目"
        nameFilters: ["Planner Replay 项目 (*.slamfpv)"]
        onAccepted: replay.openProject(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: "保存 Planner Replay 项目"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "slamfpv"
        nameFilters: ["Planner Replay 项目 (*.slamfpv)"]
        onAccepted: replay.saveProjectAs(selectedFile)
    }
    FileDialog {
        id: videoDialog
        title: "添加实拍视频"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["视频 (*.mp4 *.mov *.mkv *.avi *.webm)", "所有文件 (*)"]
        onAccepted: {
            for (const file of selectedFiles)
                replay.addVideo(file)
        }
    }
    FolderDialog {
        id: packageDialog
        title: "选择打包项目的上级文件夹"
        onAccepted: replay.exportProjectFolder(selectedFolder)
    }
    Shortcut { sequence: StandardKey.Open; enabled: !replay.loading; onActivated: projectDialog.open() }
    Shortcut { sequence: StandardKey.SaveAs; enabled: !replay.loading; onActivated: saveDialog.open() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: !replay.loading && replay.canUndo; onActivated: replay.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: !replay.loading && replay.canRedo; onActivated: replay.redo() }
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

                    PanelTitle { text: "项目" }
                    RowLayout {
                        Layout.fillWidth: true
                        Button { text: "打开项目"; enabled: !replay.loading; onClicked: projectDialog.open() }
                        Button { text: "另存为"; enabled: !replay.loading; onClicked: saveDialog.open() }
                        Button { text: "打包"; enabled: !replay.loading; onClicked: packageDialog.open() }
                    }
                    Caption {
                        Layout.fillWidth: true
                        visible: replay.projectPath.length > 0
                        text: replay.projectPath
                        elide: Text.ElideMiddle
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

                    Button {
                        text: "导入 RViz 配置"
                        Layout.fillWidth: true
                        enabled: !replay.loading
                        onClicked: rvizDialog.open()
                    }
                    Button {
                        id: topicDisplayToggle
                        Layout.fillWidth: true
                        visible: replay.topicDisplays.length > 0
                        text: (checked ? "收起" : "展开") + " RViz 话题显示（" + replay.topicDisplays.length + "）"
                        checkable: true
                    }
                    Repeater {
                        model: topicDisplayToggle.checked ? replay.topicDisplays : []
                        delegate: CheckBox {
                            id: topicDisplay
                            required property int index
                            required property var modelData
                            Layout.fillWidth: true
                            checked: modelData.enabled
                            text: modelData.label
                            enabled: !replay.loading
                            onClicked: replay.setTopicDisplayEnabled(index, checked)
                            ToolTip.visible: hovered
                            ToolTip.text: text
                            contentItem: Label {
                                text: topicDisplay.text
                                color: topicDisplay.modelData.color
                                leftPadding: topicDisplay.indicator.width + topicDisplay.spacing
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideMiddle
                            }
                        }
                    }

                    PanelTitle { text: "实拍视频"; Layout.topMargin: 8 }
                    Button {
                        Layout.fillWidth: true
                        text: "添加视频"
                        enabled: !replay.loading
                        onClicked: videoDialog.open()
                    }
                    ComboBox {
                        id: videoPicker
                        Layout.fillWidth: true
                        model: replay.videoLayers
                        textRole: "label"
                        currentIndex: count > 0 ? Math.min(window.selectedVideo, count - 1) : -1
                        onActivated: (index) => window.selectedVideo = index
                        visible: count > 0
                    }
                    ColumnLayout {
                        id: videoSettings
                        readonly property var selectedClip: replay.videoLayers[videoPicker.currentIndex] || null
                        Layout.fillWidth: true
                        visible: selectedClip !== null
                        enabled: !replay.loading
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            Caption { text: "开始（秒）" }
                            TextField { id: trimIn; Layout.fillWidth: true; text: videoSettings.selectedClip?.trimIn ?? "" }
                            Caption { text: "结束（秒）" }
                            TextField { id: trimOut; Layout.fillWidth: true; text: videoSettings.selectedClip?.trimOut ?? ""; placeholderText: "视频末尾" }
                            Caption { text: "偏移（秒）" }
                            TextField { id: offset; Layout.fillWidth: true; text: videoSettings.selectedClip?.offset ?? "" }
                        }
                        Button {
                            Layout.fillWidth: true
                            text: "应用裁剪与偏移"
                            onClicked: replay.setVideoTrim(videoPicker.currentIndex, trimIn.text, trimOut.text, offset.text)
                        }
                        Caption {
                            text: "暂停在回放事件处，输入实拍中同一事件的时间"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }
                        TextField { id: mediaTime; Layout.fillWidth: true; placeholderText: "视频时间（秒）" }
                        RowLayout {
                            Button { text: "锚点 1"; enabled: !replay.playing; onClicked: replay.setVideoAnchor(videoPicker.currentIndex, 0, mediaTime.text) }
                            Button { text: "锚点 2"; enabled: !replay.playing && (videoSettings.selectedClip?.anchors.length ?? 0) > 0; onClicked: replay.setVideoAnchor(videoPicker.currentIndex, 1, mediaTime.text) }
                            Button { text: "清除"; enabled: (videoSettings.selectedClip?.anchors.length ?? 0) > 0; onClicked: replay.clearVideoAnchors(videoPicker.currentIndex) }
                        }
                    }

                    PanelTitle { text: "图片与文字"; Layout.topMargin: 8 }
                    SceneLayerEditor {
                        id: sceneLayerEditor
                        Layout.fillWidth: true
                        controller: replay
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
                Caption { text: replay.demo ? "模拟数据" : replay.cloudTopic.length ? "ROS1 录制" : "无点云数据"; color: window.accent }
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

                Repeater {
                    model: replay.videoLayers.length
                    delegate: FlightClip {
                        required property int index
                        controller: replay
                        clipIndex: index
                        footage: replay.videoLayers[index]
                    }
                }

                SceneOverlays {
                    anchors.fill: parent
                    controller: replay
                    onSelected: (id) => sceneLayerEditor.selectedId = id
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
                Layout.preferredHeight: timelineContents.implicitHeight + 24
                radius: 8
                border.color: window.thin
                color: window.panel
                ColumnLayout {
                    id: timelineContents
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 5

                    MarkerTimeline {
                        Layout.fillWidth: true
                        controller: replay
                        labelWidth: 124
                        accent: window.accent
                    }

                    VideoTimeline {
                        Layout.fillWidth: true
                        visible: replay.videoLayers.length + replay.sceneLayers.length > 0
                        controller: replay
                        selectedIndex: videoPicker.currentIndex
                        selectedLayer: sceneLayerEditor.selectedId
                        ink: window.ink
                        accent: window.accent
                        onSelected: (index) => {
                            window.selectedVideo = index
                            sceneLayerEditor.selectedId = ""
                        }
                        onLayerSelected: (id) => sceneLayerEditor.selectedId = id
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Button {
                            text: replay.playing ? "暂停" : "播放"
                            highlighted: true
                            enabled: !replay.loading && replay.duration > 0
                            onClicked: replay.togglePlayback()
                        }
                        Button {
                            text: "分割"
                            visible: replay.videoLayers.length > 0
                            enabled: !replay.loading && videoPicker.currentIndex >= 0
                            onClicked: replay.splitVideo(videoPicker.currentIndex, replay.position)
                        }
                        Button {
                            text: "删除片段"
                            visible: replay.videoLayers.length > 0
                            enabled: !replay.loading && videoPicker.currentIndex >= 0
                            onClicked: replay.deleteVideoSegment(videoPicker.currentIndex, replay.position)
                        }
                        Button { text: "撤销视频"; visible: replay.videoLayers.length > 0; enabled: !replay.loading && replay.canUndo; onClicked: replay.undo() }
                        Button { text: "重做视频"; visible: replay.videoLayers.length > 0; enabled: !replay.loading && replay.canRedo; onClicked: replay.redo() }
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
}
