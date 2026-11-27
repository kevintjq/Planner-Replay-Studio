import QtQuick
import PlannerReplay

Rectangle {
    id: canvas
    required property var controller
    required property var exporter
    property bool interactive: true
    readonly property var activeLayout: controller.activeLayout
    readonly property bool videoOnly: activeLayout.mode === "video" && (exporter.exporting
        ? exporter.frames.some((frame, index) => frame.width > 0 && controller.videoLayers[index]?.id === activeLayout.video_id)
        : activeLayout.fullHasMedia)
    readonly property bool cloudOnly: activeLayout.mode === "cloud"
    signal insetSelected(string id)
    signal sceneSelected(string id)
    color: canvas.controller.captureMatte === 1 ? "black" : canvas.controller.captureMatte === 2 ? "white"
         : canvas.controller.transparentCanvas ? "transparent" : "#08101a"
    clip: true

    CloudView {
        id: cloud
        z: 1
        visible: !canvas.videoOnly
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
            enabled: canvas.interactive
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
        }
    }

    SceneOverlays {
        z: 3
        interactive: canvas.interactive
        showText: !canvas.videoOnly && !canvas.cloudOnly
        anchors.fill: parent
        controller: canvas.controller
        onSelected: id => canvas.sceneSelected(id)
    }
    SpeedGauge {
        z: 4
        controller: canvas.controller
        interactive: canvas.interactive
        allowed: !canvas.videoOnly && !canvas.cloudOnly
    }

}
