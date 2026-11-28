import QtQuick
import QtQuick.Controls as C
C.CheckBox {
    id: control
    palette: StudioPalette {}
    font.pixelSize: appearance.metrics.body
    implicitHeight: appearance.metrics.controlHeight
    spacing: 8
    indicator: Rectangle {
        implicitWidth: 16; implicitHeight: 16
        x: control.leftPadding; y: (control.height - height) / 2
        radius: 4
        color: control.checked ? appearance.colors.accentFill : appearance.colors.input
        border.color: control.visualFocus ? appearance.colors.accent : appearance.colors.border
        Text { anchors.centerIn: parent; text: "✓"; visible: control.checked; font.pixelSize: 12; color: appearance.colors.accentText }
        opacity: control.enabled ? 1 : .5
    }
    contentItem: Text {
        text: control.text; font: control.font
        leftPadding: control.indicator.width + control.spacing
        color: control.enabled ? appearance.colors.text : appearance.colors.disabledText
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
