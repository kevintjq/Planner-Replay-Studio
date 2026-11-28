import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: browser
    required property var inspector
    required property var nativeClient
    property var controller: null
    property bool useNativeBackend: true
    readonly property var compatibleDisplays: !selectedTopic ? [] : useNativeBackend
        ? nativeClient.compatiblePlugins(selectedTopic.type) : controller ? controller.compatibleIndependentDisplays(selectedTopic.type) : []
    property string query: ""
    property string typeFilter: ""
    property string sourceFilter: ""
    property string selectedKey: ""
    property var expanded: ({"/": true})
    signal topicSelected(var topic)
    signal addDisplayRequested(var topic)
    spacing: 7

    readonly property var entries: {
        const rows = [], seen = {}
        for (const item of (inspector.catalog || [])) {
            const row = Object.assign({}, item)
            row.key = row.source + "|" + row.name + "|" + row.type
            seen[row.name + "|" + row.type] = true
            rows.push(row)
        }
        for (const item of (nativeClient.topics || [])) {
            if (seen[item.name + "|" + item.type]) continue
            const row = Object.assign({}, item)
            row.source = item.source || "ROS · 实时"
            row.key = row.source + "|" + row.name + "|" + row.type
            rows.push(row)
        }
        return rows.sort((a, b) => a.name.localeCompare(b.name) || a.type.localeCompare(b.type))
    }
    readonly property var types: [...new Set(entries.map(t => t.type))].sort()
    readonly property var sources: [...new Set(entries.map(t => t.source))].sort()
    readonly property var selectedTopic: entries.find(t => t.key === selectedKey) || null
    readonly property var filtered: entries.filter(t => (!typeFilter.length || t.type === typeFilter)
        && (!sourceFilter.length || t.source === sourceFilter)
        && (!query.length || (t.name + " " + t.type).toLowerCase().includes(query.toLowerCase())))
    readonly property var rows: {
        const tree = {children: {}, topics: []}, output = []
        for (const topic of filtered) {
            let node = tree, path = ""
            const parts = topic.name.split("/").filter(p => p.length)
            for (let i = 0; i < parts.length - 1; ++i) {
                path += "/" + parts[i]
                if (!node.children[path]) node.children[path] = {name: parts[i], path: path, children: {}, topics: []}
                node = node.children[path]
            }
            node.topics.push(topic)
        }
        function append(node, depth) {
            for (const key of Object.keys(node.children).sort()) {
                const child = node.children[key]
                const open = query.length > 0 || expanded[key] === true
                output.push({folder: true, name: child.name, path: key, depth: depth, open: open})
                if (open) append(child, depth + 1)
            }
            for (const topic of node.topics) output.push({folder: false, name: topic.name.split("/").pop(), topic: topic, depth: depth})
        }
        append(tree, 0)
        return output
    }

    StudioTextField {
        id: topicSearch
        objectName: "topicBrowserSearch"
        Layout.fillWidth: true
        placeholderText: "Search topics / types · 搜索"
        onTextEdited: searchDelay.restart()
        Timer { id: searchDelay; interval: 140; onTriggered: browser.query = topicSearch.text.trim() }
    }
    StudioSearchComboBox {
        objectName: "topicBrowserType"
        Layout.fillWidth: true
        sourceModel: ["All message types · 全部类型"].concat(browser.types)
        selectedText: browser.typeFilter.length ? browser.typeFilter : sourceModel[0]
        onChosen: value => browser.typeFilter = value === sourceModel[0] ? "" : value
    }
    StudioSearchComboBox {
        objectName: "topicBrowserSource"
        Layout.fillWidth: true
        visible: browser.sources.length > 1
        sourceModel: ["All sources · 全部来源"].concat(browser.sources)
        selectedText: browser.sourceFilter.length ? browser.sourceFilter : sourceModel[0]
        onChosen: value => browser.sourceFilter = value === sourceModel[0] ? "" : value
    }
    RowLayout {
        Layout.fillWidth: true
        StudioLabel { Layout.fillWidth: true; text: browser.filtered.length + " / " + browser.entries.length + " Topics"; color: appearance.colors.secondaryText }
        StudioButton {
            text: "↕"; implicitWidth: 30
            Accessible.name: "Expand all namespaces · 展开命名空间"
            onClicked: {
                const all = {}
                for (const topic of browser.filtered) {
                    const parts = topic.name.split("/").filter(p => p.length)
                    let path = ""
                    for (let i = 0; i < parts.length - 1; ++i) { path += "/" + parts[i]; all[path] = true }
                }
                browser.expanded = Object.keys(browser.expanded).length > 1 ? {"/": true} : all
            }
        }
    }
    ListView {
        id: topicList
        objectName: "topicBrowserList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 160
        model: browser.rows
        clip: true
        reuseItems: true
        ScrollBar.vertical: StudioScrollBar {}
        delegate: ItemDelegate {
            id: entry
            required property var modelData
            width: topicList.width
            height: modelData.folder ? appearance.metrics.controlHeight : appearance.metrics.controlHeight + 21
            leftPadding: 5 + Math.min(modelData.depth, 8) * 10
            rightPadding: 5
            highlighted: !modelData.folder && modelData.topic.key === browser.selectedKey
            Accessible.name: modelData.folder ? modelData.path : modelData.topic.name + " " + modelData.topic.type
            onClicked: {
                if (modelData.folder) {
                    const next = Object.assign({}, browser.expanded)
                    next[modelData.path] = !modelData.open
                    browser.expanded = next
                } else {
                    browser.selectedKey = modelData.topic.key
                    browser.topicSelected(modelData.topic)
                }
            }
            contentItem: Column {
                spacing: 2
                StudioLabel { width: parent.width; text: entry.modelData.folder ? (entry.modelData.open ? "⌄ " : "› ") + entry.modelData.name : entry.modelData.name; elide: Text.ElideRight; font.weight: entry.modelData.folder ? Font.DemiBold : Font.Normal }
                StudioLabel { width: parent.width; visible: !entry.modelData.folder; text: entry.modelData.topic?.type || ""; elide: Text.ElideMiddle; color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption }
            }
            background: Rectangle { radius: appearance.metrics.radius; color: entry.highlighted ? appearance.colors.selection : entry.hovered ? appearance.colors.hover : appearance.colors.transparent }
            StudioToolTip { visible: entry.hovered; text: entry.modelData.folder ? entry.modelData.path : entry.modelData.topic.name + "\n" + entry.modelData.topic.type }
        }
        StudioLabel { anchors.centerIn: parent; width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; visible: topicList.count === 0; color: appearance.colors.secondaryText; text: browser.inspector.catalogWorking ? "Indexing all topics…\n正在索引全部话题…" : "Open a Bag or connect ROS\n打开录包或连接 ROS" }
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: !!browser.selectedTopic
        StudioLabel { Layout.fillWidth: true; text: browser.selectedTopic?.name || ""; wrapMode: Text.WrapAnywhere; font.bold: true }
        StudioLabel {
            Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: appearance.colors.secondaryText
            text: {
                const row = browser.selectedTopic
                if (!row) return ""
                return row.type + "\n" + (row.count === undefined ? "Live discovery · 实时发现" : row.count + " messages · " + Number(row.frequencyHz || 0).toFixed(2) + " Hz")
                    + (row.firstStampNs ? "\nBag storage ns · 写入时间\n" + row.firstStampNs + " … " + row.lastStampNs : "")
                    + (row.definitionAvailable === false ? "\nMessage definition missing · 缺少消息定义" : "")
            }
            font.pixelSize: appearance.metrics.caption
        }
        StudioButton {
            Layout.fillWidth: true
            text: "Add Display · 添加显示"
            enabled: browser.compatibleDisplays.length > 0
            onClicked: browser.addDisplayRequested(browser.selectedTopic)
        }
        StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; visible: !!browser.selectedTopic && browser.compatibleDisplays.length === 0; text: "No compatible Display in this backend. Raw fields remain available.\n当前后端无兼容显示；仍可检查原始字段。"; font.pixelSize: appearance.metrics.caption; color: appearance.colors.secondaryText }
    }
    StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: browser.inspector.catalogStatus || ""; color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption }
}
