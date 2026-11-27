import QtQuick
import QtQuick.Controls

Item {
    id: gauge
    required property var controller
    required property bool allowed
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
        color: "#d7192235"
        visible: !gauge.settings.transparent
    }
    Canvas {
        id: dial
        anchors.fill: parent
        renderStrategy: Canvas.Immediate
        readonly property real fraction: Math.max(0, Math.min(1, (Number(gauge.controller.speed) || 0) / gauge.settings.limit))
        onFractionChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            const center = [width / 2, height * .61]
            const radius = height * .46
            const start = Math.PI * .86, sweep = Math.PI * 1.28
            ctx.lineCap = "round"
            ctx.lineWidth = Math.max(2, width * .025)
            ctx.beginPath()
            ctx.strokeStyle = "#6685a9"
            ctx.arc(center[0], center[1], radius, start, start + sweep)
            ctx.stroke()
            if (fraction > 0) {
                ctx.beginPath()
                ctx.strokeStyle = "#82d9fd"
                ctx.arc(center[0], center[1], radius, start, start + sweep * fraction)
                ctx.stroke()
            }
        }
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * .30
        text: gauge.controller.speed
        font.pixelSize: Math.max(10, parent.width * .25)
        font.bold: true
        color: "#edf7ff"
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * .70
        text: "m/s"
        color: "#9fcce5"
        font.pixelSize: Math.max(8, parent.width * .10)
    }
    MouseArea {
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
