import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel
    readonly property bool ownsDeleteKey: true
    required property var controller
    property bool showTree: true
    property bool showInspector: true
    signal propertySelected(string id)
    signal browseTopicsRequested()
    signal renameRequested(string id)
    readonly property var selection: controller.selectedIndependentDisplay || ({})
    readonly property var rows: controller.independentDisplays
    property bool adding: false
    property var pendingTopic: ({})
    property string topicClass: ""
    property bool globalOptionsOpen: false
    property bool globalStatusOpen: false
    property var collapsedGroups: ({})
    property var selectedIds: []
    property string selectionAnchor: ""
    readonly property var visibleRows: {
        const result = [], visited = {}, children = {}
        for (const row of rows) {
            const parent = row.parentId || ""
            if (!children[parent]) children[parent] = []
            children[parent].push(row)
        }
        function append(parent) {
            for (const row of children[parent] || []) {
                if (visited[row.id]) continue
                visited[row.id] = true
                result.push(row)
                if (!panel.collapsedGroups[row.id]) append(row.id)
            }
        }
        append("")
        return result
    }
    readonly property int warningCount: rows.filter(row => ["Unsupported", "Type Mismatch", "Decode Error", "Missing TF", "Topic Missing", "No Decoded Data"].includes(row.status)).length
    readonly property bool hasDataSource: !!panel.controller.nativeReplaySource.bag
    spacing: 7

    onRowsChanged: selectedIds = selectedIds.filter(id => rows.some(row => row.id === id))
    Connections {
        target: panel.controller
        function onIndependentDisplaySelectionChanged() {
            const id = panel.controller.selectedIndependentDisplayId
            if (!id.length) panel.selectedIds = []
            else if (!panel.selectedIds.includes(id)) panel.selectedIds = [id]
        }
    }
    function beginRename() { Qt.callLater(() => { nameEditor.forceActiveFocus(); nameEditor.selectAll() }) }
    function select(id, modifiers) {
        let next = [id]
        if (modifiers & Qt.ShiftModifier) {
            const a = visibleRows.findIndex(row => row.id === selectionAnchor)
            const b = visibleRows.findIndex(row => row.id === id)
            if (a >= 0 && b >= 0) next = visibleRows.slice(Math.min(a, b), Math.max(a, b) + 1).map(row => row.id)
        } else if (modifiers & Qt.ControlModifier) {
            next = selectedIds.includes(id) ? selectedIds.filter(value => value !== id) : selectedIds.concat([id])
        }
        if (!(modifiers & Qt.ShiftModifier)) selectionAnchor = id
        selectedIds = next
        controller.selectIndependentDisplay(next.includes(id) ? id : (next.length ? next[next.length - 1] : ""))
        propertySelected(controller.selectedIndependentDisplayId)
    }
    function removeSelection() {
        const ids = selectedIds.length ? selectedIds : selection.id ? [selection.id] : []
        controller.removeIndependentDisplays(ids)
        selectedIds = []
    }
    function toggleGroup(id) {
        const next = Object.assign({}, collapsedGroups)
        next[id] = !next[id]
        collapsedGroups = next
    }

    function addByTopic(topic) {
        pendingTopic = topic || ({})
        const matches = controller.compatibleIndependentDisplays(pendingTopic.type || "")
        if (matches.length === 1) {
            const id = controller.addIndependentDisplay(matches[0].class, pendingTopic.name || "")
            if (id.length) propertySelected(id)
        } else {
            topicClass = matches.length ? matches[0].class : ""
            adding = true
        }
    }
    function edit(key, value) {
        if (!selection.id) return
        const patch = {}; patch[key] = value
        controller.editIndependentDisplay(selection.id, patch)
    }
    function depth(row) {
        let parent = row.parentId || "", count = 0
        const seen = {}
        while (parent.length && count < 24 && !seen[parent]) {
            seen[parent] = true; ++count
            const item = rows.find(value => value.id === parent)
            parent = item ? item.parentId || "" : ""
        }
        return count
    }

    StudioScrollView {
        objectName: "independentDisplaysScroll"
        visible: panel.showTree
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
        width: parent.width
        spacing: 7
        StudioLabel { text: "Displays · 独立显示"; font.bold: true }
        StudioLabel {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            color: appearance.colors.secondaryText
            text: "Standalone C++ / Qt · 独立渲染子集"
            font.pixelSize: appearance.metrics.caption
        }
        StudioButton {
            Layout.fillWidth: true; objectName: "independentGlobalOptions"
            text: (panel.globalOptionsOpen ? "⌄ " : "› ") + "Global Options · 全局选项"
            onClicked: panel.globalOptionsOpen = !panel.globalOptionsOpen
        }
        ColumnLayout {
            visible: panel.globalOptionsOpen; Layout.fillWidth: true
            StudioLabel { text: "Fixed Frame · 固定坐标系" }
            StudioSearchComboBox {
                Layout.fillWidth: true
                sourceModel: panel.controller.tfFrames
                selectedText: panel.controller.independentFixedFrame
                onChosen: value => panel.controller.setFixedFrame(value)
            }
            StudioTextField {
                objectName: "independentFixedFrame"; Layout.fillWidth: true
                text: panel.controller.independentFixedFrame
                placeholderText: "map · 无数据可提前配置"
                onEditingFinished: panel.controller.setFixedFrame(text)
            }
            StudioLabel { text: "Background Color · 场景背景" }
            StudioTextField {
                Layout.fillWidth: true; text: panel.controller.sceneBackground.toString()
                placeholderText: "#RRGGBB"
                onEditingFinished: panel.controller.sceneBackground = text
            }
        }
        StudioButton {
            Layout.fillWidth: true; objectName: "independentGlobalStatus"
            text: (panel.globalStatusOpen ? "⌄ " : "› ") + "Global Status · " + (panel.controller.independentDisplayLoading ? "Loading" : panel.warningCount ? panel.warningCount + " issue(s)" : panel.hasDataSource ? "Configured" : "No Data")
            onClicked: panel.globalStatusOpen = !panel.globalStatusOpen
        }
        StudioLabel {
            visible: panel.globalStatusOpen; Layout.fillWidth: true; wrapMode: Text.Wrap
            color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption
            text: panel.rows.length + " Displays · " + panel.warningCount + " issues\n"
                + (panel.hasDataSource ? "逐项检查 Display 状态；消息解码不代表原生插件兼容通过。" : "Waiting for Data · 可先配置属性，再导入数据。")
        }
        RowLayout {
            Layout.fillWidth: true
            StudioButton { Layout.fillWidth: true; text: panel.adding ? "Close · 收起" : "+ Add · 添加"; onClicked: { panel.pendingTopic = ({}); panel.adding = !panel.adding } }
            StudioButton { text: "Retry · 重试"; enabled: !panel.controller.independentDisplayLoading; onClicked: panel.controller.retryIndependentDisplays() }
        }
        ColumnLayout {
            visible: panel.adding; Layout.fillWidth: true
            RowLayout {
                Layout.fillWidth: true
                StudioButton { Layout.fillWidth: true; text: "By Type · 类型"; onClicked: panel.pendingTopic = ({}) }
                StudioButton { Layout.fillWidth: true; text: "By Topic · 话题"; onClicked: panel.browseTopicsRequested() }
            }
            StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: panel.pendingTopic.type ? "By Topic · " + panel.pendingTopic.name : "By Display Type · 按显示类型"; font.bold: true }
            StudioComboBox {
                id: typePicker; Layout.fillWidth: true
                model: panel.pendingTopic.type ? panel.controller.compatibleIndependentDisplays(panel.pendingTopic.type) : panel.controller.independentDisplayTypes
                textRole: "name"
            }
            StudioLabel { visible: typePicker.model.length === 0; Layout.fillWidth: true; wrapMode: Text.Wrap; color: appearance.colors.secondaryText; text: "No standalone geometry decoder for this message type. The Topic remains available in the browser.\n该消息没有独立几何解码器；仍可在 Topic Browser 查看数据。" }
            StudioTextField {
                id: initialTopic; Layout.fillWidth: true
                placeholderText: "/topic · 可稍后指定 Topic"
                text: panel.pendingTopic.name || ""
            }
            StudioComboBox {
                id: parentPicker; Layout.fillWidth: true
                readonly property var groups: [{ id: "", name: "Root · 根节点" }].concat(panel.rows.filter(value => value.kind === "group"))
                model: groups; textRole: "name"
            }
            StudioButton {
                Layout.fillWidth: true; text: "Create Display · 创建实例"
                enabled: typePicker.currentIndex >= 0
                onClicked: {
                    const type = typePicker.model[typePicker.currentIndex]
                    const group = parentPicker.groups[parentPicker.currentIndex]
                    const id = panel.controller.addIndependentDisplay(type.class, initialTopic.text, group ? group.id : "")
                    if (id.length) { panel.adding = false; panel.propertySelected(id) }
                }
            }
        }
        ListView {
            id: tree; objectName: "independentDisplayTree"; Layout.fillWidth: true
            Layout.preferredHeight: Math.max(100, Math.min(360, contentHeight))
            clip: true; boundsBehavior: Flickable.StopAtBounds
            model: panel.visibleRows
            keyNavigationEnabled: true
            Keys.onDeletePressed: panel.removeSelection()
            ScrollBar.vertical: StudioScrollBar { }
            delegate: Rectangle {
                id: displayRow
                required property var modelData
                width: tree.width; height: 64; radius: 5
                color: panel.selectedIds.includes(modelData.id) || panel.controller.selectedIndependentDisplayId === modelData.id ? appearance.colors.selection : appearance.colors.transparent
                RowLayout {
                    anchors.fill: parent; anchors.margins: 3
                    Item { Layout.preferredWidth: Math.min(64, 12 * panel.depth(displayRow.modelData)) }
                    StudioButton {
                        visible: displayRow.modelData.kind === "group"; implicitWidth: 22
                        text: panel.collapsedGroups[displayRow.modelData.id] ? "›" : "⌄"
                        Accessible.name: "Expand Group · 展开分组"
                        onClicked: panel.toggleGroup(displayRow.modelData.id)
                    }
                    StudioCheckBox {
                        checked: displayRow.modelData.enabled
                        Accessible.name: "Display enabled · 显示启用"
                        onToggled: panel.controller.editIndependentDisplay(displayRow.modelData.id, { enabled: checked })
                    }
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: displayLabels.implicitHeight
                        ColumnLayout {
                            id: displayLabels
                            anchors.fill: parent; spacing: 1
                            StudioLabel { Layout.fillWidth: true; text: displayRow.modelData.name; elide: Text.ElideMiddle; font.bold: panel.controller.selectedIndependentDisplayId === displayRow.modelData.id }
                            StudioLabel { Layout.fillWidth: true; text: displayRow.modelData.class; elide: Text.ElideMiddle; color: appearance.colors.secondaryText; font.pixelSize: 10 }
                            StudioLabel { Layout.fillWidth: true; text: displayRow.modelData.status; elide: Text.ElideRight; color: appearance.colors.secondaryText; font.pixelSize: 10 }
                        }
                        MouseArea {
                            anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: mouse => {
                                tree.forceActiveFocus()
                                if (mouse.button !== Qt.RightButton || !panel.selectedIds.includes(displayRow.modelData.id)) panel.select(displayRow.modelData.id, mouse.modifiers)
                                if (mouse.button === Qt.RightButton) displayMenu.popup()
                            }
                            onDoubleClicked: if (displayRow.modelData.kind === "group") panel.toggleGroup(displayRow.modelData.id)
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true; enabled: !!panel.selection.id
            StudioButton { Layout.fillWidth: true; text: "Copy · 复制"; onClicked: { const id = panel.controller.duplicateIndependentDisplay(panel.selection.id); if (id.length) panel.propertySelected(id) } }
            StudioButton { Layout.fillWidth: true; text: "Remove · 删除"; onClicked: panel.removeSelection() }
        }
        }
    }

    Menu {
        id: displayMenu; palette: StudioPalette {}
        MenuItem { text: "Rename · 重命名"; onTriggered: panel.renameRequested(panel.selection.id) }
        MenuItem { text: "Duplicate · 复制"; onTriggered: { const id = panel.controller.duplicateIndependentDisplay(panel.selection.id); if (id.length) panel.propertySelected(id) } }
        MenuItem { text: "Add Group · 添加分组"; onTriggered: panel.controller.addIndependentDisplay("rviz/Group") }
        MenuItem { text: "Remove · 删除选中显示"; onTriggered: panel.removeSelection() }
    }

    ColumnLayout {
        visible: panel.showInspector; Layout.fillWidth: true
        StudioLabel { text: "Display Inspector · 显示属性"; font.bold: true }
        StudioLabel {
            visible: !panel.selection.id; Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "Select a Display in the left tree.\n在左侧选择显示实例。"; color: appearance.colors.secondaryText
        }
        ColumnLayout {
            visible: !!panel.selection.id; Layout.fillWidth: true
            StudioTextField { id: nameEditor; Layout.fillWidth: true; text: panel.selection.name || ""; placeholderText: "Name · 名称"; onEditingFinished: panel.edit("name", text) }
            StudioLabel { Layout.fillWidth: true; text: panel.selection.class || ""; wrapMode: Text.Wrap; color: appearance.colors.secondaryText }
            StudioCheckBox { text: "Enabled · 启用"; checked: panel.selection.enabled ?? true; onToggled: panel.edit("enabled", checked) }
            StudioLabel { Layout.fillWidth: true; text: (panel.selection.status || "") + "\n" + (panel.selection.statusText || ""); wrapMode: Text.Wrap; color: appearance.colors.secondaryText }
            StudioLabel { visible: ["cloud","laser","path","marker","position"].indexOf(panel.selection.kind) >= 0; text: "Topic · 数据话题" }
            StudioSearchComboBox {
                Layout.fillWidth: true
                visible: ["cloud","laser","path","marker","position"].indexOf(panel.selection.kind) >= 0
                sourceModel: panel.controller.nativeReplaySource.topics || []
                selectedText: panel.selection.topic || ""
                onChosen: value => panel.edit("topic", value)
            }
            StudioTextField {
                Layout.fillWidth: true
                visible: ["cloud","laser","path","marker","position"].indexOf(panel.selection.kind) >= 0
                text: panel.selection.topic || ""; placeholderText: "/topic"
                onEditingFinished: panel.edit("topic", text)
            }
            StudioLabel { visible: panel.selection.kind !== "group" && !!panel.selection.supported; text: "Color · 颜色 #RRGGBB" }
            StudioTextField { Layout.fillWidth: true; visible: panel.selection.kind !== "group" && !!panel.selection.supported; text: panel.selection.color || "#64c9ee"; onEditingFinished: panel.edit("color", text) }
            StudioCheckBox { visible: panel.selection.kind === "marker"; text: "Use Message Color · 使用消息颜色（若存在）"; checked: panel.selection.useMessageColor ?? true; onToggled: panel.edit("useMessageColor", checked) }
            StudioLabel { visible: panel.selection.kind === "cloud"; Layout.fillWidth: true; wrapMode: Text.Wrap; color: appearance.colors.secondaryText; text: "Standalone: uniform color. Native RGB/intensity transformers require the RViz backend.\n独立后端使用统一颜色；原生 RGB / 强度颜色转换需 RViz 后端。"; font.pixelSize: appearance.metrics.caption }
            StudioLabel { visible: panel.selection.kind !== "group" && !!panel.selection.supported; text: "Alpha · 不透明度" }
            StudioSlider { Layout.fillWidth: true; visible: panel.selection.kind !== "group" && !!panel.selection.supported; from: 0; to: 1; value: panel.selection.alpha ?? 1; onMoved: panel.edit("alpha", value) }
            StudioLabel { visible: ["cloud","laser","position","marker"].indexOf(panel.selection.kind) >= 0; text: "Point Size · 点大小（像素）" }
            StudioTextField { Layout.fillWidth: true; visible: ["cloud","laser","position","marker"].indexOf(panel.selection.kind) >= 0; text: Number(panel.selection.pointSize ?? 3).toFixed(2); validator: DoubleValidator { bottom: .1; top: 512; locale: "C" } onEditingFinished: if (acceptableInput) panel.edit("pointSize", Number(text)) }
            StudioLabel { visible: ["path","marker","tf","grid","axes"].indexOf(panel.selection.kind) >= 0; text: "Line Width · 线宽（像素）" }
            StudioTextField { Layout.fillWidth: true; visible: ["path","marker","tf","grid","axes"].indexOf(panel.selection.kind) >= 0; text: Number(panel.selection.lineWidth ?? 2).toFixed(2); validator: DoubleValidator { bottom: .1; top: 512; locale: "C" } onEditingFinished: if (acceptableInput) panel.edit("lineWidth", Number(text)) }
            StudioLabel { visible: ["path","marker","tf","grid","axes"].indexOf(panel.selection.kind) >= 0; Layout.fillWidth: true; wrapMode: Text.Wrap; color: appearance.colors.secondaryText; text: "Effective line width is limited by this GPU's OpenGL support.\n实际线宽受当前 GPU 的 OpenGL 线宽上限限制。"; font.pixelSize: appearance.metrics.caption }
            StudioLabel { visible: ["cloud","laser","position"].indexOf(panel.selection.kind) >= 0; text: "History · 历史秒数（0=最新消息）" }
            StudioTextField { Layout.fillWidth: true; visible: ["cloud","laser","position"].indexOf(panel.selection.kind) >= 0; text: Number(panel.selection.historySeconds ?? 0).toFixed(3); validator: DoubleValidator { bottom: 0; top: 86400; locale: "C" } onEditingFinished: if (acceptableInput) panel.edit("historySeconds", Number(text)) }
            StudioLabel { visible: panel.selection.kind === "grid"; text: "Cell Size · 网格尺寸（米）" }
            StudioTextField { Layout.fillWidth: true; visible: panel.selection.kind === "grid"; text: Number(panel.selection.properties?.["Cell Size"] ?? 1).toString(); onEditingFinished: panel.edit("Cell Size", Number(text)) }
            StudioLabel { visible: panel.selection.kind === "grid"; text: "Plane Cell Count · 网格格数" }
            StudioTextField { Layout.fillWidth: true; visible: panel.selection.kind === "grid"; text: Number(panel.selection.properties?.["Plane Cell Count"] ?? 20).toString(); onEditingFinished: panel.edit("Plane Cell Count", Number(text)) }
            StudioLabel { visible: panel.selection.kind === "axes"; text: "Length · 坐标轴长度（米）" }
            StudioTextField { Layout.fillWidth: true; visible: panel.selection.kind === "axes"; text: Number(panel.selection.properties?.Length ?? 1).toString(); onEditingFinished: panel.edit("Length", Number(text)) }
            StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: panel.selection.compatibility || ""; color: appearance.colors.secondaryText }
            StudioButton { Layout.fillWidth: true; text: "Remove Display · 删除显示"; onClicked: panel.controller.removeIndependentDisplays([panel.selection.id]) }
            StudioCheckBox { id: preserved; text: "Original Properties · 查看保留的原始属性" }
            Repeater {
                model: preserved.checked ? panel.controller.independentDisplayProperties(panel.selection.id || "") : []
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: modelData.name; color: appearance.colors.secondaryText; font.pixelSize: 10 }
                    StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: modelData.value; font.pixelSize: 11 }
                }
            }
        }
    }
}
