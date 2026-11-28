import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

StudioComboBox {
    id: control
    property var sourceModel: []
    property string selectedText: ""
    property string query: ""
    signal chosen(string value)
    function chooseCurrent() {
        if (choices.currentIndex >= 0 && choices.currentIndex < control.model.length) {
            chosen(control.model[choices.currentIndex])
            popup.close()
        }
    }
    model: sourceModel.filter(value => value.toLowerCase().indexOf(query.toLowerCase()) >= 0)
    currentIndex: model.indexOf(selectedText)
    displayText: selectedText.length ? selectedText : "Select… · 请选择"
    onActivated: chosen(currentText)
    onQueryChanged: choices.currentIndex = control.model.length > 0 ? 0 : -1
    popup: Popup {
        y: control.height + 4
        x: Math.min(0, control.width - width)
        width: Math.max(control.width, 260)
        margins: 8
        padding: 8
        palette: StudioPalette {}
        implicitHeight: popupContents.implicitHeight + 16
        onOpened: {
            control.query = ""
            search.text = ""
            choices.currentIndex = Math.max(0, control.model.indexOf(control.selectedText))
            choices.positionViewAtIndex(choices.currentIndex, ListView.Contain)
            search.forceActiveFocus()
        }
        onClosed: control.query = ""
        background: Rectangle { color: appearance.colors.panel; radius: appearance.metrics.radius; border.color: appearance.colors.border }
        contentItem: ColumnLayout {
            id: popupContents
            spacing: 6
            StudioTextField {
                id: search
                objectName: "searchField"
                Layout.fillWidth: true
                placeholderText: "Search · 搜索 Topic / Frame"
                onTextEdited: control.query = text
                onAccepted: control.chooseCurrent()
                Keys.onDownPressed: choices.currentIndex = Math.min(choices.count - 1, choices.currentIndex + 1)
                Keys.onUpPressed: choices.currentIndex = Math.max(0, choices.currentIndex - 1)
                Keys.onEscapePressed: control.popup.close()
            }
            ListView {
                id: choices
                objectName: "searchChoices"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 300)
                model: control.popup.visible ? control.model : []
                currentIndex: 0
                clip: true
                onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                delegate: ItemDelegate {
                    id: option
                    required property string modelData
                    required property int index
                    width: choices.width
                    height: appearance.metrics.controlHeight
                    highlighted: index === choices.currentIndex
                    Accessible.name: modelData
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
