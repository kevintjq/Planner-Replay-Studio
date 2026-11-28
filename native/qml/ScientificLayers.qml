import QtQuick
import UAVStudio

Item {
    id: root
    required property var controller
    required property var sceneCanvas
    property bool interactive: true
    property bool captureActive: false
    property var captureOptions: ({})
    readonly property var rows: controller.canvas.layers.filter(row => row.scientific)
    Repeater {
        parent: root.sceneCanvas
        model: root.rows.length
        delegate: CanvasElement {
            id: element
            required property int index
            readonly property var modelData: root.rows[index] || ({})
            parent: root.sceneCanvas
            controller: root.controller
            sceneCanvas: root.sceneCanvas
            layerId: modelData.id
            layerName: modelData.name || modelData.kind
            baseZ: modelData.order ?? 600
            baseGeometry: Qt.rect((modelData.x??.1)*root.width,(modelData.y??.1)*root.height,(modelData.width??.3)*root.width,(modelData.height??.15)*root.height)
            interactive: root.interactive && !root.captureActive
            contentAllowed: root.sceneCanvas.includesLayer(layerId)
            // Scientific content applies opacity inside the common Painter,
            // matching PDF/SVG/PNG primitives instead of fading its flattened
            // preview texture after overlapping outlines/shadows are painted.
            opacity: 1
            appearanceFrame.opacity: (layerSettings.backgroundOpacity ?? 1) * (layerSettings.opacity ?? baseOpacity)
            property alias loading: drawing.loading
            property alias error: drawing.error
            CanvasScientificItem {
                id: drawing
                anchors.fill: parent
                dataMap: {const revision=root.controller.canvas.revision;return root.controller.canvas.element(element.layerId,root.controller.position)}
                contentOpacity: element.layerSettings.opacity ?? element.baseOpacity
                textureSize: Qt.size(Math.max(1,Math.ceil(width*(root.captureOptions.renderScale || 1))),Math.max(1,Math.ceil(height*(root.captureOptions.renderScale || 1))))
            }
        }
    }
}
