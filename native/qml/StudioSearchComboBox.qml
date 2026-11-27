import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

StudioComboBox {
    id: control
    property var sourceModel: []
    property string selectedText: ""
    property string query: ""
    signal chosen(string value)
    model: sourceModel.filter(value => value.toLowerCase().indexOf(query.toLowerCase()) >= 0)
    currentIndex: model.indexOf(selectedText)
    displayText: selectedText.length ? selectedText : "Select… · 请选择"
    onActivated: chosen(currentText)
    popup: Popup {
        y: control.height + 4
        width: Math.max(control.width, 260)
        padding: 8
        palette: StudioPalette {}
        implicitHeight: popupContents.implicitHeight + 16
        onOpened: { control.query = ""; search.text = ""; search.forceActiveFocus() }
        onClosed: control.query = ""
        background: Rectangle { color: appearance.colors.panel; radius: appearance.metrics.radius; border.color: appearance.colors.border }
        contentItem: ColumnLayout {
            id: popupContents
            spacing: 6
            StudioTextField {
                id: search
                Layout.fillWidth: true
                placeholderText: "Search · 搜索 Topic / Frame"
                onTextEdited: control.query = text
                onAccepted: {
                    if (control.model.length > 0) { control.chosen(control.model[Math.max(0, choices.currentIndex)]); control.popup.close() }
                }
                Keys.onDownPressed: choices.currentIndex = Math.min(choices.count - 1, choices.currentIndex + 1)
                Keys.onUpPressed: choices.currentIndex = Math.max(0, choices.currentIndex - 1)
            }
            ListView {
                id: choices
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 300)
                model: control.popup.visible ? control.model : []
                currentIndex: 0
                clip: true
                delegate: ItemDelegate {
                    id: option
                    required property string modelData
                    required property int index
                    width: choices.width
                    height: appearance.metrics.controlHeight
                    highlighted: index === choices.currentIndex
                    onHoveredChanged: if (hovered) choices.currentIndex = index
                    onClicked: { control.chosen(modelData); control.popup.close() }
                    contentItem: Text { text: option.modelData; color: appearance.colors.text; font.pixelSize: appearance.metrics.body; verticalAlignment: Text.AlignVCenter; elide: Text.ElideMiddle }
                    background: Rectangle { radius: appearance.metrics.radius; color: option.highlighted ? appearance.colors.selection : appearance.colors.panel }
                    StudioToolTip { visible: option.hovered; text: option.modelData }
                }
                ScrollBar.vertical: StudioScrollBar {}
            }
            StudioLabel { visible: choices.count === 0; text: "No matches · 无匹配项"; color: appearance.colors.secondaryText }
        }
    }
}
