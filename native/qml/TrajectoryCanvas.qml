import QtQuick
import UAVStudio

EditableCanvas {
    id: canvas
    color: "transparent"
    readonly property var rows: controller.canvasLayers
    Repeater {
        model: canvas.controller.canvasLayerModel
        delegate: CanvasElement {
            id: nativeLayer
            required property var layerRow
            readonly property var modelData: layerRow || ({})
            controller: canvas.controller
            sceneCanvas: canvas
            layerId: modelData.id
            layerName: modelData.name
            baseZ: modelData.order
            baseOpacity: modelData.opacity ?? 1
            baseGeometry: Qt.rect(modelData.x*canvas.width,modelData.y*canvas.height,modelData.width*canvas.width,modelData.height*canvas.height)
            interactive: canvas.interactive && !canvas.captureActive && !canvas.controller.working
            contentAllowed: canvas.includesLayer(layerId)
            readonly property bool vectorLayer: ["trajectory-line","trajectory-start","trajectory-goal"].indexOf(modelData.kind) >= 0
            Image {
                anchors.fill: parent
                visible: !nativeLayer.vectorLayer
                source: nativeLayer.vectorLayer ? "" : (nativeLayer.modelData.source || "")
                cache: false
                asynchronous: true
                retainWhileLoading: true
                fillMode: Image.Stretch
            }
            Loader {
                anchors.fill: parent
                active: nativeLayer.vectorLayer
                sourceComponent: TrajectoryLayerItem {
                    controller: canvas.controller
                    layerId: nativeLayer.layerId
                    textureSize: Qt.size(Math.max(1,Math.ceil(width*(canvas.captureOptions.renderScale || 1))),Math.max(1,Math.ceil(height*(canvas.captureOptions.renderScale || 1))))
                }
            }
        }
    }
    ScientificLayers { anchors.fill:parent;controller:canvas.controller;sceneCanvas:canvas;interactive:canvas.interactive&&!canvas.controller.working;captureActive:canvas.captureActive;captureOptions:canvas.captureOptions }
}
