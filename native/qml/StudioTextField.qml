import QtQuick
import QtQuick.Controls as C

C.TextField {
    id: control
    implicitHeight: appearance.metrics.controlHeight
    leftPadding: 9
    rightPadding: 9
    font.pixelSize: appearance.metrics.body
    color: enabled ? appearance.colors.text : appearance.colors.disabledText
    placeholderTextColor: appearance.colors.secondaryText
    selectionColor: appearance.colors.selection
    selectedTextColor: appearance.colors.text
    background: Rectangle {
        radius: appearance.metrics.radius
        color: appearance.colors.input
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? appearance.colors.accent : appearance.colors.border
    }
}
