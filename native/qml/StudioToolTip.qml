import QtQuick
import QtQuick.Controls as C

C.ToolTip {
    id: control
    delay: 600
    padding: 8
    implicitWidth: Math.min(420, contentItem.implicitWidth + leftPadding + rightPadding)
    font.pixelSize: appearance.metrics.caption
    contentItem: Text {
        text: control.text
        font: control.font
        color: appearance.colors.text
        wrapMode: Text.Wrap
    }
    background: Rectangle {
        color: appearance.colors.tooltip
        radius: appearance.metrics.radius
        border.color: appearance.colors.border
    }
}
