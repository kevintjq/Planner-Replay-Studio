import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

Item {
    id: gauge
    required property var controller
    required property bool allowed
    property bool interactive: true
    readonly property var outputColors: appearance.outputColors(settings.scheme ?? "dark")
    readonly property var settings: controller.speedGauge
    readonly property real horizontal: Math.max(0, parent.width - width)
    readonly property real vertical: Math.max(0, parent.height - height)
    property real previewX: settings.x
    property real previewY: settings.y
    width: Math.min(parent.width, parent.height) * .23 * (settings.scale ?? 1)
    height: width * .75
    readonly property rect geometry: {
        const base = Qt.rect(horizontal * previewX, vertical * previewY, width, height)
        return controller.layerMotions.length
            ? controller.animatedLayer("speed", "", controller.position, base, Qt.size(parent.width, parent.height))
            : base
    }
    x: geometry.x
    y: geometry.y
    transformOrigin: Item.TopLeft
    scale: width > 0 ? geometry.width / width : 1
    visible: allowed && settings.visible && controller.position >= settings.start
             && (settings.end < 0 || controller.position < settings.end)

    Rectangle {
        anchors.fill: parent
        radius: height * .18
        color: gauge.outputColors.gaugeBackground
        visible: !gauge.settings.transparent
    }
    Shape {
        id: dial
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        readonly property real fraction: Math.max(0, Math.min(1, (Number(gauge.controller.speed) || 0) / gauge.settings.limit))
        ShapePath {
            fillColor: gauge.outputColors.transparent
            strokeColor: gauge.outputColors.border
            strokeWidth: Math.max(2, gauge.width * .025)
            capStyle: ShapePath.RoundCap
            PathAngleArc { centerX: dial.width / 2; centerY: dial.height * .61; radiusX: dial.height * .46; radiusY: radiusX; startAngle: 154.8; sweepAngle: 230.4 }
        }
        ShapePath {
            fillColor: gauge.outputColors.transparent
            strokeColor: dial.fraction > 0 ? gauge.outputColors.accent : gauge.outputColors.transparent
            strokeWidth: Math.max(2, gauge.width * .025)
            capStyle: ShapePath.RoundCap
            PathAngleArc { centerX: dial.width / 2; centerY: dial.height * .61; radiusX: dial.height * .46; radiusY: radiusX; startAngle: 154.8; sweepAngle: 230.4 * dial.fraction }
        }
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * .30
        text: gauge.controller.speed
        font.pixelSize: Math.max(10, parent.width * .25)
        font.bold: true
        color: gauge.outputColors.text
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * .70
        text: "m/s"
        color: gauge.outputColors.secondaryText
        font.pixelSize: Math.max(8, parent.width * .10)
    }
    MouseArea {
        enabled: gauge.interactive
        anchors.fill: parent
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property point origin
        property real initialX
        property real initialY
        onPressed: mouse => {
            origin = mapToItem(gauge.parent, mouse.x, mouse.y)
            initialX = gauge.previewX
            initialY = gauge.previewY
            gauge.controller.pause()
        }
        onPositionChanged: mouse => {
            if (pressed) {
                const current = mapToItem(gauge.parent, mouse.x, mouse.y)
                gauge.previewX = Math.max(0, Math.min(1, initialX + (current.x - origin.x) / Math.max(1, gauge.horizontal)))
                gauge.previewY = Math.max(0, Math.min(1, initialY + (current.y - origin.y) / Math.max(1, gauge.vertical)))
            }
        }
        onReleased: gauge.controller.setSpeedGauge({x: gauge.previewX, y: gauge.previewY})
        onCanceled: {
            gauge.previewX = Qt.binding(() => gauge.settings.x)
            gauge.previewY = Qt.binding(() => gauge.settings.y)
        }
        Connections {
            target: gauge.controller
            function onRecordingChanged() {
                if (!parent.pressed) {
                    gauge.previewX = Qt.binding(() => gauge.settings.x)
                    gauge.previewY = Qt.binding(() => gauge.settings.y)
                }
            }
        }
    }
}
