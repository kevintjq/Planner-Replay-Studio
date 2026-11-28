import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ColumnLayout {
    id: browser
    readonly property bool ownsDeleteKey: true
    required property var controller
    property var selectedIds: []
    readonly property var current: controller.sources.find(s => s.id === selectedIds[selectedIds.length - 1]) || null
    readonly property var rows: controller.sources.filter(s => (category.currentIndex === 0 || s.kind === ["", "bag", "video", "image", "audio", "calibration"][category.currentIndex])
        && (s.name + " " + s.path).toLowerCase().indexOf(search.text.toLowerCase()) >= 0)
    property string replacingId: ""
    signal openBagRequested()
    signal addVideoRequested()
    spacing: 7
    function select(id, additive) {
        let ids = additive ? selectedIds.slice() : []
        const index = ids.indexOf(id)
        if (index >= 0) ids.splice(index, 1); else ids.push(id)
        selectedIds = ids
    }
    FileDialog {
        id: importDialog; fileMode: FileDialog.OpenFiles
        title: "Import Sources · 导入素材（原始文件只读）"
        nameFilters: ["Media (*.mp4 *.mov *.mkv *.avi *.webm *.png *.jpg *.jpeg *.tiff *.svg *.wav *.mp3 *.flac)", "All files (*)"]
        onAccepted: { for (const file of selectedFiles) {
            const name = file.toString().toLowerCase()
            controller.addSource(file, /\.(wav|mp3|flac|aac)$/.test(name) ? "audio" : /\.(png|jpg|jpeg|tiff|svg|bmp)$/.test(name) ? "image" : "video")
        }
        }
    }
    FileDialog {
        id: relinkDialog; title: "Replace / Relink Source · 替换或重新链接素材"
        onAccepted: controller.replaceSource(browser.replacingId, selectedFile)
    }
    StudioLabel { text: "Sources · 项目素材"; font.bold: true }
    RowLayout {
        Layout.fillWidth: true
        StudioButton { text: "+ Bag"; onClicked: browser.openBagRequested() }
        StudioButton { text: "+ Video"; onClicked: browser.addVideoRequested() }
        StudioButton { text: "+"; Accessible.name: "Import Sources · 导入素材"; onClicked: importDialog.open() }
    }
    StudioTextField { id: search; objectName: "sourceBrowserSearch"; Layout.fillWidth: true; placeholderText: "Search Sources · 搜索素材" }
    StudioComboBox { id: category; Layout.fillWidth: true; model: ["All · 全部", "ROS Bag", "Video · 视频", "Image · 图片", "Audio · 音频", "Calibration · 标定"] }
    ListView {
        id: sourceList; objectName: "sourceBrowserList"; Layout.fillWidth: true; Layout.fillHeight: true
        Layout.minimumHeight: 100; clip: true; model: browser.rows
        ScrollBar.vertical: StudioScrollBar {}
        delegate: Rectangle {
            id: row; required property var modelData
            width: sourceList.width; height: 56; radius: appearance.metrics.radius
            color: browser.selectedIds.indexOf(modelData.id) >= 0 ? appearance.colors.selection : appearance.colors.transparent
            Column {
                anchors.fill: parent; anchors.margins: 6; spacing: 3
                StudioLabel { width: parent.width; text: row.modelData.name; elide: Text.ElideMiddle }
                StudioLabel { width: parent.width; text: row.modelData.kind + " · " + row.modelData.referenceCount + " references 引用" + (row.modelData.exists ? "" : " · Missing 丢失"); font.pixelSize: appearance.metrics.caption; color: row.modelData.exists ? appearance.colors.secondaryText : appearance.colors.error; elide: Text.ElideRight }
            }
            MouseArea {
                anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: mouse => {
                    sourceList.forceActiveFocus()
                    if (mouse.button !== Qt.RightButton || browser.selectedIds.indexOf(row.modelData.id) < 0)
                        browser.select(row.modelData.id, !!(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)))
                    if (mouse.button === Qt.RightButton) sourceMenu.popup()
                }
                onDoubleClicked: controller.addSourceLayer(row.modelData.id)
            }
        }
        Keys.onDeletePressed: controller.removeSources(browser.selectedIds)
    }
    Menu {
        id: sourceMenu; palette: StudioPalette {}
        MenuItem { text: "Add Instance · 添加图层"; enabled: !!browser.current; onTriggered: controller.addSourceLayer(browser.current.id) }
        MenuItem { text: "Replace / Relink · 替换或重新链接"; enabled: !!browser.current; onTriggered: { browser.replacingId = browser.current.id; relinkDialog.open() } }
        MenuItem { text: "Remove Source · 移除素材"; enabled: browser.selectedIds.length > 0; onTriggered: controller.removeSources(browser.selectedIds) }
    }
    ColumnLayout {
        visible: !!browser.current; Layout.fillWidth: true
        StudioTextField { Layout.fillWidth: true; text: browser.current?.name || ""; Accessible.name: "Source Name · 素材名称"; onEditingFinished: if (browser.current) controller.renameSource(browser.current.id, text) }
        StudioLabel { Layout.fillWidth: true; text: browser.current?.path || ""; wrapMode: Text.WrapAnywhere; color: appearance.colors.secondaryText; font.pixelSize: appearance.metrics.caption }
        Repeater { model: browser.current?.references || []; StudioLabel { required property var modelData; Layout.fillWidth: true; text: "↳ " + modelData.name + " · " + modelData.kind; wrapMode: Text.WrapAnywhere; font.pixelSize: appearance.metrics.caption } }
    }
    RowLayout {
        Layout.fillWidth: true
        StudioButton { text: "Add · 添加"; enabled: !!browser.current; onClicked: controller.addSourceLayer(browser.current.id) }
        StudioButton { objectName: "sourceBrowserRemove"; text: "Remove · 移除"; enabled: browser.selectedIds.length > 0; onClicked: controller.removeSources(browser.selectedIds) }
    }
    StudioButton { Layout.fillWidth: true; text: "Clear Unused · 清理未使用素材"; onClicked: controller.clearUnusedSources() }
    ColumnLayout {
        visible: (controller.pendingSourceRemoval.ids || []).length > 0; Layout.fillWidth: true
        StudioLabel { Layout.fillWidth: true; text: "Remove these references too?\n以下引用将一并移除，可撤销。"; wrapMode: Text.Wrap }
        Repeater { model: controller.pendingSourceRemoval.references || []; StudioLabel { required property var modelData; Layout.fillWidth: true; text: "• " + modelData.name + " · " + modelData.kind; wrapMode: Text.WrapAnywhere } }
        RowLayout {
            StudioButton { text: "Cancel · 取消"; onClicked: controller.cancelSourceRemoval() }
            StudioButton { text: "Remove · 移除"; onClicked: { controller.removeSources(controller.pendingSourceRemoval.ids, true); browser.selectedIds = [] } }
        }
    }
    StudioLabel { Layout.fillWidth: true; text: "Project references only. Original files stay intact.\n仅移除项目引用，保留磁盘原始文件。"; color: appearance.colors.secondaryText; wrapMode: Text.Wrap; font.pixelSize: appearance.metrics.caption }
}
