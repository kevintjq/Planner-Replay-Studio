import QtQuick
import QtQuick.Controls as C

C.ScrollBar {
    id: control
    padding: 2
    contentItem: Rectangle {
        implicitWidth: 8; implicitHeight: 8
        radius: 4
        color: control.pressed ? appearance.colors.secondaryText : appearance.colors.scrollbar
        opacity: control.active || control.hovered ? .85 : .35
    }
    background: Item {}
}
