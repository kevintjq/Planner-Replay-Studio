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

    header: TabBar {
        id: workspace
        enabled: !movie.exporting
        onCurrentIndexChanged: if (currentIndex !== 0) replay.pause()
        TabButton { text: "回放与合成" }
        TabButton { text: "固定机位拖影" }
        TabButton { text: "话题查看" }
    }

    readonly property var topicInspector: inspector
    property int selectedVideo: 0
    readonly property var activeLayout: replay.activeLayout
    readonly property bool videoOnly: activeLayout.mode === "video" && (movie.exporting
        ? movie.frames.some((frame, index) => frame.width > 0 && replay.videoLayers[index]?.id === activeLayout.video_id)
        : activeLayout.fullHasMedia)
    readonly property bool cloudOnly: activeLayout.mode === "cloud"

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
        id: pngDialog
        title: "导出当前画面"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["PNG 图像 (*.png)"]
        onAccepted: replay.exportFrame(selectedFile, canvas)
    }
    FileDialog {
        id: mp4Dialog
        title: "导出 MP4 视频"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: ["MP4 视频 (*.mp4)"]
        onAccepted: movie.start(selectedFile, canvas, Number(movieBegin.text),
                                movieEnd.text.trim() === "" ? replay.duration : Number(movieEnd.text),
                                [1., .5, .25][movieSpeed.currentIndex], movieAudio.currentIndex - 1,
                                [1280, 1920, 3840][movieSize.currentIndex], [720, 1080, 2160][movieSize.currentIndex],
                                movieFps.value, movieQuality.value)
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
    Shortcut { sequence: StandardKey.Open; enabled: !movie.exporting && !replay.loading; onActivated: projectDialog.open() }
    Shortcut { sequence: StandardKey.SaveAs; enabled: !movie.exporting && !replay.loading; onActivated: saveDialog.open() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: !movie.exporting && !replay.loading && replay.canUndo; onActivated: replay.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: !movie.exporting && !replay.loading && replay.canRedo; onActivated: replay.redo() }
    Shortcut { sequence: "Space"; enabled: !movie.exporting && workspace.currentIndex === 0; onActivated: replay.togglePlayback() }
    Shortcut { sequence: "Left"; enabled: !movie.exporting && workspace.currentIndex === 0; onActivated: { replay.pause(); replay.seek(replay.position - 1 / 60) } }
    Shortcut { sequence: "Right"; enabled: !movie.exporting && workspace.currentIndex === 0; onActivated: { replay.pause(); replay.seek(replay.position + 1 / 60) } }

    component PanelTitle: Label {
        color: window.ink
        font.pixelSize: 15
        font.bold: true
    }
    component Caption: Label {
        color: window.muted
        font.pixelSize: 12
    }

    TopicInspector {
        anchors.fill: parent
        anchors.margins: 18
        visible: workspace.currentIndex === 2
        inspector: window.topicInspector
        controller: replay
    }

    TrajectoryPane {
        anchors.fill: parent
        anchors.margins: 18
        visible: workspace.currentIndex === 1
        editor: trajectory
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 18
        visible: workspace.currentIndex === 0
        enabled: !movie.exporting
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
                        Button { text: "打开项目"; enabled: !movie.exporting && !replay.loading; onClicked: projectDialog.open() }
                        Button { text: "另存为"; enabled: !movie.exporting && !replay.loading; onClicked: saveDialog.open() }
                        Button { text: "打包"; enabled: !movie.exporting && !replay.loading; onClicked: packageDialog.open() }
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
                    Caption { text: "位姿来源" }
                    ComboBox {
                        id: poseSource
                        Layout.fillWidth: true
                        model: ["Odometry", "TF"]
                        currentIndex: replay.tfPose ? 1 : 0
                        enabled: !replay.loading && !replay.demo
                        onActivated: index => {
                            if (index === 0)
                                replay.setPoseSource(false, "", "")
                        }
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        visible: poseSource.currentIndex === 1
                        columns: 2
                        Label { text: "世界坐标系" }
                        TextField {
                            id: poseWorld
                            Layout.fillWidth: true
                            text: replay.worldFrame
                            placeholderText: "自动"
                        }
                        Label { text: "机体坐标系" }
                        TextField {
                            id: poseBody
                            Layout.fillWidth: true
                            text: replay.bodyFrame
                            placeholderText: "自动"
                        }
                        Button {
                            Layout.columnSpan: 2
                            text: "应用 TF 位姿"
                            enabled: !replay.loading && !replay.demo
                            onClicked: replay.setPoseSource(true, poseWorld.text, poseBody.text)
                        }
                    }
                    Caption { text: "速度话题" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["自动识别"].concat(replay.speedTopics)
                        currentIndex: Math.max(0, model.indexOf(replay.speedSelection))
                        enabled: !replay.loading && !replay.demo
                        onActivated: index => replay.setSpeedTopic(index > 0 ? currentText : "")
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

                    Repeater {
                        model: [
                            {key: "plan", label: "规划轨迹"},
                            {key: "corridor", label: "安全走廊"},
                            {key: "goal", label: "目标点"},
                            {key: "camera", label: "相机视场"},
                            {key: "scan", label: "当前扫描"}
                        ]
                        delegate: CheckBox {
                            required property var modelData
                            readonly property var plannerLayer: replay.planningLayers[modelData.key]
                            visible: plannerLayer !== undefined
                            checked: plannerLayer?.enabled ?? false
                            text: modelData.label
                            onClicked: replay.setPlanningLayerEnabled(modelData.key, checked)
                            ToolTip.visible: hovered
                            ToolTip.text: plannerLayer?.topic ?? ""
                        }
                    }
                    Button {
                        id: scanExpand
                        visible: replay.scanTopics.length > 0
                        text: checked ? "收起当前扫描设置" : "当前扫描设置"
                        checkable: true
                    }
                    ColumnLayout {
                        visible: scanExpand.visible && scanExpand.checked
                        Layout.fillWidth: true
                        ComboBox {
                            id: scanChoice
                            Layout.fillWidth: true
                            model: ["关闭", ...replay.scanTopics]
                            currentIndex: Math.max(0, replay.scanTopics.indexOf(replay.scanTopic) + 1)
                        }
                        RowLayout {
                            Label { text: "最大保留秒数" }
                            TextField {
                                id: scanAge
                                Layout.fillWidth: true
                                text: Number(replay.scanMaximumAge).toLocaleString(Qt.locale(), "f", 2)
                                validator: DoubleValidator { bottom: .001; top: 60 }
                            }
                        }
                        Button {
                            text: "应用当前扫描"
                            enabled: !replay.loading && scanAge.acceptableInput
                            onClicked: replay.setScanOptions(scanChoice.currentIndex === 0 ? "" : scanChoice.currentText,
                                                            Number.fromLocaleString(scanAge.text))
                        }
                    }
                    CheckBox {
                        visible: replay.planningLayers.plan !== undefined
                        checked: replay.comparePlans
                        text: "对比上一条规划"
                        onClicked: replay.setComparePlans(checked)
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        visible: count > 0
                        model: replay.planningEvents
                        textRole: "label"
                        displayText: currentIndex < 0 ? "跳转到规划事件" : currentText
                        onActivated: index => {
                            replay.pause()
                            replay.seek(replay.planningEvents[index].time)
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

                    Button {
                        Layout.fillWidth: true
                        text: "查找对齐候选"
                        enabled: !replay.loading && replay.videoLayers.length > 0
                        onClicked: {
                            alignment.model = replay.alignmentCandidates()
                            alignment.currentIndex = -1
                        }
                    }
                    ComboBox {
                        id: alignment
                        Layout.fillWidth: true
                        visible: replay.videoLayers.length > 0
                        displayText: currentIndex < 0 ? (count ? "跳转到对齐候选" : "暂无对齐候选") : currentText
                        textRole: "label"
                        onActivated: replay.seek(model[currentIndex].time)
                        Connections {
                            target: replay
                            function onRecordingChanged() { alignment.model = [] }
                        }
                    }
                    PanelTitle { text: "布局切换"; Layout.topMargin: 8 }
                    LayoutEditor {
                        Layout.fillWidth: true
                        controller: replay
                        videoIndex: videoPicker.currentIndex
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
                        model: replay.cameraLabels
                        currentIndex: replay.cameraIndex
                        enabled: !replay.loading
                        onActivated: replay.setCamera(currentIndex)
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
                    CheckBox {
                        text: "透明画布"
                        checked: replay.transparentCanvas
                        enabled: !replay.loading && !replay.capturing
                        onClicked: replay.setTransparentCanvas(checked)
                    }
                    Button {
                        text: "导出当前帧 PNG"
                        enabled: !replay.loading && !replay.capturing
                        Layout.fillWidth: true
                        onClicked: {
                            replay.pause()
                            pngDialog.open()
                        }
                    }
                    PanelTitle { text: "附加点云机位" }
                    CloudInsetEditor {
                        id: insetEditor
                        Layout.fillWidth: true
                        controller: replay
                    }
                    LayerMotionEditor {
                        Layout.fillWidth: true
                        controller: replay
                    }
                    CheckBox {
                        text: "显示合成速度表"
                        checked: replay.speedGauge.visible
                        onClicked: replay.setSpeedGauge({visible: checked})
                    }
                    GridLayout {
                        visible: replay.speedGauge.visible
                        columns: 2
                        Layout.fillWidth: true
                        Label { text: "量程（m/s）" }
                        TextField { id: gaugeLimit; Layout.fillWidth: true; text: replay.speedGauge.limit }
                        Label { text: "缩放" }
                        TextField { id: gaugeScale; Layout.fillWidth: true; text: replay.speedGauge.scale }
                        Label { text: "开始（秒）" }
                        TextField { id: gaugeStart; Layout.fillWidth: true; text: replay.speedGauge.start }
                        Label { text: "结束（秒）" }
                        TextField {
                            id: gaugeEnd
                            Layout.fillWidth: true
                            text: replay.speedGauge.end < 0 ? "" : replay.speedGauge.end
                        }
                        CheckBox {
                            id: gaugeClear
                            text: "透明"
                            checked: replay.speedGauge.transparent
                        }
                        Button {
                            text: "应用速度表"
                            onClicked: replay.setSpeedGauge({limit: Number(gaugeLimit.text), scale: Number(gaugeScale.text),
                                start: Number(gaugeStart.text), end: gaugeEnd.text.trim() === "" ? -1 : Number(gaugeEnd.text),
                                transparent: gaugeClear.checked})
                        }
                    }
                    PanelTitle { text: "导出视频" }
                    GridLayout {
                        columns: 2
                        Layout.fillWidth: true
                        Label { text: "开始（秒）" }
                        TextField {
                            id: movieBegin
                            text: "0"
                            Layout.fillWidth: true
                            validator: DoubleValidator { bottom: 0; locale: "C" }
                        }
                        Label { text: "结束（秒）" }
                        TextField {
                            id: movieEnd
                            placeholderText: "回放末尾"
                            Layout.fillWidth: true
                            validator: DoubleValidator { bottom: 0; locale: "C" }
                        }
                        Label { text: "分辨率" }
                        ComboBox { id: movieSize; model: ["720p", "1080p", "2160p"]; currentIndex: 1 }
                        Label { text: "帧率" }
                        SpinBox { id: movieFps; from: 1; to: 120; value: 60; editable: true }
                        Label { text: "CRF" }
                        SpinBox { id: movieQuality; from: 0; to: 51; value: 18; editable: true }
                        Label { text: "速度" }
                        ComboBox {
                            id: movieSpeed
                            Layout.fillWidth: true
                            model: ["1×", "0.5×", "0.25×"]
                            currentIndex: Math.max(0, [1., .5, .25].indexOf(replay.moviePreferences.speed))
                            onActivated: replay.setMoviePreferences([1., .5, .25][currentIndex], movieAudio.currentIndex - 1)
                        }
                        Label { text: "音轨" }
                        ComboBox {
                            id: movieAudio
                            Layout.fillWidth: true
                            model: ["无音轨"].concat(replay.videoLayers.map(layer => layer.label))
                            currentIndex: Math.max(0, Math.min(count - 1, replay.moviePreferences.audioIndex + 1))
                            onActivated: replay.setMoviePreferences([1., .5, .25][movieSpeed.currentIndex], currentIndex - 1)
                        }
                    }
                    Button {
                        text: "导出 MP4"
                        Layout.fillWidth: true
                        enabled: !replay.loading && !replay.capturing && replay.duration > 0 && movieBegin.acceptableInput
                                 && (movieEnd.text.trim() === "" || movieEnd.acceptableInput)
                        onClicked: mp4Dialog.open()
                    }
                    Label {
                        Layout.fillWidth: true
                        text: movie.message
                        wrapMode: Text.Wrap
                        visible: text.length > 0
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

                ReplayCanvas {
                    id: canvas
                    anchors.fill: parent
                    anchors.margins: 1
                    controller: replay
                    exporter: movie
                    onInsetSelected: id => insetEditor.selectedId = id
                    onSceneSelected: id => sceneLayerEditor.selectedId = id
                }

                Rectangle {
                    z: 4
                    visible: !window.videoOnly && !window.cloudOnly
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
                    z: 4
                    visible: !window.videoOnly
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
                        Button { text: "撤销视频"; visible: replay.videoLayers.length > 0; enabled: !movie.exporting && !replay.loading && replay.canUndo; onClicked: replay.undo() }
                        Button { text: "重做视频"; visible: replay.videoLayers.length > 0; enabled: !movie.exporting && !replay.loading && replay.canRedo; onClicked: replay.redo() }
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
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        visible: movie.exporting
        color: window.panel
        border.color: window.thin
        radius: 8
        width: Math.min(parent.width - 40, 520)
        height: exportProgress.implicitHeight + 24
        RowLayout {
            id: exportProgress
            anchors.fill: parent
            anchors.margins: 12
            Caption { text: Math.round(movie.progress * 100) + "%" }
            ProgressBar { Layout.fillWidth: true; value: movie.progress }
            Button { text: "取消导出"; onClicked: movie.cancel() }
        }
    }

}
