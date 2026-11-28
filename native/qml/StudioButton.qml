import QtQuick
import QtQuick.Controls as C

C.Button {
    id: control
    property bool selected: false
    palette: StudioPalette {}
    implicitHeight: appearance.metrics.controlHeight
    implicitWidth: Math.max(60, contentItem.implicitWidth + 24)
    horizontalPadding: 12
    font.pixelSize: appearance.metrics.body
    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? appearance.colors.disabledText : control.highlighted ? appearance.colors.accentText : appearance.colors.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: appearance.metrics.radius
        color: control.highlighted ? appearance.colors.accentFill : control.down ? appearance.colors.pressed
             : control.selected || control.checked ? appearance.colors.selection
             : control.hovered ? appearance.colors.hover : appearance.colors.control
        border.width: control.visualFocus ? 2 : 0
        border.color: appearance.colors.accent
        opacity: control.enabled ? 1 : .5
    }
}
