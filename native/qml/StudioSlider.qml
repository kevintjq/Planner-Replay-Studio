import QtQuick
import QtQuick.Controls as C
C.Slider {
    id: control
    palette: StudioPalette {}
    implicitWidth: 120; implicitHeight: appearance.metrics.controlHeight
    background: Rectangle {
        x: control.leftPadding; y: control.topPadding + (control.availableHeight - height) / 2
        width: control.availableWidth; height: 4; radius: 2; color: appearance.colors.control
        Rectangle { width: control.visualPosition * parent.width; height: parent.height; radius: 2; color: appearance.colors.accentFill; opacity: control.enabled ? 1 : .5 }
    }
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 18; height: 18; radius: 9
        color: control.pressed ? appearance.colors.control : appearance.colors.panel
        border.color: control.visualFocus ? appearance.colors.accent : appearance.colors.border
        border.width: control.visualFocus ? 2 : 1
    }
}
