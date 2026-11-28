import QtQuick
import UAVStudio

EditableCanvas {
    id: canvas
    required property var exporter
    property var nativeClient: null
    property bool nativeEnabled: false
    property bool nativeConfigurationPending: false
    property bool nativeFrameReady: false
    readonly property bool nativeSceneActive: nativeEnabled && !!nativeClient && nativeClient.connected
    readonly property var nativeSceneClient: nativeClient
    readonly property alias nativeViewCoordinator: nativeCoordinator
    property string nativeTargetNs: controller.experimentStampNs ?? ""
    property bool nativeRequiresSync: !!controller.nativeReplaySource?.bag
    property bool showPath: true
    property real pointSize: controller.mainView.pointSize ?? 1.5
    readonly property bool hasMainLayer: controller.canvas.layers.some(row => row.id === "main")
    readonly property bool outputDataPending: {
        const sources = speedMeterData.sources
        return (!nativeSceneActive && controller.independentDisplayLoading)
            || controller.speedMeters.some(meter => layerItems[meter.id]?.visible && speedMeterData.sourceState(meter.id).working)
            || Object.values(layerItems).some(item => item.visible && item.loading === true)
    }
    readonly property string outputDataError: Object.values(layerItems).filter(item=>item.visible && item.error).map(item=>item.layerName+": "+item.error).join("\n")
    readonly property var activeLayout: controller.activeLayout
    readonly property bool videoOnly: activeLayout.mode === "video" && (exporter.exporting
        ? exporter.frames.some((frame, index) => frame.width > 0 && controller.videoLayers[index]?.id === activeLayout.video_id)
        : activeLayout.fullHasMedia)
    readonly property bool cloudOnly: activeLayout.mode === "cloud"
    function resetCamera() {
        if (nativeSceneActive) nativeClient.resetCamera()
        else if (cloudLoader.item) cloudLoader.item.resetCamera()
    }
    signal insetSelected(string id)
    signal sceneSelected(string id)
    color: captureActive && captureOptions.background !== undefined ? captureOptions.background : canvas.controller.captureMatte === 1 ? appearance.outputColors().matteBlack : canvas.controller.captureMatte === 2 ? appearance.outputColors().matteWhite
         : canvas.controller.transparentCanvas ? appearance.colors.transparent : canvas.controller.sceneBackground
    clip: true

    NativeViewCoordinator {
        id: nativeCoordinator
        client: canvas.nativeClient
        active: canvas.nativeSceneActive && !canvas.captureActive && !canvas.exporter.exporting
        configurationPending: canvas.nativeConfigurationPending
        views: canvas.controller.cloudViews
    }
    CanvasElement {
        id: mainLayer
        controller:canvas.controller;sceneCanvas:canvas;layerId:"main";layerName:"Main 3D · 主三维视角"
        cameraElement:true;interactive:canvas.interactive&&!canvas.captureActive&&!canvas.exporter.exporting
        baseZ:100
        baseGeometry: {
            const settings=canvas.controller.mainView,w=canvas.width*settings.width,h=canvas.height*settings.width
            const base=canvas.cloudOnly?Qt.rect(0,0,canvas.width,canvas.height):Qt.rect((canvas.width-w)*settings.x,(canvas.height-h)*settings.y,w,h)
            return canvas.controller.layerMotions.length?canvas.controller.animatedLayer("main","",canvas.controller.position,base,Qt.size(canvas.width,canvas.height)):base
        }
        contentAllowed:!canvas.videoOnly&&canvas.includesLayer("main")
      Loader {
        id: cloudLoader
        anchors.fill: parent
        active: canvas.hasMainLayer && !canvas.nativeSceneActive
        sourceComponent: CloudView {
        id: cloud
        objectName: "primaryCloudView"
        property string exportViewId: "main"
        captureBackground: canvas.captureActive && canvas.captureOptions.background !== undefined ? canvas.captureOptions.background : ""
        visible: !canvas.nativeSceneActive
        transparent: canvas.controller.transparentCanvas || (!canvas.cloudOnly
                     && (canvas.activeLayout.backdropId.length > 0 || canvas.activeLayout.progress === 0))
        anchors.fill:parent
        controller: canvas.controller
        camera: canvas.controller.cameraIndex
        cameraZoom: canvas.controller.mainView.zoom
        targetFrame: canvas.controller.mainView.targetFrame ?? ""
        followMode: canvas.controller.mainView.followMode ?? 0
        followLag: canvas.controller.mainView.followLag ?? 0
        smoothing: canvas.controller.mainView.smoothing ?? 0
        opticalFrame: canvas.controller.mainView.opticalFrame ?? false
        cloudVisible: canvas.controller.mainView.cloudVisible ?? true
        cloudColor: canvas.controller.mainView.cloudColor || ""
        pathColor: canvas.controller.mainView.pathColor || ""
        viewBackground: canvas.controller.mainView.viewBackground || ""
        hiddenDisplays: canvas.controller.mainView.hiddenDisplays ?? []
        pointSize: canvas.pointSize
        showPath: canvas.showPath
        MouseArea {
            anchors.fill: parent
            enabled: canvas.interactive && !canvas.exporter.exporting && !canvas.controller.canvas.layoutMode
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            property real lastX: 0
            property real lastY: 0
            onPressed: function(mouse) { lastX = mouse.x; lastY = mouse.y }
            onPositionChanged: function(mouse) {
                if (!pressed) return
                if ((pressedButtons & Qt.MiddleButton) || (mouse.modifiers & Qt.ShiftModifier))
                    cloud.pan(mouse.x - lastX, mouse.y - lastY)
                else if (pressedButtons & Qt.RightButton)
                    cloud.zoom((lastY - mouse.y) / 30)
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
      }

    NativeSceneView {
        id: nativeView
        anchors.fill:parent
        visible: canvas.nativeSceneActive && canvas.hasMainLayer
        client: canvas.nativeClient
        coordinator: nativeCoordinator
        exporter: canvas.exporter
        viewId: "main"
        targetNs: canvas.nativeTargetNs
        requireSynchronized: canvas.nativeRequiresSync
        interactive: canvas.interactive && !canvas.captureActive && !canvas.exporter.exporting && !canvas.controller.canvas.layoutMode
        showDiagnostics: canvas.interactive && !canvas.captureActive && !canvas.videoOnly
    }
    }
    StudioLabel {
        objectName: "nativeViewSyncError"
        z: 90001
        anchors.centerIn:mainLayer
        width:Math.max(0,mainLayer.width-24)
        visible: nativeView.visible && !canvas.captureActive && canvas.interactive && nativeCoordinator.syncError.length > 0
        text: "Native views · 原生多视角：" + nativeCoordinator.syncError
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: appearance.colors.text
        padding: 10
        background: Rectangle { radius: appearance.metrics.radius; color: appearance.colors.panel }
    }

    StudioLabel {
        objectName: "cameraStatusMessage"
        z: 90002
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
        width: Math.min(implicitWidth, parent.width - 16)
        visible: !canvas.nativeSceneActive && canvas.interactive && !canvas.captureActive && !canvas.videoOnly && text.length > 0
        text: cloudLoader.item?.cameraStatus || ""
        color: appearance.colors.text
        padding: 6
        wrapMode: Text.Wrap
        background: Rectangle { color: appearance.colors.panel; radius: appearance.metrics.radius }
    }

    CloudInsets {
        anchors.fill: parent
        controller: canvas.controller
        pointSize: canvas.pointSize
        interactive: canvas.interactive && !canvas.captureActive && !canvas.exporter.exporting
        captureOptions: canvas.captureOptions
        nativeClient: canvas.nativeClient
        nativeEnabled: canvas.nativeSceneActive
        nativeCoordinator: canvas.nativeViewCoordinator
        exporter: canvas.exporter
        nativeTargetNs: canvas.nativeTargetNs
        nativeRequiresSync: canvas.nativeRequiresSync
        allowed: !canvas.videoOnly && !canvas.cloudOnly
        onSelected: id => canvas.insetSelected(id)
    }
    Repeater {
        model: canvas.controller.videoLayerModel
        delegate: FlightClip {
            required property int index
            required property var videoLayer
            controller: canvas.controller
            clipIndex: index
            footage: videoLayer
            layout: canvas.activeLayout
            videoOnly: canvas.videoOnly
            cloudOnly: canvas.cloudOnly
            exporting: canvas.exporter.exporting
            sceneCanvas:canvas
            exportFrame: canvas.exporter.frames[index] ?? null
            included: canvas.includesLayer(footage.id)
        }
    }

    SceneOverlays {
        interactive: canvas.interactive && !canvas.captureActive && !canvas.exporter.exporting
        showText: !canvas.videoOnly && !canvas.cloudOnly
        includeText: !canvas.captureActive || canvas.captureOptions.includeText !== false
        captureLayerIds: canvas.captureActive && canvas.captureOptions.scope === "layers" ? canvas.captureOptions.layerIds ?? [] : null
        anchors.fill: parent
        controller: canvas.controller
        onSelected: id => canvas.sceneSelected(id)
    }
    Repeater {
        model: canvas.controller.speedMeters.length
        delegate: SpeedGauge {
            required property int index
            meter: canvas.controller.speedMeters[index] || ({})
            controller: canvas.controller
            sceneCanvas: canvas
            interactive: canvas.interactive && !canvas.captureActive && !canvas.exporter.exporting
            allowed: !canvas.videoOnly && !canvas.cloudOnly && canvas.includesLayer(layerId) && (!canvas.captureActive || canvas.captureOptions.includeGauge !== false)
        }
    }
    ScientificLayers { anchors.fill:parent;controller:canvas.controller;sceneCanvas:canvas;interactive:canvas.interactive&&!canvas.exporter.exporting;captureActive:canvas.captureActive;captureOptions:canvas.captureOptions }

}
