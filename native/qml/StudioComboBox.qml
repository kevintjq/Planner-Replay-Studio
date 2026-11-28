import QtQuick
import QtQuick.Controls as C

C.ComboBox {
    id: control
    palette: StudioPalette {}
    implicitWidth: Math.max(100, contentItem.implicitWidth + 38)
    implicitHeight: appearance.metrics.controlHeight
    leftPadding: 10
    rightPadding: 28
    font.pixelSize: appearance.metrics.body
    contentItem: Text {
        text: control.displayText
        font: control.font
        color: control.enabled ? appearance.colors.text : appearance.colors.disabledText
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Text {
        x: control.width - width - 10
        y: (control.height - height) / 2
        text: "⌄"; font.pixelSize: 16; color: appearance.colors.secondaryText
    }
    delegate: C.ItemDelegate {
        id: option
        required property int index
        width: control.popup.width - 8
        height: appearance.metrics.controlHeight
        text: control.textAt(index)
        highlighted: control.highlightedIndex === index
        contentItem: Text { text: option.text; font: control.font; color: appearance.colors.text; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { radius: appearance.metrics.radius; color: option.highlighted ? appearance.colors.selection : option.hovered ? appearance.colors.hover : appearance.colors.panel }
        StudioToolTip { visible: option.hovered && option.text.length > 30; text: option.text }
    }
    popup: C.Popup {
        y: control.height + 4
        x: Math.min(0, control.width - width)
        width: Math.max(control.width, 220)
        margins: 8
        padding: 4
        palette: StudioPalette {}
        implicitHeight: contentItem.implicitHeight + 8
        contentItem: ListView {
            implicitHeight: Math.min(contentHeight, 360)
            clip: true
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            C.ScrollBar.vertical: StudioScrollBar {}
        }
        background: Rectangle { color: appearance.colors.panel; radius: appearance.metrics.radius; border.color: appearance.colors.border }
    }
    background: Rectangle {
        radius: appearance.metrics.radius
        color: control.down ? appearance.colors.pressed : control.hovered ? appearance.colors.hover : appearance.colors.control
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? appearance.colors.accent : appearance.colors.border
        opacity: control.enabled ? 1 : .5
    }
}
