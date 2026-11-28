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
    title: appInfo.fullBranding
    color: appearance.colors.window
    palette: StudioPalette {}
    font.pixelSize: appearance.metrics.body
    property alias workspaceIndex: workspace.currentIndex
    property int selectedVideo: 0
    property int sidebarIndex: 0
    property url nativeStartupConfig: ""
    // Standalone editing is always available. An optional native runtime is
    // detected asynchronously and can be selected without managing a process.
    property bool nativeBackendSelected: replay.renderBackend === "native-rviz"
    readonly property bool nativeBackendActive: nativeBackendSelected && nativeRviz.connected
    property bool backendSettingsVisible: false
    property string pendingNativeYaml: ""
    property string nativeSaveJob: ""
    property bool nativeSaveWaiting: false
    property url nativeSaveDestination: ""
    property string nativeSaveKind: ""
    property string nativeSaveSignature: ""
    property string nativeSaveViewSignature: ""
    property string nativeSaveError: ""
    Component.onCompleted: { replay.canvas.layoutMode = false; if (automaticNativeBackend) nativeRviz.ensureStarted() }
    property bool timelineFloating: false
    property int timelineMode: 0
    property var frameOptions: ({})
    property real frameTime: 0
    property bool frameSettingsVisible: false
    readonly property bool exportBusy: movie.exporting || frameBatch.active || nativeSaveJob.length > 0
    readonly property bool textInputFocused: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit
    property var batchOptions: ({})
    readonly property var movieRateValues: [1., .5, .25, 2., 4.]
    readonly property var movieRateLabels: movieRateValues.map(rate => String(rate) + "×")
    readonly property var topicInspector: inspector
    property string selectedMeterId: ""
    property var copiedMeterStyle: null
    property string meterExportFormat: "png"
    property string meterExportMessage: ""
    property var copiedCanvasElements: []
    readonly property var activeCanvasModel: workspace.currentIndex === 3 ? trajectory.canvas : replay.canvas
    readonly property var selectedMeter: replay.speedMeters.find(row => row.id === selectedMeterId) || replay.speedMeters[0] || ({})
    readonly property color panel: appearance.colors.panel
    readonly property color thin: appearance.colors.separator
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
                background: Rectangle { color: appearance.colors.transparent; radius: appearance.metrics.radius }
                onCurrentIndexChanged: if (currentIndex < 3) { settingsSection.currentIndex = currentIndex === 0 ? 0 : 1; replay.canvas.layoutMode = currentIndex !== 0 }
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
            StudioButton { objectName: "aboutButton"; text: "About · 关于"; onClicked: about.open() }
        }
    }
    component WorkspaceTab: TabButton {
        id: tab
        implicitHeight: 40
        contentItem: Text { text: tab.text; font.pixelSize: appearance.metrics.body; font.weight: tab.checked ? Font.DemiBold : Font.Normal; color: tab.checked ? appearance.colors.text : appearance.colors.secondaryText; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: appearance.metrics.radius; color: tab.checked ? appearance.colors.selection : tab.hovered ? appearance.colors.hover : appearance.colors.transparent; border.width: tab.visualFocus ? 2 : 0; border.color: appearance.colors.accent }
    }
    component PanelTitle: StudioLabel { font.pixelSize: appearance.metrics.section; font.weight: Font.DemiBold; color: appearance.colors.text }
    component Caption: StudioLabel { font.pixelSize: appearance.metrics.caption; color: appearance.colors.secondaryText }
    AboutPane {
        id: about
        parent: Overlay.overlay
        applicationInfo: appInfo
        updateManager: updater
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
        onAccepted: window.topicInspector.openBag(selectedFile)
    }
    FileDialog {
        id: meterExportDialog
        title: "Export Speed Meter · 导出速度仪表"
        fileMode: FileDialog.SaveFile
        defaultSuffix: window.meterExportFormat
        nameFilters: [window.meterExportFormat.toUpperCase() + " (*." + window.meterExportFormat + ")"]
        onAccepted: {
            const id = window.selectedMeter.id
            if (window.meterExportFormat === "csv") {
                speedMeterData.exportCsv(id, selectedFile, Object.assign({}, window.selectedMeter.settings, {playheadNs:replay.experimentStampNs}))
                return
            }
            const item = canvas.layerItems[id]
            if (item) item.exportElement(selectedFile, Qt.size(2400,2080), 300, window.meterExportFormat)
        }
    }
    Connections { target: speedMeterData; function onExportFinished(success, message) { window.meterExportMessage = message } }
    Connections {
        target: canvas.layerItems[window.selectedMeter.id] || null
        ignoreUnknownSignals: true
        function onExportFinished(jobId, success, metadata, error) { window.meterExportMessage = success ? "Exported · 已导出 " + metadata.path : error }
    }
    FileDialog {
        id: rvizDialog
        title: "导入 RViz 显示配置"
        nameFilters: ["RViz 配置 (*.rviz *.yaml *.yml)"]
        onAccepted: window.importRviz(selectedFile)
    }
    FileDialog {
        id: rvizExportDialog
        title: "导出 RViz 配置 · 保留原插件属性"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "rviz"
        nameFilters: ["RViz configuration (*.rviz)"]
        onAccepted: window.nativeBackendActive ? nativeRviz.saveConfig(selectedFile) : replay.exportRviz(selectedFile)
    }
    FileDialog {
        id: projectDialog
        title: "打开 UAV Studio 项目（兼容旧格式）"
        nameFilters: ["UAV Studio 项目 (*.uavstudio *.slamfpv)"]
        onAccepted: replay.openProject(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: "保存 UAV Studio 项目（兼容旧格式）"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "uavstudio"
        nameFilters: ["UAV Studio 项目 (*.uavstudio *.slamfpv)"]
        onAccepted: window.saveProject(selectedFile, "project")
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
                                replay.moviePreferences.speed, movieAudio.currentIndex - 1,
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
        onAccepted: window.saveProject(selectedFolder, "package")
    }
    Shortcut { sequence: StandardKey.Open; enabled: !window.exportBusy && !replay.loading; onActivated: projectDialog.open() }
    Shortcut { sequence: StandardKey.SaveAs; enabled: !window.exportBusy && !replay.loading; onActivated: saveDialog.open() }
    Shortcut { sequence: StandardKey.Save; enabled: !window.exportBusy && !replay.loading; onActivated: {
        if (replay.projectPath.length) window.saveProject(Qt.resolvedUrl("file:///" + replay.projectPath.replace(/\\/g, "/")), "project")
        else saveDialog.open()
    } }
    Shortcut { sequences: [StandardKey.Copy]; enabled: !window.textInputFocused && window.activeCanvasModel.selection.length > 0; onActivated: window.copiedCanvasElements = window.activeCanvasModel.copySelected() }
    Shortcut { sequences: [StandardKey.Paste]; enabled: !window.textInputFocused && window.copiedCanvasElements.length > 0 && !window.exportBusy; onActivated: {
        const ids = window.activeCanvasModel.pasteElements(window.copiedCanvasElements)
        if (workspace.currentIndex < 3 && ids.length) window.selectCanvasLayer(ids[ids.length-1])
    } }
    CanvasDeleteShortcut {
        active: workspace.currentIndex >= 2 && !window.exportBusy &&
            (workspace.currentIndex === 3 ? !trajectory.working : !replay.loading)
        focusItem: window.activeFocusItem
        timelineItem: workspace.currentIndex === 2 && videoTimeline.visible ? videoTimeline : null
        selectionCount: window.activeCanvasModel.selection.length
        onDeleteCanvasSelection: window.activeCanvasModel.removeSelected()
        onDeleteTimelineSelection: videoTimeline.removeSelectedClip()
    }
    Shortcut { sequence: "Ctrl+Shift+I"; enabled: !window.exportBusy && !replay.loading; onActivated: rvizDialog.open() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: !window.textInputFocused && workspace.currentIndex < 3 && !window.exportBusy && !replay.loading && replay.canUndo; onActivated: replay.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: !window.textInputFocused && workspace.currentIndex < 3 && !window.exportBusy && !replay.loading && replay.canRedo; onActivated: replay.redo() }
    Shortcut { sequence: "Space"; enabled: !window.textInputFocused && !window.exportBusy && workspace.currentIndex < 3; onActivated: replay.togglePlayback() }
    Shortcut { sequence: "Left"; enabled: !window.textInputFocused && !window.exportBusy && workspace.currentIndex < 3; onActivated: { if (replay.canvas.selection.length && replay.canvas.layoutMode) canvas.nudge(-1, 0); else { replay.pause(); replay.seek(replay.position - 1 / 60) } } }
    Shortcut { sequence: "Right"; enabled: !window.textInputFocused && !window.exportBusy && workspace.currentIndex < 3; onActivated: { if (replay.canvas.selection.length && replay.canvas.layoutMode) canvas.nudge(1, 0); else { replay.pause(); replay.seek(replay.position + 1 / 60) } } }
    Shortcut { sequence: "Ctrl+Shift+E"; enabled: workspace.currentIndex < 3 && !window.exportBusy && !replay.loading; onActivated: { replay.pause(); window.frameOptions = framePanel.options(); window.frameTime = replay.position; pngDialog.open() } }

    function selectCanvasLayer(id, content = false) {
        const row = replay.canvas.layers.find(r => r.id === id)
        if (!row) return
        if (replay.canvas.selection.indexOf(id) < 0) replay.canvas.select(id)
        if (row.kind === "video") { window.selectedVideo = replay.videoLayers.findIndex(v => v.id === id); sceneLayerEditor.selectedId = "" }
        if (row.kind === "view" || row.kind === "main") insetEditor.selectedId = id === "main" ? "" : id
        if (!row.scientific && (row.kind === "image" || row.kind === "text")) sceneLayerEditor.selectedId = id
        if (row.kind === "speed") window.selectedMeterId = id
        settingsSection.currentIndex = !content || row.scientific ? 8 : (row.kind === "main" || row.kind === "view") ? 2 : row.kind === "speed" ? 6 : 1
        sidebarIndex = 2
        settingsScroll.contentItem.contentY = 0
    }
    function projectSignature() { return replay.projectPath + "|" + (replay.nativeReplaySource.revision || "") }
    function saveProject(destination, kind) {
        nativeSaveError = ""
        if (!window.nativeBackendActive) {
            nativeSaveWaiting = false
            if (kind === "package") replay.exportProjectFolder(destination)
            else replay.saveProjectAs(destination)
            return
        }
        nativeSaveDestination = destination
        nativeSaveKind = kind
        nativeSaveSignature = projectSignature()
        nativeSaveWaiting = true
        continueNativeSave()
    }
    function continueNativeSave() {
        if (!nativeSaveWaiting) return
        if (nativeSaveSignature !== projectSignature() || !nativeBackendActive) {
            nativeSaveWaiting = false
            nativeSaveError = "Project or native connection changed before saving. Please save the current project again.\n保存前项目或原生连接已改变，请重新保存当前项目。"
            return
        }
        const coordinator = canvas.nativeViewCoordinator
        if (replay.loading || movie.exporting || frameBatch.active || pendingNativeYaml.length
                || nativeRviz.busy || coordinator.configurationPending || coordinator.syncPending || coordinator.framePending) return
        if (coordinator.desiredSignature !== coordinator.actualSignature) {
            if (coordinator.failedSignature === coordinator.desiredSignature) {
                nativeSaveWaiting = false
                nativeSaveError = coordinator.syncError || "Native cameras could not be synchronized; the project was not saved.\n原生机位同步失败，本次项目未保存。"
            } else coordinator.tick()
            return
        }
        // Keep the coordinator active while waiting. Only the ready snapshot
        // locks editing and holds preview requests until its async result.
        nativeSaveViewSignature = coordinator.desiredSignature
        nativeSaveWaiting = false
        nativeSaveJob = nativeRviz.snapshotConfig()
    }
    Timer {
        interval: 50; repeat: true
        running: window.nativeSaveWaiting
        onTriggered: window.continueNativeSave()
    }
    Connections {
        target: replay
        function onRenderBackendChanged() {
            if (replay.renderBackend === "native-rviz" && automaticNativeBackend)
                nativeRviz.ensureStarted()
        }
        function onNativeConfigLoaded(config) {
            window.nativeStartupConfig = ""
            window.pendingNativeYaml = config.yaml || "Visualization Manager:\n  Displays: []\n  Global Options:\n    Fixed Frame: map\n"
            if (!nativeRviz.connected) {
                nativeRviz.loadConfigYaml(window.pendingNativeYaml)
                window.pendingNativeYaml = ""
            }
        }
        function onSourceRemoved(kind, file) { if (kind === "bag" && nativeRviz.connected) nativeRviz.closeBag() }
    }
    Timer {
        interval: 100; repeat: true
        running: window.pendingNativeYaml.length > 0 && nativeRviz.connected
        onTriggered: {
            if (nativeRviz.busy) return
            const yaml = window.pendingNativeYaml
            nativeRviz.loadConfigYaml(yaml)
            window.pendingNativeYaml = ""
        }
    }
    Shortcut { sequence: "V"; enabled: !window.textInputFocused && !window.exportBusy && workspace.currentIndex < 3; onActivated: replay.canvas.layoutMode = true }
    Shortcut { sequence: "C"; enabled: !window.textInputFocused && !window.exportBusy && workspace.currentIndex < 3; onActivated: replay.canvas.layoutMode = false }
    Shortcut { sequence: "Escape"; enabled: !window.textInputFocused && replay.canvas.selection.length > 0; onActivated: replay.canvas.clearSelection() }
    function importRviz(file) {
        // The controller retains the offline tree and publishes one YAML
        // restore request; the optional backend queues it before connecting.
        replay.openRviz(file)
        settingsSection.currentIndex = 5
        sidebarIndex = 0
    }
    Connections {
        target: nativeRviz
        property string lastSelected: ""
        function onModelChanged() {
            const id = nativeRviz.selectedId
            if (!id.length) { lastSelected = ""; return }
            if (lastSelected === id) return
            const selected = nativeRviz.selected || ({})
            let scope = selected.view_id ? "views" : ""
            if (!scope.length) {
                const root = nativeRviz.properties.find(row => row.depth === 0 && nativeInspector.containsNativeId(row, id))
                if (root) scope = nativeInspector.nativeRootScope(root)
            }
            if (!scope.length) return
            lastSelected = id
            if (scope === "views") {
                if (selected.view_id && selected.view_id !== "main") insetEditor.selectedId = selected.view_id
            } else nativeInspector.treeScope = scope
            const section = scope === "views" ? 2 : 5
            // Keep the camera editor and its scroll position when another
            // property of the same real controller is selected.
            if (settingsSection.currentIndex !== section) {
                settingsSection.currentIndex = section
                settingsScroll.contentItem.contentY = 0
            }
        }
        function onConfigSnapshotReady(jobId, success, yaml, metadata, error) {
            if (jobId !== window.nativeSaveJob) return
            window.nativeSaveJob = ""
            if (!success || window.nativeSaveSignature !== window.projectSignature()
                    || window.pendingNativeYaml.length > 0
                    || window.nativeSaveViewSignature !== canvas.nativeViewCoordinator.desiredSignature
                    || window.nativeSaveViewSignature !== canvas.nativeViewCoordinator.actualSignature) {
                window.nativeSaveError = success ? "Project or cameras changed while saving. Please save the current project again.\n保存期间项目或机位已改变，本次未写入文件。" : error
                return
            }
            replay.setNativeRvizConfig(yaml, metadata)
            if (window.nativeSaveKind === "package") replay.exportProjectFolder(window.nativeSaveDestination)
            else replay.saveProjectAs(window.nativeSaveDestination)
        }
        function onChanged() {
            if (!nativeRviz.connected && (window.nativeSaveWaiting || window.nativeSaveJob.length)) {
                window.nativeSaveWaiting = false
                window.nativeSaveJob = ""
                window.nativeSaveError = "Native RViz disconnected before the project snapshot was saved.\n原生连接中断，尚未保存本次项目。"
            }
        }
    }

    function droppedFileKind(urls) {
        if (!urls || urls.length === 0)
            return ""
        const videoPattern = /\.(mp4|mov|mkv|avi|webm)$/i
        let videos = true
        for (let i = 0; i < urls.length; ++i) {
            const path = urls[i].toString()
            if (!path.toLowerCase().startsWith("file:"))
                return ""
            videos = videos && videoPattern.test(path)
        }
        if (videos)
            return "video"
        if (urls.length !== 1)
            return ""
        const path = urls[0].toString().toLowerCase()
        if (/\.(rviz|yaml|yml)$/.test(path)) return "rviz"
        if (/\.bag$/.test(path)) return "bag"
        if (/\.(uavstudio|slamfpv)$/.test(path)) return "project"
        return ""
    }
    DropArea {
        id: fileDrop
        anchors.fill: parent
        enabled: !window.exportBusy && !replay.loading && workspace.currentIndex < 3
        z: 100
        onEntered: drag => drag.accepted = window.droppedFileKind(drag.urls).length > 0
        onDropped: drop => {
            const kind = window.droppedFileKind(drop.urls)
            if (kind === "rviz") {
                window.importRviz(drop.urls[0])
                settingsSection.currentIndex = 0
            } else if (kind === "bag") {
                window.topicInspector.openBag(drop.urls[0])
                settingsSection.currentIndex = 0
            } else if (kind === "project") {
                replay.openProject(drop.urls[0])
            } else if (kind === "video") {
                for (let i = 0; i < drop.urls.length; ++i) replay.addVideo(drop.urls[i])
                settingsSection.currentIndex = 1
            } else {
                drop.accepted = false
                return
            }
            drop.acceptProposedAction()
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 8
            radius: appearance.metrics.panelRadius
            visible: fileDrop.containsDrag
            color: appearance.colors.selection
            border.color: appearance.colors.accent
            border.width: 2
            StudioLabel {
                anchors.centerIn: parent
                text: "Drop to import · 松开以导入\nRViz · ROS Bag · Project · Video"
                font.pixelSize: appearance.metrics.title
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    TrajectoryPane { anchors.fill: parent; anchors.margins: 16; visible: workspace.currentIndex === 3; editor: trajectory }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 16; spacing: 12
        enabled: !window.exportBusy
        visible: workspace.currentIndex < 3
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12
            Rectangle {
                Layout.preferredWidth: Math.max(210, Math.min(270, window.width * .19)); Layout.fillHeight: true
                color: appearance.colors.panel; radius: appearance.metrics.panelRadius; border.color: appearance.colors.separator
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        PanelTitle { text: "Scene · 场景"; Layout.fillWidth: true }
                        StudioButton { text: "Data · 数据"; onClicked: settingsSection.currentIndex = 0 }
                    }
                    TabBar {
                        id: sidebarTabs; Layout.fillWidth: true
                        currentIndex: window.sidebarIndex
                        onCurrentIndexChanged: window.sidebarIndex = currentIndex
                        background: Rectangle { color: appearance.colors.transparent }
                        WorkspaceTab { text: "Displays\n显示" }
                        WorkspaceTab { text: "Topics\n话题" }
                        WorkspaceTab { text: "Layers\n图层" }
                        WorkspaceTab { text: "Sources\n素材" }
                    }
                    ColumnLayout {
                        visible: window.sidebarIndex === 0
                        Layout.fillWidth: true; Layout.fillHeight: true
                        StudioComboBox {
                            objectName: "displayBackendPicker"
                            Layout.fillWidth: true
                            model: ["Independent · 独立后端", "Native RViz · 原生增强"]
                            currentIndex: window.nativeBackendSelected ? 1 : 0
                            onActivated: { replay.renderBackend = currentIndex === 1 ? "native-rviz" : "independent"; if (currentIndex === 1) nativeRviz.ensureStarted() }
                        }
                        StudioLabel {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            visible: window.nativeBackendSelected && !nativeRviz.connected
                            text: nativeRviz.detecting ? "Detecting optional RViz runtime…\n正在检测可选环境，独立后端仍可使用。" : "Native enhancement unavailable. Editing with the independent backend.\n原生增强暂不可用，可继续编辑独立场景。"
                            color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption
                        }
                        IndependentDisplays {
                            id: independentTree
                            visible: !window.nativeBackendActive
                            Layout.fillWidth: true; Layout.fillHeight: true
                            controller: replay
                            showInspector: false
                            onBrowseTopicsRequested: window.sidebarIndex = 1
                            onRenameRequested: id => { settingsSection.currentIndex = 5; independentInspector.beginRename() }
                            onPropertySelected: id => { nativeInspector.treeScope = "displays"; settingsSection.currentIndex = 5 }
                        }
                        NativeRvizPanel {
                            id: displayTree
                            objectName: "nativeDisplaysTree"
                            visible: window.nativeBackendActive
                            Layout.fillWidth: true; Layout.fillHeight: true
                            client: nativeRviz
                            treeScope: "displays"
                            showConnection: false; showInspector: false; showPreview: false
                            onImportRequested: file => window.importRviz(file)
                            onExportRequested: file => window.nativeBackendActive ? nativeRviz.saveConfig(file) : replay.exportRviz(file)
                            onPropertySelected: id => { nativeInspector.treeScope = "displays"; settingsSection.currentIndex = 5 }
                        }
                    }
                    TopicBrowser {
                        objectName: "allTopicBrowser"
                        visible: window.sidebarIndex === 1
                        Layout.fillWidth: true; Layout.fillHeight: true
                        inspector: window.topicInspector
                        nativeClient: nativeRviz
                        controller: replay
                        useNativeBackend: window.nativeBackendActive
                        onTopicSelected: topic => { window.topicInspector.inspect(topic.name, topic.type); settingsSection.currentIndex = 4 }
                        onAddDisplayRequested: topic => { if (window.nativeBackendActive) displayTree.addByTopic(topic); else independentTree.addByTopic(topic); window.sidebarIndex = 0 }
                    }
                    StudioScrollView {
                        objectName: "mediaSourceScroll"
                        visible: window.sidebarIndex === 2
                        Layout.fillWidth: true; Layout.fillHeight: true
                        contentWidth: availableWidth; clip: true
                        ColumnLayout {
                            width: parent.width; spacing: 8
                            StudioButton { text: "+ ROS Bag"; Layout.fillWidth: true; enabled: !replay.loading; onClicked: bagDialog.open() }
                            StudioButton { text: "+ Video · 视频"; Layout.fillWidth: true; enabled: !replay.loading; onClicked: videoDialog.open() }
                            StudioButton { text: "+ RViz Config · 配置"; Layout.fillWidth: true; enabled: !replay.loading; onClicked: rvizDialog.open() }
                            Caption { text: replay.name; Layout.fillWidth: true; elide: Text.ElideMiddle }
                            CanvasLayers {
                                Layout.fillWidth: true
                                controller: replay
                                onLayerSelected: id => window.selectCanvasLayer(id)
                            }
                            StudioButton { text: "Clear Canvas · 清空画布"; Layout.fillWidth: true; onClicked: replay.clearCanvas() }
                            StudioButton { text: "+ Main View · 主视角"; Layout.fillWidth: true; onClicked: replay.restoreMainView() }
                            StudioButton { text: "Camera · 相机"; Layout.fillWidth: true; onClicked: { insetEditor.selectedId = ""; settingsSection.currentIndex = 2 } }
                            StudioButton { text: "Telemetry · 科研数据"; Layout.fillWidth: true; onClicked: settingsSection.currentIndex = 6 }
                            StudioButton { text: "Open Project · 打开项目"; Layout.fillWidth: true; onClicked: projectDialog.open() }
                            StudioButton { text: "Save As · 另存为"; Layout.fillWidth: true; onClicked: saveDialog.open() }
                        }
                    }
                    SourceBrowser {
                        id: sourcesPanel
                        visible: window.sidebarIndex === 3
                        Layout.fillWidth: true; Layout.fillHeight: true
                        controller: replay
                        onOpenBagRequested: bagDialog.open()
                        onAddVideoRequested: videoDialog.open()
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    PanelTitle { text: ["Viewport · 三维画布", "Synchronization · 同步预览", "Canvas · 视频画布"][workspace.currentIndex] ?? "" }
                    Item { Layout.fillWidth: true }
                    StudioButton { objectName: "layoutModeButton"; text: replay.canvas.layoutMode ? "Layout · 布局" : "Camera · 相机"; selected: replay.canvas.layoutMode; onClicked: replay.canvas.layoutMode = !replay.canvas.layoutMode; StudioToolTip { visible: parent.hovered; text: "V: Layout · 布局编辑 / C: Camera · 相机操作" } }
                    StudioCheckBox { text: "Snap"; checked: replay.canvas.snapping; onClicked: replay.canvas.snapping = checked }
                    StudioButton { text: "重置视角"; onClicked: canvas.resetCamera() }
                    StudioButton { text: "📷"; Accessible.name: "Export Frame · 导出当前画面"; onClicked: { window.frameSettingsVisible = true; settingsSection.currentIndex = 3 } StudioToolTip { visible: parent.hovered; text: "Export Frame · 导出当前画面 · Ctrl+Shift+E" } }
                }
                Flow {
                    visible: workspace.currentIndex === 0
                    Layout.fillWidth: true; spacing: 6
                    Repeater {
                        model: window.nativeBackendActive ? nativeRviz.tools.filter(tool => ["MoveCamera", "Select", "FocusCamera", "Measure"].indexOf(tool.class.split("/").pop()) >= 0) : []
                        delegate: StudioButton {
                            required property var modelData
                            text: modelData.name
                            selected: nativeRviz.activeToolClass === modelData.class
                            onClicked: { replay.canvas.layoutMode = false; nativeRviz.activateTool(modelData.class) }
                            StudioToolTip { visible: parent.hovered; text: parent.modelData.description }
                        }
                    }
                    StudioButton {
                        visible: !window.nativeBackendActive
                        text: "Move Camera · 操作相机"
                        selected: !replay.canvas.layoutMode
                        onClicked: replay.canvas.layoutMode = false
                        StudioToolTip { visible: parent.hovered; text: "Left: orbit · 左键旋转；Middle / Shift: pan · 中键平移；Right / Wheel: zoom · 右键/滚轮缩放" }
                    }
                    StudioButton { visible: !window.nativeBackendActive; text: "Focus · 重置观察"; onClicked: canvas.resetCamera() }
                    StudioComboBox {
                        visible: !window.nativeBackendActive
                        width: 190
                        model: replay.cameraLabels
                        currentIndex: replay.cameraIndex
                        onActivated: replay.setCamera(currentIndex)
                    }
                    StudioSearchComboBox {
                        visible: window.nativeBackendActive
                        width: 190
                        sourceModel: nativeRviz.viewControllers.map(view => view.class)
                        onChosen: value => { selectedText = value; nativeRviz.setViewController(value) }
                    }
                    StudioButton { visible: window.nativeBackendActive; text: "Tools · 全部工具"; onClicked: { settingsSection.currentIndex = 5; nativeInspector.treeScope = "tools" } }
                    StudioButton { visible: window.nativeBackendActive && nativeInspector.treeScope !== "displays"; text: "Displays · 显示属性"; onClicked: { settingsSection.currentIndex = 5; nativeInspector.treeScope = "displays" } }
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
                    width: Math.max(0, Math.min(parent.width - 2, (parent.height - 2) * replay.canvas.outputWidth / replay.canvas.outputHeight))
                    height: width * replay.canvas.outputHeight / replay.canvas.outputWidth
                    controller: replay
                    exporter: movie
                    nativeClient: nativeRviz
                    nativeEnabled: window.nativeBackendActive
                    nativeConfigurationPending: window.pendingNativeYaml.length > 0 || window.nativeSaveJob.length > 0
                    nativeFrameReady: !replay.nativeReplaySource.bag || nativeInspector.matchesPlayhead
                    interactive: !window.exportBusy
                    onLayerSelected: id => window.selectCanvasLayer(id)
                    onInsetSelected: id => { insetEditor.selectedId = id; settingsSection.currentIndex = 2 }
                    onSceneSelected: id => { sceneLayerEditor.selectedId = id; settingsSection.currentIndex = 1 }
                }

            }
                Caption { text: replay.worldFrame + " · " + Number(replay.pointCount).toLocaleString(Qt.locale(), "f", 0) + " points · " + (replay.demo ? "Demo · 主动演示模式" : replay.nativeReplaySource.bag ? "ROS Recording · 实验记录" : "No Data · 空场景，可预先配置"); Layout.fillWidth: true; elide: Text.ElideRight }
            }
        Rectangle {
            Layout.preferredWidth: Math.min(280, window.width * .22)
            Layout.minimumWidth: 216
            Layout.maximumWidth: 280
            Layout.fillHeight: true
            color: window.panel
            border.color: window.thin
            radius: 10

            PanelTitle {
                id: inspectorHeading
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                text: "Inspector · 属性"
                font.pixelSize: appearance.metrics.title
            }
            StudioComboBox {
                id: settingsSection
                objectName: "settingsSection"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: inspectorHeading.bottom
                anchors.margins: 12
                model: ["Data · 数据与项目", "Media · 视频与图层", "Camera · 相机", "Export · 导出", "Topic · 原始消息", "Displays · 显示属性", "Telemetry · 科研数据", "Quick Presets · 快捷预设", "Layer · 画布元素"]
                onActivated: settingsScroll.contentItem.contentY = 0
            }
            StudioScrollView {
                objectName: "workspaceSettingsScroll"
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

                    StudioLabel { Layout.fillWidth: true; visible: window.nativeSaveWaiting || window.nativeSaveJob.length > 0; text: window.nativeSaveWaiting ? "Waiting for native cameras · 正在等待原生机位同步后保存…" : "Saving native configuration · 正在保存原生配置…"; wrapMode: Text.Wrap; color: appearance.colors.secondaryText }
                    StudioLabel { Layout.fillWidth: true; visible: window.nativeSaveError.length > 0; text: window.nativeSaveError; wrapMode: Text.Wrap; color: appearance.colors.text }

                    ColumnLayout {
                        visible: settingsSection.currentIndex === 8
                        Layout.fillWidth: true
                        CanvasTransformEditor { Layout.fillWidth: true; controller: replay; canvas: canvas }
                        ScientificElementInspector { Layout.fillWidth:true;controller:replay }
                        StudioButton { Layout.fillWidth: true; text: "Content / Timing · 内容与时间"; enabled: replay.canvas.selectedId.length > 0; onClicked: window.selectCanvasLayer(replay.canvas.selectedId, true) }
                    }
                    TopicInspector {
                        visible: settingsSection.currentIndex === 4
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(580, settingsScroll.height - 24)
                        inspector: window.topicInspector
                        controller: replay
                    }

                    NativeRvizPanel {
                        id: nativeInspector
                        visible: settingsSection.currentIndex === 5 && window.nativeBackendActive
                        objectName: "nativeRvizInspector"
                        Layout.fillWidth: true
                        embedded: true
                        client: nativeRviz
                        treeScope: "displays"
                        previewExternallyManaged: window.nativeBackendActive
                        showTree: false
                        showConnection: false
                        onImportRequested: file => window.importRviz(file)
                        onExportRequested: file => window.nativeBackendActive ? nativeRviz.saveConfig(file) : replay.exportRviz(file)
                        showInspector: settingsSection.currentIndex === 5
                        showPreviewImage: false
                        showPreview: settingsSection.currentIndex === 5
                        autoAttachSource: window.nativeBackendSelected
                        startupConfig: window.nativeStartupConfig
                        experimentBag: replay.nativeReplaySource.bag || ""
                        experimentTopics: (replay.nativeReplaySource.topics || []).filter(t => t === "/tf" || t === "/tf_static")
                        experimentTimeBasis: replay.nativeReplaySource.timeBasis === "bag-storage" ? "record" : replay.nativeReplaySource.timeBasis === "header-with-storage-fallback" ? "mixed" : "header"
                        linkPausedFrame: window.nativeBackendSelected
                        experimentRevision: replay.nativeReplaySource.revision || ""
                        experimentTargetNs: replay.experimentStampNs
                        experimentPaused: !replay.playing && !replay.loading && !replay.capturing && !movie.exporting
                    }

                    IndependentDisplays {
                        id: independentInspector
                        visible: settingsSection.currentIndex === 5 && !window.nativeBackendActive
                        Layout.fillWidth: true
                        Layout.preferredHeight: 640
                        controller: replay
                        showTree: false
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
                        PanelTitle { text: "Data Sources · 数据源" }
                        StudioButton { Layout.fillWidth: true; text: "Manage Sources · 素材及引用管理"; onClicked: window.sidebarIndex = 3 }
                        StudioButton { Layout.fillWidth: true; text: "Open ROS Bag · 打开录包"; enabled: !replay.loading; highlighted: true; onClicked: bagDialog.open() }
                        Caption { Layout.fillWidth: true; text: window.topicInspector.catalogStatus || replay.name; wrapMode: Text.WrapAnywhere }
                        StudioButton { Layout.fillWidth: true; text: "Browse All Topics · 全部话题"; onClicked: window.sidebarIndex = 1 }
                        StudioButton { Layout.fillWidth: true; text: "Import .rviz · 导入原生配置"; enabled: !replay.loading && !nativeRviz.busy; onClicked: rvizDialog.open() }
                        StudioButton { Layout.fillWidth: true; text: "Export .rviz · 导出配置"; enabled: !replay.loading && !nativeRviz.busy; onClicked: rvizExportDialog.open() }
                        PanelTitle { text: "Project · 项目" }
                        StudioButton { Layout.fillWidth: true; text: "Open Project · 打开项目"; enabled: !window.exportBusy && !replay.loading; onClicked: projectDialog.open() }
                        StudioButton { Layout.fillWidth: true; text: "Save Project As · 另存为"; enabled: !window.exportBusy && !replay.loading; onClicked: saveDialog.open() }
                        StudioButton { Layout.fillWidth: true; text: "Save Project · 保存"; enabled: !window.exportBusy && !replay.loading; onClicked: {
                            if (replay.projectPath.length) window.saveProject(Qt.resolvedUrl("file:///" + replay.projectPath.replace(/\\/g, "/")), "project")
                            else saveDialog.open()
                        } }
                        StudioButton { Layout.fillWidth: true; text: "Package Project · 打包项目"; enabled: !window.exportBusy && !replay.loading; onClicked: packageDialog.open() }
                        Caption { Layout.fillWidth: true; text: replay.projectPath; visible: text.length > 0; wrapMode: Text.WrapAnywhere }
                        StudioButton { Layout.fillWidth: true; text: "Legacy Quick Presets · 旧项目预设"; onClicked: settingsSection.currentIndex = 7 }
                        Caption { Layout.fillWidth: true; text: "Displays own their Topics and properties. Speed and numeric fields are configured in Telemetry.\n每个显示对象独立选择话题及原生属性；速度与科研字段请在 Telemetry 配置。"; wrapMode: Text.Wrap }
                        StudioButton { Layout.fillWidth: true; text: "Demo · 加载模拟演示"; onClicked: replay.openDemo() }
                        StudioButton { Layout.fillWidth: true; text: (window.backendSettingsVisible ? "⌄ " : "› ") + "RViz Environment · 可选增强环境"; onClicked: window.backendSettingsVisible = !window.backendSettingsVisible }
                        NativeRvizPanel {
                            visible: window.backendSettingsVisible
                            Layout.fillWidth: true
                            embedded: true
                            client: nativeRviz
                            showTree: false; showInspector: false; showPreview: false
                            onImportRequested: file => window.importRviz(file)
                            onExportRequested: file => window.nativeBackendActive ? nativeRviz.saveConfig(file) : replay.exportRviz(file)
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: settingsSection.currentIndex === 7
                        Caption { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "Legacy research presets · 兼容科研快捷预设。仅供旧画布/轨迹及速度计算使用；不限制原生 Display 实例与话题。" }
                        PanelTitle { text: "Legacy Quick Presets · 旧项目快捷预设" }
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
                        objectName: "cameraSettingsSection"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: settingsScroll.availableWidth
                        visible: settingsSection.currentIndex === 2
                        PanelTitle { Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap; text: "Native RViz Camera · 原生相机"; visible: nativeRviz.connected }
                        NativeRvizPanel {
                            objectName: "nativeCameraInspector"
                            visible: nativeRviz.connected
                            Layout.fillWidth: true
                            embedded: true
                            client: nativeRviz
                            treeScope: "views"
                            showConnection: false
                            showPreview: false
                            showPreviewImage: false
                        }
                        Caption {
                            visible: nativeRviz.connected
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: "Native camera properties control the librviz scene. The settings below belong to the compatible point-cloud canvas.\n原生属性控制 librviz 场景；以下设置用于兼容点云画布。"
                        }
                        PanelTitle { Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap; text: "Point-cloud Canvas Camera · 点云画布相机"; Layout.topMargin: 8 }
                        Caption { Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap; text: "Scene Background · 场景背景" }
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
                        MainCameraSettings {
                            objectName: "mainCameraSettings"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.maximumWidth: parent.width
                            controller: replay
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
                                from: .25; to: 20; value: replay.mainView.pointSize ?? 1.5; stepSize: .25
                                onMoved: replay.setMainView({pointSize: value})
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
                        PanelTitle { Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap; text: window.nativeBackendActive ? "Native Views · 原生多视角" : "附加点云机位" }
                        CloudInsetEditor {
                            id: insetEditor
                            Layout.fillWidth: true
                            controller: replay
                            pointSize: canvas.pointSize
                            nativeClient: nativeRviz
                            nativeEnabled: window.nativeBackendActive
                            nativeCoordinator: canvas.nativeViewCoordinator
                        }
                        LayerMotionEditor {
                            Layout.fillWidth: true
                            controller: replay
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: settingsSection.currentIndex === 6
                        PanelTitle { text: "Telemetry · 科研数据" }
                        RowLayout {
                            Layout.fillWidth: true
                            StudioComboBox {
                                Layout.fillWidth: true
                                model: replay.speedMeters.map(row => row.label || row.id)
                                currentIndex: replay.speedMeters.findIndex(row => row.id === window.selectedMeter.id)
                                onActivated: { window.selectedMeterId = replay.speedMeters[currentIndex].id; replay.canvas.select(window.selectedMeterId) }
                            }
                            StudioButton { text: "+"; Accessible.name: "Add Speed Meter · 添加速度表"; onClicked: window.selectedMeterId = replay.addSpeedMeter() }
                        }
                        StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: window.meterExportMessage; visible: text.length > 0 }
                        SpeedMeterInspector {
                            Layout.fillWidth: true
                            visible: !!window.selectedMeter.id
                            controller: replay
                            model: speedMeterData
                            meterId: window.selectedMeter.id || ""
                            topicInspector: window.topicInspector
                            sourceMap: window.selectedMeter.source || ({})
                            settings: window.selectedMeter.settings || ({})
                            styleMap: window.selectedMeter.style || ({})
                            onSourceEdited: changes => replay.editSpeedMeter(meterId,"source",changes)
                            onSettingsEdited: changes => replay.editSpeedMeter(meterId,"settings",changes)
                            onStyleEdited: changes => replay.editSpeedMeter(meterId,"style",changes)
                            onPresetRequested: preset => {
                                const item = canvas.layerItems[meterId]
                                if (item) replay.editSpeedMeter(meterId,"style",item.presetStyle(preset))
                            }
                            onCopyStyleRequested: window.copiedMeterStyle = Object.assign({},styleMap)
                            onPasteStyleRequested: if (window.copiedMeterStyle) replay.editSpeedMeter(meterId,"style",window.copiedMeterStyle)
                            onExportElementRequested: format => { window.meterExportFormat = format; meterExportDialog.open() }
                            onExportCsvRequested: { window.meterExportFormat = "csv"; meterExportDialog.open() }
                        }
                        StudioButton { Layout.fillWidth: true; visible: !!window.selectedMeter.id; text: "Remove Meter · 删除速度表"; onClicked: { replay.canvas.select(window.selectedMeter.id); replay.canvas.removeSelected() } }
                        StudioButton { Layout.fillWidth: true; text: "Numeric Fields / Curves · 数值与曲线"; onClicked: { window.sidebarIndex = 1; settingsSection.currentIndex = 4 } }
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
                                objectName: "movieRatePicker"
                                Layout.fillWidth: true
                                model: window.movieRateLabels
                                currentIndex: Math.max(0, window.movieRateValues.indexOf(replay.moviePreferences.speed))
                                onActivated: replay.setMoviePreferences(window.movieRateValues[currentIndex], movieAudio.currentIndex - 1)
                            }
                            StudioLabel { text: "音轨" }
                            StudioComboBox {
                                id: movieAudio
                                Layout.fillWidth: true
                                model: ["无音轨"].concat(replay.videoLayers.map(layer => layer.label))
                                currentIndex: Math.max(0, Math.min(count - 1, replay.moviePreferences.audioIndex + 1))
                                onActivated: replay.setMoviePreferences(replay.moviePreferences.speed, currentIndex - 1)
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
                        id: videoTimeline
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
                            settingsSection.currentIndex = 1
                        }
                        onLayerSelected: (id) => { sceneLayerEditor.selectedId = id; settingsSection.currentIndex = 1 }
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
                        StudioComboBox {
                            objectName: "playbackRatePicker"
                            Layout.preferredWidth: 80
                            model: window.movieRateLabels
                            currentIndex: Math.max(0, window.movieRateValues.indexOf(replay.moviePreferences.speed))
                            enabled: !window.exportBusy && !replay.loading
                            Accessible.name: "Playback speed · 播放倍率"
                            onActivated: replay.setMoviePreferences(window.movieRateValues[currentIndex], replay.moviePreferences.audioIndex)
                            StudioToolTip { visible: parent.hovered; text: "Playback and export speed · 播放及导出倍率；不改变物理速度值" }
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
                        StudioButton { text: "Undo · 撤销"; visible: window.timelineMode < 2 && replay.videoLayers.length > 0; enabled: !window.exportBusy && !replay.loading && replay.canUndo; onClicked: replay.undo() }
                        StudioButton { text: "Redo · 重做"; visible: window.timelineMode < 2 && replay.videoLayers.length > 0; enabled: !window.exportBusy && !replay.loading && replay.canRedo; onClicked: replay.redo() }
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
        visible: movie.exporting || frameBatch.active
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
