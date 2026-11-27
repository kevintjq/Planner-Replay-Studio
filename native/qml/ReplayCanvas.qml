import QtQuick
import UAVStudio

Rectangle {
    id: canvas
    required property var controller
    required property var exporter
    property bool interactive: true
    property var captureOptions: ({})
    readonly property bool captureActive: Object.keys(captureOptions).length > 0
    function includesLayer(id) { return !captureActive || captureOptions.scope !== "layers" || (captureOptions.layerIds ?? []).indexOf(id) >= 0 }
    property alias showPath: cloud.showPath
    property alias pointSize: cloud.pointSize
    readonly property var activeLayout: controller.activeLayout
    readonly property bool videoOnly: activeLayout.mode === "video" && (exporter.exporting
        ? exporter.frames.some((frame, index) => frame.width > 0 && controller.videoLayers[index]?.id === activeLayout.video_id)
        : activeLayout.fullHasMedia)
    readonly property bool cloudOnly: activeLayout.mode === "cloud"
    function resetCamera() { cloud.resetCamera() }
    signal insetSelected(string id)
    signal sceneSelected(string id)
    color: captureActive && captureOptions.background !== undefined ? captureOptions.background : canvas.controller.captureMatte === 1 ? appearance.outputColors().matteBlack : canvas.controller.captureMatte === 2 ? appearance.outputColors().matteWhite
         : canvas.controller.transparentCanvas ? appearance.colors.transparent : canvas.controller.sceneBackground
    clip: true

    CloudView {
        id: cloud
        objectName: "primaryCloudView"
        property string exportViewId: "main"
        captureBackground: canvas.captureActive && canvas.captureOptions.background !== undefined ? canvas.captureOptions.background : ""
        z: 1
        visible: !canvas.videoOnly && canvas.includesLayer("main")
        transparent: canvas.controller.transparentCanvas || (!canvas.cloudOnly
                     && (canvas.activeLayout.backdropId.length > 0 || canvas.activeLayout.progress === 0))
        readonly property rect geometry: {
            if (canvas.cloudOnly)
                return Qt.rect(0, 0, canvas.width, canvas.height)
            const settings = canvas.controller.mainView
            const w = canvas.width * settings.width
            const h = canvas.height * settings.width
            const base = Qt.rect((canvas.width - w) * settings.x, (canvas.height - h) * settings.y, w, h)
            return canvas.controller.layerMotions.length
                ? canvas.controller.animatedLayer("main", "", canvas.controller.position, base, Qt.size(canvas.width, canvas.height))
                : base
        }
        x: geometry.x
        y: geometry.y
        width: geometry.width
        height: geometry.height
        controller: canvas.controller
        camera: canvas.controller.cameraIndex
        cameraZoom: canvas.controller.mainView.zoom
        MouseArea {
            anchors.fill: parent
            enabled: canvas.interactive && !canvas.exporter.exporting
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property real lastX: 0
            property real lastY: 0
            onPressed: function(mouse) { lastX = mouse.x; lastY = mouse.y }
            onPositionChanged: function(mouse) {
                if (!pressed) return
                if (pressedButtons & Qt.RightButton)
                    cloud.pan(mouse.x - lastX, mouse.y - lastY)
                else
                    cloud.orbit(mouse.x - lastX, mouse.y - lastY)
                lastX = mouse.x; lastY = mouse.y
            }
            onWheel: function(wheel) {
                cloud.zoom(wheel.angleDelta.y / 120)
                wheel.accepted = true
            }
        }
    }

    CloudInsets {
        z: 2
        anchors.fill: parent
        controller: canvas.controller
        pointSize: cloud.pointSize
        interactive: canvas.interactive
        captureOptions: canvas.captureOptions
        allowed: !canvas.videoOnly && !canvas.cloudOnly
        onSelected: id => canvas.insetSelected(id)
    }
    Repeater {
        model: canvas.controller.videoLayers.length
        delegate: FlightClip {
            required property int index
            controller: canvas.controller
            clipIndex: index
            footage: canvas.controller.videoLayers[index]
            layout: canvas.activeLayout
            videoOnly: canvas.videoOnly
            cloudOnly: canvas.cloudOnly
            exporting: canvas.exporter.exporting
            exportFrame: canvas.exporter.frames[index] ?? null
            included: canvas.includesLayer(footage.id)
        }
    }

    SceneOverlays {
        z: 3
        interactive: canvas.interactive
        showText: !canvas.videoOnly && !canvas.cloudOnly
        includeText: !canvas.captureActive || canvas.captureOptions.includeText !== false
        captureLayerIds: canvas.captureActive && canvas.captureOptions.scope === "layers" ? canvas.captureOptions.layerIds ?? [] : null
        anchors.fill: parent
        controller: canvas.controller
        onSelected: id => canvas.sceneSelected(id)
    }
    SpeedGauge {
        z: 4
        controller: canvas.controller
        interactive: canvas.interactive
        allowed: !canvas.videoOnly && !canvas.cloudOnly && canvas.includesLayer("speed") && (!canvas.captureActive || canvas.captureOptions.includeGauge !== false)
    }

}
