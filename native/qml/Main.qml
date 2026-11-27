import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import UAVStudio

ApplicationWindow {
    id: window
    objectName: "studioWindow"
    visible: !smokeTest
    width: 1440; height: 900
    minimumWidth: 980; minimumHeight: 640
    title: "UAV Studio — All-in-One Robotics Research Studio"
    color: appearance.colors.window
    palette: StudioPalette {}
font.pixelSize: appearance.metrics.body
    property alias workspaceIndex: workspace.currentIndex
    property int selectedVideo: 0
    property bool timelineFloating: false
    property int timelineMode: 0
    property var frameOptions: ({})
    property real frameTime: 0
    property bool frameSettingsVisible: false
    readonly property bool exportBusy: movie.exporting || frameBatch.active
    property var batchOptions: ({})
    readonly property var topicInspector: inspector
    readonly property color panel: appearance.colors.panel
    readonly property color thin: appearance.colors.border
    readonly property color ink: appearance.colors.text
    readonly property color muted: appearance.colors.secondaryText
    readonly property color accent: appearance.colors.accent

    header: ToolBar {
        height: 64
        background: Rectangle { color: appearance.colors.window }
        RowLayout {
            anchors.fill: parent; anchors.margins: 12; spacing: 12
            StudioLabel { text: "UAV Studio"; font.pixelSize: 18; font.weight: Font.DemiBold; Layout.preferredWidth: 154 }
            TabBar {
                id: workspace
                Layout.fillWidth: true
                enabled: !window.exportBusy
                background: Rectangle { color: appearance.colors.control; radius: appearance.metrics.radius }
                onCurrentIndexChanged: if (currentIndex < 3) settingsSection.currentIndex = currentIndex === 0 ? 0 : 1
                WorkspaceTab { text: "Visualization\n数据可视化" }
                WorkspaceTab { text: "Synchronization\n编辑与同步" }
                WorkspaceTab { text: "Video Editor\n视频剪辑" }
                WorkspaceTab { text: "Trajectory Studio\n轨迹图生成" }
            }
            StudioComboBox {
                objectName: "appearancePicker"
                Layout.preferredWidth: 152
                model: ["Light · 亮色", "Dark · 深色", "System · 跟随系统"]
                currentIndex: appearance.mode
                onActivated: appearance.mode = currentIndex
                Accessible.name: "Appearance · 外观"
                StudioToolTip { visible: parent.hovered; text: "Appearance · 外观，自动保存；不改变场景和输出配色" }
            }
            StudioButton { text: "About"; onClicked: about.open() }
        }
    }
    component WorkspaceTab: TabButton {
        id: tab
        implicitHeight: 40
        contentItem: Text { text: tab.text; font.pixelSize: 12; color: tab.checked ? appearance.colors.text : appearance.colors.secondaryText; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: appearance.metrics.radius; color: tab.checked ? appearance.colors.panel : tab.hovered ? appearance.colors.hover : appearance.colors.transparent; border.width: tab.visualFocus ? 2 : 0; border.color: appearance.colors.accent }
    }
    component PanelTitle: StudioLabel { font.pixelSize: appearance.metrics.section; font.weight: Font.DemiBold; color: appearance.colors.text }
    component Caption: StudioLabel { font.pixelSize: appearance.metrics.caption; color: appearance.colors.secondaryText }
    Dialog { palette: StudioPalette {}

        id: about; title: "About UAV Studio"; width: 420; anchors.centerIn: parent; modal: true; standardButtons: Dialog.Ok
        contentItem: StudioLabel { text: "UAV Studio\nAll-in-One Robotics Research Studio\n无人机科研工作站\nC++ / Qt · Development Preview\n完整验收通过前不发布稳定版"; wrapMode: Text.Wrap; padding: 16 }
    }
    Window {
        id: timelineWindow; title: "UAV Studio · Floating Timeline"; width: 900; height: Math.max(120, timelineContents.implicitHeight + 24); transientParent: window; flags: Qt.Tool
        visible: window.timelineFloating; color: appearance.colors.panel
        onClosing: window.timelineFloating = false
    }
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
        id: rvizExportDialog
        title: "导出 RViz 配置 · 保留原插件属性"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "rviz"
        nameFilters: ["RViz configuration (*.rviz)"]
        onAccepted: replay.exportRviz(selectedFile)
    }
    FileDialog {
        id: projectDialog
        title: "打开 UAV Studio 项目（兼容旧格式）"
        nameFilters: ["UAV Studio 项目（兼容旧格式） (*.slamfpv)"]
        onAccepted: replay.openProject(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: "保存 UAV Studio 项目（兼容旧格式）"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "slamfpv"
        nameFilters: ["UAV Studio 项目（兼容旧格式） (*.slamfpv)"]
        onAccepted: replay.saveProjectAs(selectedFile)
    }
    FileDialog {
        id: pngDialog
        title: "导出当前画面"
        fileMode: FileDialog.SaveFile
        defaultSuffix: selectedNameFilter.extensions[0] ?? "png"
        nameFilters: ["PNG (*.png)", "JPEG (*.jpg)", "TIFF (*.tiff)", "PDF — embedded raster (*.pdf)", "SVG — embedded raster (*.svg)"]
        onAccepted: movie.startStill(selectedFile, canvas, window.frameTime, window.frameOptions)
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
                                movieFps.value, movieQuality.value, movieSample.currentIndex + 1, movieEncoder.currentIndex === 1)
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
        id: frameBatchDialog
        title: "批量导出：选择上级目录（自动创建独立子目录）"
        onAccepted: frameBatch.start(selectedFolder, canvas, window.batchOptions)
    }
    FolderDialog {
        id: packageDialog
        title: "选择打包项目的上级文件夹"
        onAccepted: replay.exportProjectFolder(selectedFolder)
    }
    Shortcut { sequence: StandardKey.Open; enabled: !window.exportBusy && !replay.loading; onActivated: projectDialog.open() }
    Shortcut { sequence: StandardKey.SaveAs; enabled: !window.exportBusy && !replay.loading; onActivated: saveDialog.open() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: workspace.currentIndex < 3 && !window.exportBusy && !replay.loading && replay.canUndo; onActivated: replay.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: workspace.currentIndex < 3 && !window.exportBusy && !replay.loading && replay.canRedo; onActivated: replay.redo() }
    Shortcut { sequence: "Space"; enabled: !window.exportBusy && workspace.currentIndex < 3; onActivated: replay.togglePlayback() }
    Shortcut { sequence: "Left"; enabled: !window.exportBusy && workspace.currentIndex < 3; onActivated: { replay.pause(); replay.seek(replay.position - 1 / 60) } }
    Shortcut { sequence: "Right"; enabled: !window.exportBusy && workspace.currentIndex < 3; onActivated: { replay.pause(); replay.seek(replay.position + 1 / 60) } }
    Shortcut { sequence: "Ctrl+Shift+E"; enabled: workspace.currentIndex < 3 && !window.exportBusy && !replay.loading; onActivated: { replay.pause(); window.frameOptions = framePanel.options(); window.frameTime = replay.position; pngDialog.open() } }


    TrajectoryPane { anchors.fill: parent; anchors.margins: 16; visible: workspace.currentIndex === 3; editor: trajectory }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 16; spacing: 12
        enabled: !window.exportBusy
        visible: workspace.currentIndex < 3
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12
            Rectangle {
                Layout.preferredWidth: Math.max(180, Math.min(250, window.width * .18)); Layout.fillHeight: true
                color: appearance.colors.panel; radius: appearance.metrics.panelRadius; border.color: appearance.colors.border
                ScrollView {
        ScrollBar.vertical: StudioScrollBar {}
        ScrollBar.horizontal: StudioScrollBar {}
                    id: layerScroll; anchors.fill: parent; anchors.margins: 12; contentWidth: availableWidth; clip: true
                    ColumnLayout {
                        width: layerScroll.availableWidth; spacing: 10
                        PanelTitle { text: "Layers · 图层" }
                        StudioButton { text: "+ ROS Bag"; Layout.fillWidth: true; enabled: !replay.loading; onClicked: bagDialog.open() }
                        StudioButton { text: "+ Video · 视频"; Layout.fillWidth: true; enabled: !replay.loading; onClicked: videoDialog.open() }
                        Caption { text: replay.name; Layout.fillWidth: true; elide: Text.ElideMiddle }
                        Caption { text: workspace.currentIndex === 1 ? "实验时间 → 视频时间\n使用 Inspector 编辑同步锚点" : workspace.currentIndex === 2 ? "选择图层以编辑；拖动时间轴边缘裁剪" : "共享三维场景与独立机位"; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        StudioButton { text: "3D Scene · 三维场景"; Layout.fillWidth: true; onClicked: settingsSection.currentIndex = 2 }
                        Repeater {
                            model: replay.cloudViews
                            delegate: StudioButton { required property var modelData; text: "↳ " + modelData.label; Layout.fillWidth: true; onClicked: { insetEditor.selectedId = modelData.id; settingsSection.currentIndex = 2 } }
                        }
                        Repeater {
                            model: replay.videoLayers
                            delegate: StudioButton { required property int index; required property var modelData; text: "▸ " + modelData.label; Layout.fillWidth: true; highlighted: index === window.selectedVideo; onClicked: { window.selectedVideo = index; settingsSection.currentIndex = 1 } }
                        }
                        Repeater {
                            model: replay.sceneLayers
                            delegate: StudioButton { required property var modelData; text: modelData.label; Layout.fillWidth: true; onClicked: { sceneLayerEditor.selectedId = modelData.id; settingsSection.currentIndex = 1 } }
                        }
                        StudioCheckBox { text: "Speed Meter · 速度表"; checked: replay.speedGauge.visible; onClicked: replay.setSpeedGauge({visible: checked}) }
                        StudioButton { text: "Topic Inspector · 话题"; Layout.fillWidth: true; onClicked: settingsSection.currentIndex = 4 }
                        StudioButton { text: "Open Project · 打开项目"; Layout.fillWidth: true; onClicked: projectDialog.open() }
                        StudioButton { text: "Save As · 另存为"; Layout.fillWidth: true; onClicked: saveDialog.open() }
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    PanelTitle { text: ["Viewport · 三维画布", "Synchronization · 同步预览", "Canvas · 视频画布"][workspace.currentIndex] ?? "" }
                    Item { Layout.fillWidth: true }
                    StudioButton { text: "重置视角"; onClicked: canvas.resetCamera() }
                    StudioButton { text: "📷"; Accessible.name: "Export Frame · 导出当前画面"; onClicked: { window.frameSettingsVisible = true; settingsSection.currentIndex = 3 } StudioToolTip { visible: parent.hovered; text: "Export Frame · 导出当前画面 · Ctrl+Shift+E" } }
                }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: appearance.colors.preview
                border.color: window.thin
                radius: 8
                clip: true

                ReplayCanvas {
                    id: canvas
                    objectName: "previewCanvas"
                    anchors.centerIn: parent
                    width: Math.max(0, Math.min(parent.width - 2, (parent.height - 2) * 16 / 9))
                    height: width * 9 / 16
                    controller: replay
                    exporter: movie
                    interactive: !window.exportBusy
                    onInsetSelected: id => insetEditor.selectedId = id
                    onSceneSelected: id => sceneLayerEditor.selectedId = id
                }

            }
                Caption { text: replay.worldFrame + " · " + Number(replay.pointCount).toLocaleString(Qt.locale(), "f", 0) + " points · " + (replay.demo ? "Demo · 模拟数据" : "ROS Recording · 实验记录"); Layout.fillWidth: true; elide: Text.ElideRight }
            }
        Rectangle {
            Layout.preferredWidth: Math.min(280, window.width * .22)
            Layout.minimumWidth: 216
            Layout.maximumWidth: 280
            Layout.fillHeight: true
            color: window.panel
            border.color: window.thin
            radius: 10

            StudioComboBox {
                id: settingsSection
                objectName: "settingsSection"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                model: ["Data · 数据与项目", "Media · 视频与图层", "View · 视角与机位", "Export · 导出", "Topic Inspector · 消息检查"]
                onActivated: settingsScroll.contentItem.contentY = 0
            }
            ScrollView {
        ScrollBar.vertical: StudioScrollBar {}
        ScrollBar.horizontal: StudioScrollBar {}
                id: settingsScroll
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: settingsSection.bottom
                anchors.bottom: parent.bottom
                anchors.margins: 12
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: settingsScroll.availableWidth
                    spacing: 9

                    TopicInspector {
                        visible: settingsSection.currentIndex === 4
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(580, settingsScroll.height - 24)
                        inspector: window.topicInspector
                        controller: replay
                    }

                    FrameExportPanel {
                        id: framePanel
                        visible: settingsSection.currentIndex === 3 && window.frameSettingsVisible
                        Layout.fillWidth: true
                        controller: replay
                        onSaveRequested: (options, time) => { window.frameOptions = options; window.frameTime = time; pngDialog.open() }
                        onBatchRequested: options => { window.batchOptions = options; frameBatchDialog.open() }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: settingsSection.currentIndex === 0
                        StudioButton {
                            text: "打开 ROS1 bag"
                            Layout.fillWidth: true
                            enabled: !replay.loading
                            highlighted: true
                            onClicked: bagDialog.open()
                        }
                        StudioButton {
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
                            StudioButton { text: "打开项目"; enabled: !window.exportBusy && !replay.loading; onClicked: projectDialog.open() }
                            StudioButton { text: "另存为"; enabled: !window.exportBusy && !replay.loading; onClicked: saveDialog.open() }
                            StudioButton { text: "打包"; enabled: !window.exportBusy && !replay.loading; onClicked: packageDialog.open() }
                        }
                        Caption {
                            Layout.fillWidth: true
                            visible: replay.projectPath.length > 0
                            text: replay.projectPath
                            elide: Text.ElideMiddle
                        }

                        PanelTitle { text: "数据来源" }
                        Caption { text: "点云话题" }
                        StudioSearchComboBox {
                            Layout.fillWidth: true
                            sourceModel: replay.cloudTopics
                            selectedText: replay.cloudTopic
                            enabled: !replay.loading && !replay.demo
                            onChosen: value => replay.setCloudTopic(value)
                        }
                        Caption { text: "里程计 / 速度话题" }
                        StudioSearchComboBox {
                            Layout.fillWidth: true
                            sourceModel: replay.odometryTopics
                            selectedText: replay.odometryTopic
                            enabled: !replay.loading && !replay.demo && sourceModel.length > 0
                            onChosen: value => replay.setOdometryTopic(value)
                        }
                        Caption { text: "位姿来源" }
                        StudioComboBox {
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
                            StudioLabel { text: "世界坐标系" }
                            StudioSearchComboBox {
                                id: poseWorld
                                Layout.fillWidth: true
                                sourceModel: replay.tfFrames
                                selectedText: replay.worldFrame
                                onChosen: value => replay.setPoseSource(true, value, replay.bodyFrame)
                            }
                            StudioLabel { text: "机体坐标系" }
                            StudioSearchComboBox {
                                id: poseBody
                                Layout.fillWidth: true
                                sourceModel: replay.tfFrames
                                selectedText: replay.bodyFrame
                                onChosen: value => replay.setPoseSource(true, replay.worldFrame, value)
                            }
                        }
                        Caption { text: "速度话题" }
                        StudioComboBox {
                            Layout.fillWidth: true
                            model: ["自动识别"].concat(replay.speedTopics)
                            currentIndex: Math.max(0, model.indexOf(replay.speedSelection))
                            enabled: !replay.loading && !replay.demo
                            onActivated: index => replay.setSpeedTopic(index > 0 ? currentText : "")
                        }
                        StudioCheckBox {
                            text: "整图替换"
                            checked: replay.replacing
                            enabled: !replay.loading && !replay.demo
                            onClicked: replay.setReplacing(checked)
                        }

                        Caption { text: "Fixed Frame · 固定坐标系" }
                        StudioSearchComboBox {
                            Layout.fillWidth: true
                            sourceModel: replay.tfFrames
                            selectedText: replay.worldFrame
                            enabled: !replay.loading && !replay.demo
                            onChosen: value => replay.setFixedFrame(value)
                        }
                        StudioButton {
                            text: "导入 RViz 配置"
                            Layout.fillWidth: true
                            enabled: !replay.loading
                            onClicked: rvizDialog.open()
                        }
                        StudioButton {
                            text: "Export RViz · 导出 RViz 配置"
                            Layout.fillWidth: true; enabled: !replay.loading && (replay.topicDisplays.length > 0 || replay.rvizCompatibility.length > 0)
                            onClicked: rvizExportDialog.open()
                        }
                        Caption { text: replay.rvizCompatibility; visible: text.length > 0; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        StudioButton {
                            id: topicDisplayToggle
                            Layout.fillWidth: true
                            visible: replay.topicDisplays.length > 0
                            text: (checked ? "收起" : "展开") + " RViz 话题显示（" + replay.topicDisplays.length + "）"
                            checkable: true
                        }
                        Repeater {
                            model: topicDisplayToggle.checked ? replay.topicDisplays : []
                            delegate: StudioCheckBox {
                                id: topicDisplay
                                required property int index
                                required property var modelData
                                Layout.fillWidth: true
                                checked: modelData.enabled
                                text: modelData.label
                                enabled: !replay.loading
                                onClicked: replay.setTopicDisplayEnabled(index, checked)
                                StudioToolTip { visible: topicDisplay.hovered; text: topicDisplay.text }
                                contentItem: StudioLabel {
                                    text: topicDisplay.text
                                    color: appearance.colors.text
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
                            delegate: StudioCheckBox {
                                required property var modelData
                                readonly property var plannerLayer: replay.planningLayers[modelData.key]
                                visible: plannerLayer !== undefined
                                checked: plannerLayer?.enabled ?? false
                                text: modelData.label
                                onClicked: replay.setPlanningLayerEnabled(modelData.key, checked)
                                StudioToolTip { visible: parent.hovered; text: parent.plannerLayer?.topic ?? "" }
                            }
                        }
                        StudioButton {
                            id: scanExpand
                            visible: replay.scanTopics.length > 0
                            text: checked ? "收起当前扫描设置" : "当前扫描设置"
                            checkable: true
                        }
                        ColumnLayout {
                            visible: scanExpand.visible && scanExpand.checked
                            Layout.fillWidth: true
                            StudioComboBox {
                                id: scanChoice
                                Layout.fillWidth: true
                                model: ["关闭", ...replay.scanTopics]
                                currentIndex: Math.max(0, replay.scanTopics.indexOf(replay.scanTopic) + 1)
                            }
                            RowLayout {
                                StudioLabel { text: "最大保留秒数" }
                                StudioTextField {
                                    id: scanAge
                                    Layout.fillWidth: true
                                    text: Number(replay.scanMaximumAge).toLocaleString(Qt.locale(), "f", 2)
                                    validator: DoubleValidator { bottom: .001; top: 60 }
                                }
                            }
                            StudioButton {
                                text: "应用当前扫描"
                                enabled: !replay.loading && scanAge.acceptableInput
                                onClicked: replay.setScanOptions(scanChoice.currentIndex === 0 ? "" : scanChoice.currentText,
                                                                Number.fromLocaleString(scanAge.text))
                            }
                        }
                        StudioCheckBox {
                            visible: replay.planningLayers.plan !== undefined
                            checked: replay.comparePlans
                            text: "对比上一条规划"
                            onClicked: replay.setComparePlans(checked)
                        }
                        StudioComboBox {
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
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: settingsSection.currentIndex === 1
                        PanelTitle { text: "实拍视频"; Layout.topMargin: 8 }
                        StudioButton {
                            Layout.fillWidth: true
                            text: "添加视频"
                            enabled: !replay.loading
                            onClicked: videoDialog.open()
                        }
                        StudioComboBox {
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
                                StudioTextField { id: trimIn; Layout.fillWidth: true; text: videoSettings.selectedClip?.trimIn ?? "" }
                                Caption { text: "结束（秒）" }
                                StudioTextField { id: trimOut; Layout.fillWidth: true; text: videoSettings.selectedClip?.trimOut ?? ""; placeholderText: "视频末尾" }
                                Caption { text: "偏移（秒）" }
                                StudioTextField { id: offset; Layout.fillWidth: true; text: videoSettings.selectedClip?.offset ?? "" }
                            }
                            StudioButton {
                                Layout.fillWidth: true
                                text: "应用裁剪与偏移"
                                onClicked: replay.setVideoTrim(videoPicker.currentIndex, trimIn.text, trimOut.text, offset.text)
                            }
                            Caption {
                                text: "暂停在回放事件处，输入实拍中同一事件的时间"
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                            StudioTextField { id: mediaTime; Layout.fillWidth: true; placeholderText: "视频时间（秒）" }
                            RowLayout {
                                StudioButton { text: "锚点 1"; enabled: !replay.playing; onClicked: replay.setVideoAnchor(videoPicker.currentIndex, 0, mediaTime.text) }
                                StudioButton { text: "锚点 2"; enabled: !replay.playing && (videoSettings.selectedClip?.anchors.length ?? 0) > 0; onClicked: replay.setVideoAnchor(videoPicker.currentIndex, 1, mediaTime.text) }
                                StudioButton { text: "清除"; enabled: (videoSettings.selectedClip?.anchors.length ?? 0) > 0; onClicked: replay.clearVideoAnchors(videoPicker.currentIndex) }
                            }
                        }

                        StudioButton {
                            Layout.fillWidth: true
                            text: "查找对齐候选"
                            enabled: !replay.loading && replay.videoLayers.length > 0
                            onClicked: {
                                alignment.model = replay.alignmentCandidates()
                                alignment.currentIndex = -1
                            }
                        }
                        StudioComboBox {
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
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: settingsSection.currentIndex === 2
                        PanelTitle { text: "View · 视口"; Layout.topMargin: 8 }
                        Caption { text: "Scene Background · 场景背景" }
                        StudioTextField {
                            Layout.fillWidth: true; text: replay.sceneBackground.toString()
                            placeholderText: "#RRGGBB · 独立场景颜色"
                            validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
                            onEditingFinished: if (acceptableInput) replay.sceneBackground = text
                        }
                        Caption { text: "场景与输出配色独立于软件外观"; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        StudioComboBox {
                            Layout.fillWidth: true
                            model: replay.cameraLabels
                            currentIndex: replay.cameraIndex
                            enabled: !replay.loading
                            onActivated: replay.setCamera(currentIndex)
                        }
                        StudioCheckBox {
                            text: "显示已飞轨迹"
                            checked: true
                            onToggled: canvas.showPath = checked
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Caption { text: "点大小" }
                            StudioSlider {
                                Layout.fillWidth: true
                                from: 1; to: 6; value: 1.5; stepSize: .25
                                onValueChanged: canvas.pointSize = value
                            }
                            Caption { text: canvas.pointSize.toFixed(1) + " px" }
                        }
                        StudioCheckBox {
                            text: "透明画布"
                            checked: replay.transparentCanvas
                            enabled: !replay.loading && !replay.capturing
                            onClicked: replay.setTransparentCanvas(checked)
                        }
                        StudioButton {
                            text: "导出当前帧 PNG"
                            enabled: !replay.loading && !replay.capturing
                            Layout.fillWidth: true
                            onClicked: {
                                window.frameSettingsVisible = true
                                settingsSection.currentIndex = 3
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
                        StudioCheckBox {
                            text: "显示合成速度表"
                            checked: replay.speedGauge.visible
                            onClicked: replay.setSpeedGauge({visible: checked})
                        }
                        GridLayout {
                            visible: replay.speedGauge.visible
                            columns: 2
                            Layout.fillWidth: true
                            StudioLabel { text: "量程（m/s）" }
                            StudioTextField { id: gaugeLimit; Layout.fillWidth: true; text: replay.speedGauge.limit }
                            StudioLabel { text: "缩放" }
                            StudioTextField { id: gaugeScale; Layout.fillWidth: true; text: replay.speedGauge.scale }
                            StudioLabel { text: "开始（秒）" }
                            StudioTextField { id: gaugeStart; Layout.fillWidth: true; text: replay.speedGauge.start }
                            StudioLabel { text: "结束（秒）" }
                            StudioTextField {
                                id: gaugeEnd
                                Layout.fillWidth: true
                                text: replay.speedGauge.end < 0 ? "" : replay.speedGauge.end
                            }
                            StudioCheckBox {
                                id: gaugeClear
                                text: "透明"
                                checked: replay.speedGauge.transparent
                            }
                            StudioLabel { text: "Output Style" }
                            StudioComboBox { id: gaugeScheme; model: ["Dark · 深色", "Light · 亮色"]; currentIndex: replay.speedGauge.scheme === "light" ? 1 : 0 }
                            StudioButton {
                                text: "应用速度表"
                                onClicked: replay.setSpeedGauge({limit: Number(gaugeLimit.text), scale: Number(gaugeScale.text),
                                    start: Number(gaugeStart.text), end: gaugeEnd.text.trim() === "" ? -1 : Number(gaugeEnd.text),
                                    transparent: gaugeClear.checked, scheme: gaugeScheme.currentIndex === 0 ? "dark" : "light"})
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: settingsSection.currentIndex === 3
                        PanelTitle { text: "导出视频" }
                        GridLayout {
                            columns: 2
                            Layout.fillWidth: true
                            StudioLabel { text: "开始（秒）" }
                            StudioTextField {
                                id: movieBegin
                                text: "0"
                                Layout.fillWidth: true
                                validator: DoubleValidator { bottom: 0; locale: "C" }
                            }
                            StudioLabel { text: "结束（秒）" }
                            StudioTextField {
                                id: movieEnd
                                placeholderText: "回放末尾"
                                Layout.fillWidth: true
                                validator: DoubleValidator { bottom: 0; locale: "C" }
                            }
                            StudioLabel { text: "分辨率" }
                            StudioComboBox { id: movieSize; model: ["720p", "1080p", "2160p"]; currentIndex: 1 }
                            StudioLabel { text: "帧率" }
                            SpinBox { palette: StudioPalette {}
 id: movieFps; from: 1; to: 120; value: 60; editable: true }
                            StudioLabel { text: "编码器" }
                            StudioComboBox { id: movieEncoder; model: ["CPU", "NVIDIA NVENC"] }
                            StudioLabel { text: "超采样" }
                            StudioComboBox { id: movieSample; model: ["1×", "2×"] }
                            StudioLabel { text: "CRF" }
                            SpinBox { palette: StudioPalette {}
 id: movieQuality; from: 0; to: 51; value: 18; editable: true }
                            StudioLabel { text: "速度" }
                            StudioComboBox {
                                id: movieSpeed
                                Layout.fillWidth: true
                                model: ["1×", "0.5×", "0.25×"]
                                currentIndex: Math.max(0, [1., .5, .25].indexOf(replay.moviePreferences.speed))
                                onActivated: replay.setMoviePreferences([1., .5, .25][currentIndex], movieAudio.currentIndex - 1)
                            }
                            StudioLabel { text: "音轨" }
                            StudioComboBox {
                                id: movieAudio
                                Layout.fillWidth: true
                                model: ["无音轨"].concat(replay.videoLayers.map(layer => layer.label))
                                currentIndex: Math.max(0, Math.min(count - 1, replay.moviePreferences.audioIndex + 1))
                                onActivated: replay.setMoviePreferences([1., .5, .25][movieSpeed.currentIndex], currentIndex - 1)
                            }
                        }
                        StudioButton {
                            text: "导出 MP4"
                            Layout.fillWidth: true
                            enabled: !replay.loading && !replay.capturing && replay.duration > 0 && movieBegin.acceptableInput
                                     && (movieEnd.text.trim() === "" || movieEnd.acceptableInput)
                            onClicked: mp4Dialog.open()
                        }
                        StudioLabel {
                            Layout.fillWidth: true
                            text: movie.message
                            wrapMode: Text.Wrap
                            visible: text.length > 0
                        }
                        StudioButton {
                            text: "重置视角"
                            Layout.fillWidth: true
                            onClicked: canvas.resetCamera()
                        }
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
                    StudioButton {
                        text: "取消读取"
                        Layout.fillWidth: true
                        visible: replay.loading
                        onClicked: replay.cancel()
                    }
                }
            }
        }
        }
        Item {
            id: timelineDock; Layout.fillWidth: true
            Layout.preferredHeight: window.timelineFloating ? 0 : timelineContents.implicitHeight + 24
            Rectangle {
                id: timelinePanel
                parent: window.timelineFloating ? timelineWindow.contentItem : timelineDock
                anchors.fill: parent
                radius: 8
                border.color: window.thin
                color: window.panel
                ColumnLayout {
                    id: timelineContents
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 5

                    RowLayout {
                        Layout.fillWidth: true
                        PanelTitle { text: "Timeline · 时间轴" }
                        Item { Layout.fillWidth: true }
                        StudioComboBox { model: ["Full · 完整", "Compact · 紧凑", "Mini · 迷你", "Collapsed · 折叠"]; currentIndex: window.timelineMode; onActivated: window.timelineMode = currentIndex }
                        StudioButton { text: window.timelineFloating ? "停靠" : "浮动"; onClicked: window.timelineFloating = !window.timelineFloating }
                    }
                    MarkerTimeline {
                        visible: window.timelineMode < 2
                        Layout.fillWidth: true
                        controller: replay
                        labelWidth: 124
                        accent: window.accent
                    }

                    VideoTimeline {
                        Layout.fillWidth: true
                        visible: window.timelineMode === 0 && replay.videoLayers.length + replay.sceneLayers.length > 0
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
                        StudioButton {
                            text: replay.playing ? "暂停" : "播放"
                            highlighted: true
                            enabled: !replay.loading && replay.duration > 0
                            onClicked: replay.togglePlayback()
                        }
                        StudioButton {
                            text: "分割"
                            visible: window.timelineMode < 2 && replay.videoLayers.length > 0
                            enabled: !replay.loading && videoPicker.currentIndex >= 0
                            onClicked: replay.splitVideo(videoPicker.currentIndex, replay.position)
                        }
                        StudioButton {
                            text: "删除片段"
                            visible: window.timelineMode < 2 && replay.videoLayers.length > 0
                            enabled: !replay.loading && videoPicker.currentIndex >= 0
                            onClicked: replay.deleteVideoSegment(videoPicker.currentIndex, replay.position)
                        }
                        StudioButton { text: "撤销视频"; visible: window.timelineMode < 2 && replay.videoLayers.length > 0; enabled: !window.exportBusy && !replay.loading && replay.canUndo; onClicked: replay.undo() }
                        StudioButton { text: "重做视频"; visible: window.timelineMode < 2 && replay.videoLayers.length > 0; enabled: !window.exportBusy && !replay.loading && replay.canRedo; onClicked: replay.redo() }
                        ColumnLayout {
                            visible: window.timelineMode < 3
                            spacing: 3
                            Layout.fillWidth: true
                            StudioSlider {
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
        visible: window.exportBusy
        color: window.panel
        border.color: window.thin
        radius: 8
        width: Math.min(parent.width - 40, 520)
        height: exportProgress.implicitHeight + 24
        RowLayout {
            id: exportProgress
            anchors.fill: parent
            anchors.margins: 12
            Caption { text: Math.round((frameBatch.active ? frameBatch.progress : movie.progress) * 100) + "%" }
            ProgressBar { Layout.fillWidth: true; value: frameBatch.active ? frameBatch.progress : movie.progress }
            StudioButton { text: "取消导出"; onClicked: frameBatch.active ? frameBatch.cancel() : movie.cancel() }
        }
    }
}
