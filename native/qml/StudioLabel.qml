import QtQuick.Controls as C

C.Label {
    color: enabled ? appearance.colors.text : appearance.colors.disabledText
    font.pixelSize: appearance.metrics.body
    palette: StudioPalette {}
}
