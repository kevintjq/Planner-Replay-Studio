import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// All instances share one NativeRvizClient. No sample properties or local copy.
Item {
    id: root
    readonly property bool ownsDeleteKey: true
    required property var client
    // A view of the shared native Property model, never an independent scene.
    property string treeScope: "all" // all | displays | views | tools
    property string viewIdFilter: "" // Empty shows every native camera.
    property bool previewExternallyManaged: false
    // Embedded Inspectors share their parent's scrolling. Growing native help
    // text must grow the panel, so the value editor never sits in a second
    // clipped viewport. Standalone sidebars keep their own bounded viewport.
    property bool embedded: false
    readonly property bool managesDisplays: treeScope === "all" || treeScope === "displays"
    readonly property bool viewsOnly: treeScope === "views"
    readonly property var scopedProperties: {
        const rows = []
        let scope = ""
        for (const row of client.properties) {
            if (row.depth === 0) scope = nativeRootScope(row)
            if ((treeScope === "all" || scope === treeScope)
                && (!viewIdFilter.length || scope === "views" && (row.view_id || "main") === viewIdFilter)) rows.push(row)
        }
        return rows
    }
    property bool showConnection: true
    property bool showTree: true
    property bool showInspector: true
    property bool showPreview: true
    property bool showPreviewImage: true
    property bool advancedVisible: false
    signal importRequested(url file)
    signal exportRequested(url file)
    property bool autoAttachSource: false
    property url startupConfig: ""
    property bool addingDisplay: false
    property string attachedBag: ""
    property string attachingBag: ""
    property bool indexAfterAttach: false
    signal propertySelected(string id)
    property string experimentTargetNs: ""
    property var experimentTopics: []
    property string experimentTimeBasis: "header"
    property url experimentBag: ""
    property string experimentRevision: ""
    property bool experimentPaused: true
    property bool linkPausedFrame: false
    property string indexedSignature: ""
    property string pendingSignature: ""
    readonly property var indexTopics: {
        const topics = []
        for (const source of [experimentTopics, client.displayTopics || []])
            for (const topic of source)
                if (topic.length && topics.indexOf(topic) < 0) topics.push(topic)
        return topics.sort()
    }
    readonly property string experimentSignature: experimentBag.toString() + "|" + experimentRevision + "|" + experimentTimeBasis + "|" + JSON.stringify(indexTopics)
    readonly property bool experimentIndexed: client.indexed && indexedSignature.length > 0 && indexedSignature === experimentSignature
    readonly property bool matchesPlayhead: experimentIndexed && experimentPaused && experimentTargetNs.length > 0
        && !client.busy && client.synchronized && client.sceneReady
        && client.targetNs === experimentTargetNs && client.clockNs === experimentTargetNs && client.previewTargetNs === experimentTargetNs
    property var displaySelection: []
    property string syncError: ""
    property string selectedPlugin: ""
    property string selectedTopic: ""
    property string editingId: ""
    property string editError: ""
    readonly property var selection: {
        const selected = client.selected
        if (treeScope === "all" && !viewIdFilter.length) return selected
        if (selected && selected.id)
            for (const row of client.properties)
                if (row.depth === 0 && (treeScope === "all" || nativeRootScope(row) === treeScope)
                    && (!viewIdFilter.length || (row.view_id || "main") === viewIdFilter) && containsNativeId(row, selected.id))
                    return selected
        return ({})
    }
    readonly property string topicType: {
        const entry = client.topics.find(t => t.name === selectedTopic)
        return entry ? entry.type : ""
    }
    readonly property var pluginChoices: topicMode.currentIndex === 1
        ? (topicType.length ? client.compatiblePlugins(topicType) : []) : client.plugins
    implicitWidth: 360
    implicitHeight: embedded ? contentLayout.implicitHeight : 640

    function nativeRootScope(node) {
        // The client exposes three actual roots. Names are not identifiers:
        // ViewControllerContainer and ToolManager can have an empty root name.
        if (node.native_type === "rviz::DisplayGroup") return "displays"
        if (node.view_id || node.native_type === "rviz::ViewControllerContainer") return "views"
        return "tools"
    }
    function containsNativeId(node, id) {
        if (node.id === id) return true
        for (const child of node.children || [])
            if (containsNativeId(child, id)) return true
        return false
    }
    function scopeHeading(scope) {
        if (scope === "displays") return "Displays · 原生显示"
        if (scope === "views") return "Views · 原生相机视角"
        if (scope === "tools") return "Tools · 原生工具"
        return "Displays / Views / Tools · 原生属性"
    }
    function refreshSelectionEditor() {
        if (!propertyEditor || !displayName) return
        const id = root.selection.id || ""
        if (root.editingId !== id) {
            root.editingId = id
            propertyEditor.focus = false
            root.editError = ""
            displayName.text = root.selection.name || ""
        }
        if (!propertyEditor.activeFocus) propertyEditor.text = root.formattedValue(root.selection.value)
    }
    onSelectionChanged: refreshSelectionEditor()

    function addByTopic(topic) {
        if (!managesDisplays) return
        addingDisplay = true
        topicMode.currentIndex = 1
        selectedTopic = topic.name
        selectedPlugin = ""
    }
    function ensureSource() {
        const bag = experimentBag.toString()
        if (!client.connected || client.busy || !bag.length) return false
        if (attachedBag === bag) return true
        if (!attachingBag.length) {
            attachingBag = bag
            client.openBag(experimentBag)
        }
        return false
    }

    // Bag inventory is independent from Display subscriptions. Opening a source
    // never clears the native scene or rebuilds its Display instances.
    Timer {
        interval: 300
        running: root.autoAttachSource && !root.viewsOnly && root.client.connected
        repeat: true
        onTriggered: {
            if (root.client.busy) return
            if (!root.experimentBag.toString().length && root.attachedBag.length) { root.attachedBag = ""; root.client.closeBag(); return }
            root.ensureSource()
            if (!root.experimentIndexed && !root.pendingSignature.length && root.attachedBag.length) root.indexExperiment()
        }
    }

    function indexExperiment() {
        if (!client.connected || client.busy || !experimentBag.toString().length || !indexTopics.length) return
        if (experimentTimeBasis !== "header" && experimentTimeBasis !== "record") { syncError = "Mixed Header / storage timestamps need an explicit mapping; native synchronization is unavailable.\n混合时间基准尚无明确映射，原生同步未执行。"; return }
        syncError = ""
        if (autoAttachSource && !ensureSource()) { indexAfterAttach = true; return }
        indexedSignature = ""
        pendingSignature = experimentSignature
        client.indexBag(experimentBag, experimentTimeBasis, "reject", {topics: indexTopics})
    }
    function syncPausedFrame() {
        if (experimentIndexed && experimentPaused && experimentTargetNs.length)
            client.seekAbsolute(experimentTargetNs)
    }
    onExperimentSignatureChanged: {
        pausedSync.stop()
        indexedSignature = ""
        if (pendingSignature.length && client.busy) client.cancelReplay()
        pendingSignature = ""
    }
    onExperimentTargetNsChanged: {
        if (linkPausedFrame && experimentIndexed && experimentPaused) pausedSync.restart()
    }
    onExperimentPausedChanged: {
        if (!experimentPaused) client.cancelPendingSeek()
        if (linkPausedFrame && experimentIndexed && experimentPaused) pausedSync.restart()
        else pausedSync.stop()
    }
    onLinkPausedFrameChanged: {
        if (linkPausedFrame && experimentIndexed && experimentPaused) pausedSync.restart()
        else pausedSync.stop()
    }
    Timer { id: pausedSync; interval: 200; onTriggered: root.syncPausedFrame() }
    Timer {
        interval: 250; repeat: true
        running: root.autoAttachSource && root.linkPausedFrame && root.experimentIndexed && !root.experimentPaused
        onTriggered: if (root.experimentTargetNs.length && !root.client.busy) root.client.advanceAbsolute(root.experimentTargetNs)
    }

    function formattedValue(value) {
        if (Array.isArray(value)) return JSON.stringify(value)
        return value === undefined || value === null ? "" : String(value)
    }
    function applyValue() {
        const item = root.selection
        let value = propertyEditor.text
        try {
            if (Array.isArray(item.value)) {
                value = JSON.parse(value)
                if (!Array.isArray(value) || value.length !== item.value.length
                    || value.some(v => typeof v !== "number" || !Number.isFinite(v)))
                    throw new Error("Use finite [x,y,z] or [x,y,z,w] · 请输入有限向量")
            } else if (["int", "float", "double"].indexOf(item.value_type) >= 0) {
                if (!value.trim().length) throw new Error("Enter a number · 请输入数值")
                value = Number(value)
                if (!Number.isFinite(value) || (item.value_type === "int" && !Number.isInteger(value)))
                    throw new Error("Invalid native number · 数值格式错误")
            }
            root.editError = ""
            client.setPropertyValue(item.id, value)
        } catch (error) { root.editError = String(error.message) }
    }

    Connections {
        target: root.client
        function onCommandFinished(command, success, error) {
            if (command === "bag.open" && root.attachingBag.length) {
                root.attachedBag = success ? root.attachingBag : ""
                root.attachingBag = ""
                const index = root.indexAfterAttach
                root.indexAfterAttach = false
                if (index && success) root.indexExperiment()
            }
            if (command === "bag.close") { root.attachedBag = ""; root.attachingBag = ""; root.indexedSignature = ""; root.pendingSignature = "" }
            if (success && ["load", "load_yaml"].indexOf(command) >= 0) { root.indexedSignature = ""; root.pendingSignature = "" }
            if (success && ["set", "remove", "remove_many", "duplicate", "add", "move", "undo", "redo"].indexOf(command) >= 0 && root.linkPausedFrame && root.experimentIndexed && root.experimentPaused) pausedSync.restart()
            if (command === "bag.index" && root.pendingSignature.length) {
                root.indexedSignature = success && root.pendingSignature === root.experimentSignature ? root.pendingSignature : ""
                root.pendingSignature = ""
                if (root.linkPausedFrame && root.experimentIndexed && root.experimentPaused) pausedSync.restart()
            }
        }
        function onModelChanged() {
            root.refreshSelectionEditor()
        }
        function onChanged() { if (!root.client.connected) { root.attachedBag = ""; root.attachingBag = ""; root.indexedSignature = ""; root.pendingSignature = "" } }
    }
    Timer {
        interval: 300; running: !root.previewExternallyManaged && !root.viewsOnly && (root.autoAttachSource && root.client.connected || root.visible && root.showPreview && root.client.connected); repeat: true
        onTriggered: root.client.requestPreview()
    }

    StudioScrollView {
        id: panelScroll
        objectName: "nativePanelScroll"
        visible: !root.embedded
        anchors.fill: parent
        clip: true
        wheelEnabled: !root.embedded
        contentWidth: availableWidth
        contentHeight: contentLayout.implicitHeight
        ScrollBar.vertical.policy: root.embedded ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
        ScrollBar.horizontal.policy: root.embedded ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
    }
    // Keep one content tree. In embedded mode it belongs directly to the
    // panel, leaving the outer Inspector as its only scrollable ancestor.
    ColumnLayout {
        id: contentLayout
        parent: root.embedded ? root : panelScroll.contentItem.contentItem
        width: root.embedded ? root.width : panelScroll.availableWidth
        spacing: 10
        ColumnLayout {
            objectName: "nativeConnection"
            visible: root.showConnection && !root.viewsOnly
            Layout.fillWidth: true
            spacing: 6
            StudioLabel { Layout.fillWidth: true; Layout.minimumWidth: 0; text: "Optional RViz Backend · 可选原生增强"; font.bold: true; wrapMode: Text.Wrap }
            StudioLabel { Layout.fillWidth: true; text: root.syncError.length ? root.syncError : root.client.status; wrapMode: Text.Wrap; textFormat: Text.PlainText }
            StudioLabel {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                visible: !root.client.connected
                text: "The independent backend remains available. Native plugin support is enabled when a compatible runtime is detected.\n独立后端可正常使用；检测到兼容环境后启用原生插件能力。"
                font.pixelSize: appearance.metrics.caption; color: appearance.colors.secondaryText
            }
            StudioButton {
                Layout.fillWidth: true
                visible: !root.client.connected && !root.client.detecting && !root.client.running
                text: "Check Environment Again · 重新检测环境"
                onClicked: root.client.retryConnection()
            }
            StudioButton { Layout.fillWidth: true; text: (root.advancedVisible ? "⌄ " : "› ") + "Advanced · 高级后端设置"; onClicked: root.advancedVisible = !root.advancedVisible }
            StudioTextField {
                visible: root.advancedVisible
                Layout.fillWidth: true; Layout.minimumWidth: 0; enabled: !root.client.running
                text: root.client.launcher; placeholderText: "ROS launcher · ROS 启动脚本"
                onEditingFinished: root.client.launcher = text
            }
            ColumnLayout {
                visible: root.advancedVisible
                Layout.fillWidth: true
                StudioTextField {
                    objectName: "nativeDistribution"
                    Layout.fillWidth: true; Layout.minimumWidth: 0; enabled: !root.client.running
                    text: root.client.distribution; placeholderText: "WSL distro · ROS 发行版"
                    onEditingFinished: root.client.distribution = text
                }
                RowLayout {
                    Layout.fillWidth: true
                    StudioButton { objectName: "nativeLauncherBrowse"; Layout.fillWidth: true; Layout.minimumWidth: 0; text: "Launcher · 脚本…"; enabled: !root.client.running; onClicked: launcherDialog.open(); Accessible.name: "Select ROS launcher · 选择 ROS 启动脚本" }
                    StudioButton {
                        objectName: "nativeConnect"
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: root.client.running ? "Stop · 停止" : "Start · 连接"
                        onClicked: root.client.running ? root.client.stop() : root.client.start(root.client.launcher, root.client.distribution, root.startupConfig)
                    }
                }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: root.width < 340 ? 1 : 2
                StudioButton { objectName: "nativeImport"; Layout.fillWidth: true; Layout.minimumWidth: 0; text: "Import .rviz · 导入"; onClicked: configDialog.open() }
                StudioButton { objectName: "nativeSave"; Layout.fillWidth: true; Layout.minimumWidth: 0; text: "Save .rviz · 保存"; onClicked: saveDialog.open() }
            }
            StudioButton { visible: root.advancedVisible; text: diagnosticLabel.visible ? "Hide diagnostics · 收起诊断" : "Diagnostics · 诊断日志"; onClicked: diagnosticLabel.visible = !diagnosticLabel.visible }
            StudioLabel {
                id: diagnosticLabel; visible: false; Layout.fillWidth: true
                text: root.client.diagnostic; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
                color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption
            }
        }

        ColumnLayout {
            visible: root.showTree
            Layout.fillWidth: true
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                StudioLabel { objectName: "nativeTreeHeading"; Layout.fillWidth: true; Layout.minimumWidth: 0; text: root.scopeHeading(root.treeScope); font.bold: true; wrapMode: Text.Wrap }
                StudioButton { Layout.preferredWidth: 30; Layout.minimumWidth: 0; implicitWidth: 30; text: "↻"; enabled: root.client.connected; onClicked: root.client.refresh(); Accessible.name: "Refresh native properties · 刷新原生属性" }
            }
            StudioTextField { Layout.fillWidth: true; placeholderText: "Search properties · 搜索属性"; text: root.client.search; onTextEdited: root.client.search = text }
            ListView {
                id: tree
                objectName: "nativePropertyTree"
                Layout.fillWidth: true; Layout.preferredHeight: 330
                model: root.scopedProperties; clip: true
                Keys.onDeletePressed: if (root.managesDisplays) root.client.removeDisplays(root.displaySelection)
                ScrollBar.vertical: StudioScrollBar {}
                delegate: Rectangle {
                    id: treeRow
                    required property var modelData
                    width: tree.width; height: appearance.metrics.controlHeight + 4
                    radius: appearance.metrics.radius
                    color: root.client.selectedId === modelData.id ? appearance.colors.selection : hover.hovered ? appearance.colors.hover : "transparent"
                    HoverHandler { id: hover }
                    MouseArea {
                        anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: mouse => {
                            tree.forceActiveFocus()
                            root.client.select(treeRow.modelData.id)
                            root.propertySelected(treeRow.modelData.id)
                            if (root.managesDisplays && treeRow.modelData.plugin_class && treeRow.modelData.depth > 0) {
                                let ids = mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier) ? root.displaySelection.slice() : []
                                if (ids.indexOf(treeRow.modelData.id) < 0) ids.push(treeRow.modelData.id)
                                root.displaySelection = ids
                                if (mouse.button === Qt.RightButton) nativeActions.popup()
                            } else root.displaySelection = []
                        }
                    }
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 6 + treeRow.modelData.depth * 13; anchors.rightMargin: 6
                        spacing: 3
                        StudioButton {
                            implicitWidth: 22; Layout.preferredWidth: 22; horizontalPadding: 0
                            text: treeRow.modelData.children.length ? (treeRow.modelData.expanded ? "⌄" : "›") : ""
                            enabled: treeRow.modelData.children.length > 0
                            onClicked: root.client.toggleExpanded(treeRow.modelData.id)
                            Accessible.name: "Expand " + treeRow.modelData.name
                        }
                        StudioCheckBox {
                            visible: treeRow.modelData.checkable && treeRow.modelData.value_type === "bool"
                            checked: treeRow.modelData.value === true
                            enabled: treeRow.modelData.enabled && !treeRow.modelData.read_only
                            onClicked: root.client.setPropertyValue(treeRow.modelData.id, checked)
                            Accessible.name: treeRow.modelData.name
                        }
                        StudioLabel { Layout.fillWidth: true; Layout.minimumWidth: 0; text: treeRow.modelData.name || (treeRow.modelData.depth === 0 ? root.scopeHeading(root.nativeRootScope(treeRow.modelData)) : ""); elide: Text.ElideRight; opacity: treeRow.modelData.enabled ? 1 : .6 }
                        StudioLabel {
                            Layout.maximumWidth: tree.width * .42
                            Layout.minimumWidth: 0
                            text: treeRow.modelData.status ? treeRow.modelData.status + ": " + treeRow.modelData.display_value : treeRow.modelData.display_value
                            elide: Text.ElideRight; color: appearance.colors.secondaryText
                        }
                    }
                    StudioToolTip { visible: hover.hovered; text: treeRow.modelData.description || treeRow.modelData.name }
                }
            }
            Menu {
                id: nativeActions; palette: StudioPalette {}
                MenuItem { text: "Duplicate · 复制显示"; enabled: root.managesDisplays; onTriggered: root.client.displayAction("duplicate", root.client.selectedId) }
                MenuItem { text: "Remove Selected · 删除选中显示"; enabled: root.managesDisplays; onTriggered: root.client.removeDisplays(root.displaySelection) }
            }
            GridLayout {
                visible: root.managesDisplays
                Layout.fillWidth: true
                columns: root.width < 340 ? 1 : 3
                StudioButton { Layout.fillWidth: true; Layout.minimumWidth: 0; text: "Undo"; enabled: root.client.canUndo && !root.client.busy; onClicked: root.client.undoNative() }
                StudioButton { Layout.fillWidth: true; Layout.minimumWidth: 0; text: "Redo"; enabled: root.client.canRedo && !root.client.busy; onClicked: root.client.redoNative() }
                StudioButton { Layout.fillWidth: true; Layout.minimumWidth: 0; text: "Remove · 删除"; enabled: root.displaySelection.length > 0 && !root.client.busy; onClicked: root.client.removeDisplays(root.displaySelection) }
            }
            StudioButton {
                objectName: "nativeAddToggle"
                visible: root.managesDisplays
                Layout.fillWidth: true
                text: root.addingDisplay ? "Close Add · 收起添加" : "+ Add Display · 添加显示"
                enabled: root.client.connected && !root.client.busy
                onClicked: root.addingDisplay = !root.addingDisplay
            }
            ColumnLayout {
            visible: root.managesDisplays && root.addingDisplay
            Layout.fillWidth: true
            StudioComboBox { id: topicMode; Layout.fillWidth: true; model: ["By Type · 按类型添加", "By Topic · 按话题添加"]; enabled: root.client.connected }
            StudioSearchComboBox {
                visible: topicMode.currentIndex === 1
                Layout.fillWidth: true; enabled: root.client.connected
                sourceModel: root.client.topics.map(t => t.name); selectedText: root.selectedTopic
                onChosen: value => { root.selectedTopic = value; root.selectedPlugin = "" }
            }
            StudioTextField {
                visible: root.addingDisplay
                Layout.fillWidth: true; text: root.selectedTopic
                placeholderText: "Topic · 话题（可预先手动输入）"
                onTextEdited: root.selectedTopic = text.trim()
            }
            StudioLabel { visible: topicMode.currentIndex === 1; Layout.fillWidth: true; text: root.topicType; color: appearance.colors.secondaryText; elide: Text.ElideMiddle }
            StudioSearchComboBox {
                Layout.fillWidth: true; enabled: root.client.connected
                sourceModel: root.pluginChoices.map(p => p.class); selectedText: root.selectedPlugin
                onChosen: value => root.selectedPlugin = value
            }
            RowLayout {
                Layout.fillWidth: true
                StudioButton { text: "+ Group · 显示分组"; onClicked: root.client.addDisplay("rviz/Group", newName.text.trim() || "Group") }
                StudioTextField { id: newName; Layout.fillWidth: true; placeholderText: "Display name · 显示名称" }
                StudioButton {
                    text: "Add · 添加"; enabled: root.client.connected && root.selectedPlugin.length > 0 && root.pluginChoices.some(p => p.class === root.selectedPlugin)
                    onClicked: root.client.addDisplay(root.selectedPlugin, newName.text.trim() || root.selectedPlugin.split("/").pop(), root.selectedTopic)
                }
            }
            StudioLabel {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: topicMode.currentIndex === 1 && root.topicType.length && !root.pluginChoices.length
                    ? "No plugin declares this message type. Inspect its raw fields in Topics.\n该类型没有声明兼容的显示插件，仍可在话题浏览器检查原始数据。"
                    : "Classes and properties are discovered from this RViz installation.\n插件及条件属性来自当前原生 RViz。"
                color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption
            }
            }
        }

        ColumnLayout {
            visible: root.showInspector
            Layout.fillWidth: true
            spacing: 6
            StudioLabel { text: "Inspector · 原生属性检查器"; font.bold: true }
            StudioLabel { Layout.fillWidth: true; text: root.selection.name || "Select a native property · 选择原生属性"; wrapMode: Text.Wrap }
            StudioLabel { Layout.fillWidth: true; text: root.selection.native_type || ""; color: appearance.colors.secondaryText; wrapMode: Text.WrapAnywhere; font.pixelSize: appearance.metrics.caption }
            StudioLabel { Layout.fillWidth: true; text: root.selection.description || ""; wrapMode: Text.Wrap; textFormat: Text.RichText }
            StudioLabel { visible: !!root.selection.status; Layout.fillWidth: true; text: (root.selection.status || "") + " · " + (root.selection.display_value || ""); wrapMode: Text.Wrap }
            StudioLabel {
                visible: root.selection.minimum !== undefined
                Layout.fillWidth: true; text: "Range · 范围: " + root.selection.minimum + " … " + root.selection.maximum
                font.pixelSize: appearance.metrics.caption; color: appearance.colors.secondaryText; wrapMode: Text.Wrap
            }
            StudioCheckBox {
                visible: root.selection.value_type === "bool"
                text: "Enabled / Value · 启用 / 属性值"; checked: root.selection.value === true
                enabled: root.client.connected && !!root.selection.enabled && !root.selection.read_only
                onClicked: root.client.setPropertyValue(root.selection.id, checked)
            }
            StudioSearchComboBox {
                visible: !!root.selection.options_available
                Layout.fillWidth: true; sourceModel: root.selection.options || []; selectedText: String(root.selection.value || "")
                enabled: root.client.connected && !!root.selection.editable && !!root.selection.enabled && !root.selection.read_only
                onChosen: value => root.client.setPropertyValue(root.selection.id, value)
            }
            GridLayout {
                visible: root.selection.value_type !== "bool" && (!root.selection.options_available || root.selection.allow_custom)
                Layout.fillWidth: true
                columns: root.width < 360 ? 1 : 2
                StudioTextField {
                    id: propertyEditor; objectName: "nativePropertyEditor"; Layout.fillWidth: true; Layout.minimumWidth: 0
                    enabled: root.client.connected && !!root.selection.editable && !!root.selection.enabled && !root.selection.read_only
                    placeholderText: root.selection.native_type && root.selection.native_type.endsWith("ColorProperty") ? "#RRGGBB" : "Native value · 原生属性值"
                    onAccepted: root.applyValue()
                }
                StudioButton { objectName: "nativePropertyApply"; Layout.fillWidth: parent.columns === 1; Layout.minimumWidth: 0; text: "Apply · 应用"; enabled: propertyEditor.enabled; onClicked: root.applyValue() }
            }
            StudioLabel { visible: root.editError.length > 0; Layout.fillWidth: true; text: root.editError; wrapMode: Text.Wrap }
            StudioLabel { visible: !!root.selection.id && (!root.selection.enabled || root.selection.read_only); text: "Read only / disabled by native RViz · 原生只读或禁用"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: appearance.colors.secondaryText }
            StudioTextField { id: displayName; visible: root.managesDisplays && !!root.selection.plugin_class && root.selection.depth > 0; Layout.fillWidth: true; placeholderText: "Display name · 显示名称" }
            Flow {
                visible: root.managesDisplays && !!root.selection.plugin_class && root.selection.depth > 0
                Layout.fillWidth: true; spacing: 5
                StudioButton { text: "Rename · 重命名"; onClicked: root.client.displayAction("rename", root.selection.id, displayName.text) }
                StudioButton { text: "Duplicate · 复制"; onClicked: root.client.displayAction("duplicate", root.selection.id) }
                StudioButton { text: "Remove · 移除"; onClicked: root.client.displayAction("remove", root.selection.id) }
                StudioButton { text: "Clear runtime data · 清运行数据"; onClicked: root.client.displayAction("reset", root.selection.id) }
            }
        }

        ColumnLayout {
            objectName: "nativePreview"
            visible: root.showPreview && !root.viewsOnly
            Layout.fillWidth: true
            spacing: 6
            StudioLabel { text: "Native preview · 原生预览"; font.bold: true }
            StudioLabel {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: !root.experimentBag.toString().length ? "No Data · 无数据。场景与原生属性可编辑，添加数据后恢复显示。" : root.matchesPlayhead
                    ? "Paused playhead matched · 已恢复当前 Header 历史，必需 TF 及原生 Display 状态就绪。未认证第三方异步工作或高分辨率输出。"
                    : "Native frame not ready · 原生画面尚未通过当前暂停帧的时间、TF 和 Display 就绪检查；连续播放未同步。此预览不用于高分辨率输出。"
                color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption
            }
            Rectangle {
                visible: root.showPreviewImage
                Layout.fillWidth: true; Layout.preferredHeight: width * 9 / 16
                color: appearance.colors.input; radius: appearance.metrics.radius
                Image { anchors.fill: parent; source: root.client.preview; fillMode: Image.PreserveAspectFit; cache: false }
                StudioLabel { anchors.centerIn: parent; visible: !root.client.preview.length; text: "Waiting for native frame · 等待原生画面" }
            }
            StudioLabel { Layout.fillWidth: true; text: "ROS ns: " + (root.client.clockNs || "—"); elide: Text.ElideMiddle; font.pixelSize: appearance.metrics.caption }
            Flow {
                visible: root.advancedVisible
                Layout.fillWidth: true; spacing: 5
                StudioButton { text: "Open bag · 原生录包"; enabled: root.client.connected && !root.client.busy; onClicked: bagDialog.open() }
                StudioButton { text: "Play from start · 从头播放"; enabled: root.client.connected && !root.client.busy; onClicked: root.client.playBag(1) }
                StudioButton { text: "Stop bag · 停止录包"; enabled: root.client.connected && !root.client.busy; onClicked: root.client.stopBag() }
                StudioButton { objectName: "indexExperimentTime"; visible: root.experimentBag.toString().length > 0 && root.indexTopics.length > 0; text: "Index Header time · 索引实验时间"; enabled: root.client.connected && !root.client.busy; onClicked: root.indexExperiment() }
                StudioButton { objectName: "syncPausedFrame"; visible: root.experimentTargetNs.length > 0; text: "Sync paused frame · 同步暂停帧"; enabled: root.experimentIndexed && root.experimentPaused; onClicked: root.syncPausedFrame() }
                StudioButton { visible: root.client.busy; text: "Cancel replay · 取消恢复"; onClicked: root.client.cancelReplay() }
            }
            StudioCheckBox {
                objectName: "linkPausedFrame"
                visible: root.advancedVisible && root.experimentBag.toString().length > 0
                text: "Follow paused playhead · 跟随暂停播放头"
                enabled: root.experimentIndexed
                checked: root.linkPausedFrame
                onToggled: root.linkPausedFrame = checked
            }
            StudioLabel {
                visible: root.advancedVisible && root.experimentBag.toString().length > 0
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                text: "Header index topics · 实验索引话题（含启用的原生 Display）：" + root.indexTopics.join(", ")
                    + "\n零/缺失 Header 会明确拒绝；不把录包写入时间冒充实验时间。随机跳转需恢复历史，非实时播放。"
                color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption
            }
        }
    }
    Binding {
        target: panelScroll.contentItem
        property: "interactive"
        value: false
        when: root.embedded
    }
    FileDialog { id: launcherDialog; title: "ROS launcher · ROS 启动脚本"; nameFilters: ["Shell scripts (*.sh)", "All files (*)"]; onAccepted: root.client.launcher = selectedFile.toString() }
    FileDialog { id: configDialog; title: "Import native RViz · 导入原生配置"; nameFilters: ["RViz (*.rviz)"]; onAccepted: root.importRequested(selectedFile) }
    FileDialog { id: saveDialog; title: "Save native RViz · 保存原生配置"; fileMode: FileDialog.SaveFile; defaultSuffix: "rviz"; nameFilters: ["RViz (*.rviz)"]; onAccepted: root.exportRequested(selectedFile) }
    FileDialog { id: bagDialog; title: "Open native ROS bag · 打开原生录包"; nameFilters: ["ROS bag (*.bag)"]; onAccepted: root.client.openBag(selectedFile) }
}
