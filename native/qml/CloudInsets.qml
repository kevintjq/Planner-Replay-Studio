import QtQuick
import UAVStudio

Item {
    id: insets
    required property var controller
    required property real pointSize
    required property bool allowed
    property bool interactive: true
    property var captureOptions: ({})
    property var nativeClient: null
    property bool nativeEnabled: false
    property var nativeCoordinator: null
    property var exporter: null
    property string nativeTargetNs: ""
    property bool nativeRequiresSync: false
    signal selected(string id)

    Repeater {
        parent: insets.parent
        model: insets.controller.cloudViews.length
        delegate: CanvasElement {
            id: inset
            required property int index
            parent: insets.parent
            sceneCanvas: insets.parent
            controller: insets.controller
            layerId: settings.id ?? ""
            layerName: settings.label ?? layerId
            interactive: insets.interactive
            cameraElement: true
            baseZ: 300 + index
            readonly property var settings: insets.controller.cloudViews[index] ?? {}
            readonly property real normalWidth: Math.min(insets.width * (settings.width ?? .32), insets.height * 16 / 9)
            readonly property real normalHeight: normalWidth * 9 / 16
            readonly property real spanX: Math.max(0, insets.width - normalWidth)
            readonly property real spanY: Math.max(0, insets.height - normalHeight)
            baseGeometry: {
                const base = Qt.rect(spanX * relativeX, spanY * relativeY, normalWidth, normalHeight)
                return insets.controller.layerMotions.length
                    ? insets.controller.animatedLayer("view", settings.id, insets.controller.position, base, Qt.size(insets.width, insets.height))
                    : base
            }
            property real relativeX: settings.x ?? 0
            property real relativeY: settings.y ?? 0
            contentAllowed: insets.allowed && (insets.captureOptions.scope !== "layers" || (insets.captureOptions.layerIds ?? []).indexOf(settings.id) >= 0)

            Loader {
                id: compatibleLoader
                anchors.fill: parent
                active: !insets.nativeEnabled
                sourceComponent: CloudView {
                id: scene
                property string exportViewId: inset.settings.id ?? ""
                anchors.fill: parent
                captureBackground: Object.keys(insets.captureOptions).length > 0 && insets.captureOptions.background !== undefined ? insets.captureOptions.background : ""
                controller: insets.controller
                camera: inset.settings.camera ?? 0
                cameraZoom: inset.settings.zoom ?? 1
                overlays: inset.settings.overlays ?? []
                targetFrame: inset.settings.targetFrame ?? ""
                followMode: inset.settings.followMode ?? 0
                followLag: inset.settings.followLag ?? 0
                smoothing: inset.settings.smoothing ?? 0
                opticalFrame: inset.settings.opticalFrame ?? false
                cloudVisible: inset.settings.cloudVisible ?? true
                cloudColor: inset.settings.cloudColor ?? ""
                pathColor: inset.settings.pathColor ?? ""
                viewBackground: inset.settings.viewBackground ?? ""
                hiddenDisplays: inset.settings.hiddenDisplays ?? []
                pointSize: inset.settings.pointSize ?? insets.pointSize
                transparent: inset.settings.transparent ?? true
                auxiliary: true
                }
            }

            Loader {
                anchors.fill: parent
                active: insets.nativeEnabled
                sourceComponent: NativeSceneView {
                    anchors.fill: parent
                    client: insets.nativeClient
                    coordinator: insets.nativeCoordinator
                    exporter: insets.exporter
                    viewId: inset.settings.id ?? ""
                    targetNs: insets.nativeTargetNs
                    requireSynchronized: insets.nativeRequiresSync
                    interactive: insets.interactive && !insets.controller.canvas.layoutMode
                    showDiagnostics: insets.interactive && Object.keys(insets.captureOptions).length === 0
                    onSelected: viewId => insets.selected(viewId)
                }
            }

            Rectangle {
                objectName: "insetCameraStatus-" + (inset.settings.id ?? "")
                // Camera diagnostics belong to editing UI, never exported content.
                visible: !insets.nativeEnabled && insets.interactive && Object.keys(insets.captureOptions).length === 0
                         && !!compatibleLoader.item && compatibleLoader.item.camera !== 0
                         && !compatibleLoader.item.followingTarget && compatibleLoader.item.cameraStatus.length > 0
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: cameraMessage.implicitHeight + 10
                color: appearance.colors.panel
                opacity: .94
                StudioLabel {
                    id: cameraMessage
                    anchors.fill: parent
                    anchors.margins: 5
                    text: compatibleLoader.item?.cameraStatus ?? ""
                    wrapMode: Text.Wrap
                    color: appearance.colors.text
                    font.pixelSize: appearance.metrics.caption
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: !insets.nativeEnabled && insets.interactive && !insets.controller.canvas.layoutMode
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                property point previous
                onPressed: mouse => { previous=Qt.point(mouse.x,mouse.y); insets.selected(inset.settings.id) }
                onPositionChanged: mouse => {
                    if(!pressed)return
                    if(pressedButtons&(Qt.RightButton|Qt.MiddleButton))compatibleLoader.item.pan(mouse.x-previous.x,mouse.y-previous.y)
                    else compatibleLoader.item.orbit(mouse.x-previous.x,mouse.y-previous.y)
                    previous=Qt.point(mouse.x,mouse.y)
                }
                onWheel: wheel => { compatibleLoader.item.zoom(wheel.angleDelta.y/120);wheel.accepted=true }
            }
        }
    }
}
