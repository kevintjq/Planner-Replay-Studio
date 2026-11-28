import QtQuick
import UAVStudio

CanvasElement {
    id: gauge
    required property var meter
    required property bool allowed
    readonly property var legacy: controller.speedGauge
    readonly property var meterSettings: meter.settings || ({})
    readonly property var meterStyle: meter.style || ({})
    readonly property real naturalWidth: Math.min(sceneCanvas.width, sceneCanvas.height) * .34 * (layerId === "speed" ? legacy.scale ?? 1 : 1)
    readonly property real naturalHeight: naturalWidth * 520 / 600
    layerId: meter.id
    layerName: meter.label || "Speed Meter · 速度表"
    baseZ: 500
    baseGeometry: {
        const base = Qt.rect((sceneCanvas.width-naturalWidth)*(layerId === "speed" ? legacy.x ?? .05 : .05),
                             (sceneCanvas.height-naturalHeight)*(layerId === "speed" ? legacy.y ?? .32 : .32), naturalWidth, naturalHeight)
        return controller.layerMotions.length ? controller.animatedLayer(layerId, "", controller.position, base, Qt.size(sceneCanvas.width, sceneCanvas.height)) : base
    }
    contentAllowed: allowed && (layerId !== "speed" || (legacy.visible && controller.position >= legacy.start && (legacy.end < 0 || controller.position < legacy.end)))
    function presetStyle(name) { return drawing.presetStyle(name) }
    function exportElement(destination, size, dpi, format) { return drawing.exportElement(destination, size, dpi, format) }
    signal exportFinished(string jobId, bool success, var metadata, string error)
    SpeedGaugeItem {
        id: drawing
        objectName: "speedGaugeDrawing-" + gauge.layerId
        anchors.fill: parent
        dataMap: {
            const revision = speedMeterData.sources
            const settings = Object.assign({}, gauge.meterSettings, {playheadNs:gauge.controller.experimentStampNs})
            const result = speedMeterData.frame(gauge.layerId, gauge.controller.experimentStampNs, settings)
            if (gauge.meterStyle.preset === "speed-curve") result.plot = speedMeterData.plot(gauge.layerId, settings)
            return Object.assign({unit:settings.unit || "m/s"}, result)
        }
        styleMap: gauge.meterStyle
        textureSize: Qt.size(Math.max(1,Math.ceil(width*(gauge.sceneCanvas.captureOptions.renderScale || 1))),
                             Math.max(1,Math.ceil(height*(gauge.sceneCanvas.captureOptions.renderScale || 1))))
        onExportFinished: (jobId, success, metadata, error) => gauge.exportFinished(jobId, success, metadata, error)
    }
}
