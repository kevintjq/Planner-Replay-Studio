import QtQuick
import QtQuick.Controls

ScrollView {
    id: control
    palette: StudioPalette {}

    // Replacing a ScrollView's styled bars also replaces their geometry.
    // Keep these constraints here; Flickable/ListView attached bars position
    // themselves and continue to use StudioScrollBar directly.
    ScrollBar.vertical: StudioScrollBar {
        objectName: "studioVerticalScrollBar"
        parent: control
        x: control.mirrored ? 0 : control.width - width
        y: control.topPadding
        height: control.availableHeight
        visible: policy === ScrollBar.AlwaysOn || (policy === ScrollBar.AsNeeded && size < 1)
        active: control.ScrollBar.horizontal.active
    }
    ScrollBar.horizontal: StudioScrollBar {
        objectName: "studioHorizontalScrollBar"
        parent: control
        x: control.leftPadding
        y: control.height - height
        width: control.availableWidth
        visible: policy === ScrollBar.AlwaysOn || (policy === ScrollBar.AsNeeded && size < 1)
        active: control.ScrollBar.vertical.active
    }
}
