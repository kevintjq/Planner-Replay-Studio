import QtQuick
import UAVStudio

Item {
    id: insets
    required property var controller
    required property real pointSize
    required property bool allowed
    property bool interactive: true
    property var captureOptions: ({})
    signal selected(string id)

    Repeater {
        model: insets.controller.cloudViews.length
        delegate: Item {
            id: inset
            required property int index
            readonly property var settings: insets.controller.cloudViews[index] ?? {}
            readonly property real normalWidth: Math.min(insets.width * (settings.width ?? .32), insets.height * 16 / 9)
            readonly property real normalHeight: normalWidth * 9 / 16
            readonly property real spanX: Math.max(0, insets.width - normalWidth)
            readonly property real spanY: Math.max(0, insets.height - normalHeight)
            readonly property rect geometry: {
                const base = Qt.rect(spanX * relativeX, spanY * relativeY, normalWidth, normalHeight)
                return insets.controller.layerMotions.length
                    ? insets.controller.animatedLayer("view", settings.id, insets.controller.position, base, Qt.size(insets.width, insets.height))
                    : base
            }
            property real relativeX: settings.x ?? 0
            property real relativeY: settings.y ?? 0
            visible: insets.allowed && (insets.captureOptions.scope !== "layers" || (insets.captureOptions.layerIds ?? []).indexOf(settings.id) >= 0)
            x: geometry.x
            y: geometry.y
            width: geometry.width
            height: geometry.height

            CloudView {
                property string exportViewId: inset.settings.id ?? ""
                anchors.fill: parent
                captureBackground: Object.keys(insets.captureOptions).length > 0 && insets.captureOptions.background !== undefined ? insets.captureOptions.background : ""
                controller: insets.controller
                camera: inset.settings.camera ?? 0
                cameraZoom: inset.settings.zoom ?? 1
                overlays: inset.settings.overlays ?? []
                pointSize: insets.pointSize
                transparent: inset.settings.transparent ?? true
                auxiliary: true
            }

            MouseArea {
                id: move
                enabled: insets.interactive
                anchors.fill: parent
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property point start
                property bool moved: false
                property real initialX
                property real initialY

                function resetPosition() {
                    inset.relativeX = Qt.binding(() => inset.settings.x ?? 0)
                    inset.relativeY = Qt.binding(() => inset.settings.y ?? 0)
                }

                onPressed: mouse => {
                    start = mapToItem(insets, mouse.x, mouse.y)
                    initialX = inset.relativeX
                    initialY = inset.relativeY
                    insets.controller.pause()
                    moved = false
                    insets.selected(inset.settings.id)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const current = mapToItem(insets, mouse.x, mouse.y)
                    const dx = current.x - start.x
                    const dy = current.y - start.y
                    if (!moved && Math.abs(dx) + Math.abs(dy) < 2)
                        return
                    moved = true
                    if (inset.spanX > 0)
                        inset.relativeX = Math.max(0, Math.min(1, initialX + dx / inset.spanX))
                    if (inset.spanY > 0)
                        inset.relativeY = Math.max(0, Math.min(1, initialY + dy / inset.spanY))
                }
                onReleased: {
                    if (moved) {
                        insets.controller.setCloudView(inset.settings.id, {x: inset.relativeX, y: inset.relativeY})
                        resetPosition()
                    }
                }
                onCanceled: resetPosition()
            }
        }
    }
}
