import QtQuick
import QtQuick.Controls

Item {
    id: element
    default property alias layerData: content.data
    required property var controller
    required property string layerId
    property string layerName: layerId
    property Item sceneCanvas: parent
    property rect baseGeometry: Qt.rect(0, 0, 100, 100)
    property real baseZ: 1
    property real baseOpacity: 1
    property bool contentAllowed: true
    property bool interactive: true
    property bool cameraElement: false
    property bool clipContent: true
    readonly property var layerModel: controller.canvas
    readonly property var layerSettings: { const revision = layerModel.revision; return layerModel.settings(layerId, controller.position) }
    readonly property rect geometry: { const revision = layerModel.revision; return layerModel.geometry(layerId, baseGeometry, Qt.size(sceneCanvas.width, sceneCanvas.height), controller.position) }
    readonly property bool selected: layerModel.selection.indexOf(layerId) >= 0
    readonly property real angle: layerSettings.rotation ?? 0
    readonly property real anchorX: layerSettings.anchorX ?? .5
    readonly property real anchorY: layerSettings.anchorY ?? .5
    readonly property bool editable: interactive && !layerSettings.locked && (!cameraElement || layerModel.layoutMode)
    x: geometry.x; y: geometry.y; width: geometry.width; height: geometry.height
    z: layerSettings.z ?? baseZ
    opacity: layerSettings.opacity ?? baseOpacity
    visible: { const revision = layerModel.revision; return contentAllowed && layerModel.active(layerId, controller.position) }
    transform: Rotation { angle: element.angle; origin.x: element.width * element.anchorX; origin.y: element.height * element.anchorY }
    property string registeredId: ""
    property Item registeredCanvas: null
    function updateRegistration() {
        if (registeredCanvas?.unregisterLayer) registeredCanvas.unregisterLayer(registeredId, element)
        registeredId = layerId
        registeredCanvas = sceneCanvas
        if (registeredCanvas?.registerLayer) registeredCanvas.registerLayer(registeredId, element)
    }
    Component.onCompleted: updateRegistration()
    Component.onDestruction: if (registeredCanvas?.unregisterLayer) registeredCanvas.unregisterLayer(registeredId, element)
    onLayerIdChanged: updateRegistration()
    onSceneCanvasChanged: updateRegistration()

    property Item contentItem: Item {
        id: content
        parent: element; anchors.fill: parent; clip: element.clipContent
        transform: Scale {
            origin.x: content.width / 2; origin.y: content.height / 2
            xScale: element.layerSettings.flipH ? -1 : 1
            yScale: element.layerSettings.flipV ? -1 : 1
        }
    }
    property Rectangle appearanceFrame: Rectangle {
        parent: element; anchors.fill: parent; z: -1
        color: element.layerSettings.backgroundMode === "solid" ? (element.layerSettings.backgroundColor || "transparent")
             : element.layerSettings.backgroundMode === "translucent" ? (element.layerSettings.backgroundColor || "transparent") : "transparent"
        opacity: element.layerSettings.backgroundOpacity ?? 1
        radius: (element.layerSettings.cornerRadius ?? 0) * element.sceneCanvas.width / element.layerModel.outputWidth
        border.width: (element.layerSettings.borderWidth ?? 0) * element.sceneCanvas.width / element.layerModel.outputWidth
        border.color: element.layerSettings.borderColor || "transparent"
    }
    property MouseArea moveArea: MouseArea {
        parent: element; anchors.fill: parent; z: 10000
        enabled: element.editable
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property point origin
        property var initial: []
        onPressed: mouse => {
            element.sceneCanvas.forceActiveFocus()
            const additive = !!(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))
            if (!element.selected || additive) element.layerModel.select(element.layerId, additive)
            element.sceneCanvas.layerSelected(element.layerId)
            if (mouse.button === Qt.RightButton) { element.contextMenu.popup(); return }
            origin = mapToItem(element.sceneCanvas, mouse.x, mouse.y)
            initial = element.sceneCanvas.selectionFrames()
            element.layerModel.beginEdit()
        }
        onPositionChanged: mouse => {
            if (!(pressedButtons & Qt.LeftButton)) return
            const p = mapToItem(element.sceneCanvas, mouse.x, mouse.y)
            let dx = (p.x-origin.x)/element.sceneCanvas.width, dy = (p.y-origin.y)/element.sceneCanvas.height
            const moved = element.sceneCanvas.snapDelta(element.layerId, initial, dx, dy)
            for (const row of initial) element.layerModel.edit(row.id, {x:row.x+moved.x, y:row.y+moved.y, width:row.width, height:row.height})
        }
        onReleased: { element.layerModel.endEdit(); element.sceneCanvas.clearGuides() }
        onCanceled: { element.layerModel.endEdit(true); element.sceneCanvas.clearGuides() }
    }
    property Menu contextMenu: Menu {
        palette: StudioPalette {}
        MenuItem { text: "Duplicate · 复制图层"; onTriggered: element.layerModel.duplicate(element.layerId) }
        MenuItem { text: "Bring to Front · 置于顶层"; onTriggered: element.layerModel.moveLayer(element.layerId,-2) }
        MenuItem { text: "Send to Back · 置于底层"; onTriggered: element.layerModel.moveLayer(element.layerId,2) }
        MenuItem { text: "Remove · 删除图层"; onTriggered: element.layerModel.removeSelected() }
    }
    property Item selectionChrome: Item {
        id: chrome
        parent: element.sceneCanvas
        x: element.x; y: element.y; width: element.width; height: element.height; z: 100000
        visible: element.visible && element.interactive && element.selected
        transform: Rotation { angle: element.angle; origin.x: chrome.width * element.anchorX; origin.y: chrome.height * element.anchorY }
        Rectangle { anchors.fill: parent; color: "transparent"; border.color: appearance.colors.accent; border.width: 1.5 }
        Rectangle {
            y: -22; height: 20; width: Math.min(chrome.width, name.implicitWidth + 12); radius: 3; color: appearance.colors.accent
            Text { id: name; anchors.fill: parent; leftPadding: 6; rightPadding: 6; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; text: element.layerSettings.name || element.layerName; color: appearance.colors.accentText; font.pixelSize: 11 }
        }
        Repeater {
            model: [[0,0,"nw"],[.5,0,"n"],[1,0,"ne"],[1,.5,"e"],[1,1,"se"],[.5,1,"s"],[0,1,"sw"],[0,.5,"w"]]
            delegate: Rectangle {
                id: grip
                required property var modelData
                objectName: "canvasHandle-" + element.layerId + "-" + modelData[2]
                x: chrome.width*modelData[0]-5; y: chrome.height*modelData[1]-5
                width: 10; height: 10; radius: 2; color: appearance.colors.panel
                border.color: appearance.colors.accent; border.width: 1.5
                MouseArea {
                    anchors.fill: parent; anchors.margins: -4; enabled: element.editable
                    cursorShape: grip.modelData[2] === "n" || grip.modelData[2] === "s" ? Qt.SizeVerCursor
                               : grip.modelData[2] === "e" || grip.modelData[2] === "w" ? Qt.SizeHorCursor
                               : grip.modelData[2] === "ne" || grip.modelData[2] === "sw" ? Qt.SizeBDiagCursor : Qt.SizeFDiagCursor
                    property point origin
                    property rect startRect
                    property real startAngle
                    onPressed: mouse => {
                        origin = mapToItem(element.sceneCanvas, mouse.x, mouse.y)
                        startRect = Qt.rect(element.x, element.y, element.width, element.height)
                        startAngle = element.angle * Math.PI / 180
                        element.layerModel.beginEdit()
                    }
                    onPositionChanged: mouse => {
                        if (!pressed) return
                        const p = mapToItem(element.sceneCanvas, mouse.x, mouse.y)
                        const cs = Math.cos(startAngle), sn = Math.sin(startAngle)
                        const dx = cs*(p.x-origin.x)+sn*(p.y-origin.y), dy=-sn*(p.x-origin.x)+cs*(p.y-origin.y)
                        const h = grip.modelData[2], left = h.indexOf("w")>=0, right=h.indexOf("e")>=0
                        const top=h.indexOf("n")>=0, bottom=h.indexOf("s")>=0
                        let w=Math.max(12,startRect.width+(right?dx:left?-dx:0)), ht=Math.max(12,startRect.height+(bottom?dy:top?-dy:0))
                        if (element.layerSettings.aspectLocked !== false && !(mouse.modifiers & Qt.AltModifier)) {
                            if ((left||right) && (!top&&!bottom || Math.abs(w/startRect.width-1)>Math.abs(ht/startRect.height-1))) ht=w*startRect.height/startRect.width
                            else w=ht*startRect.width/startRect.height
                        }
                        const l=left?startRect.width-w:(!right?(startRect.width-w)/2:0)
                        const t=top?startRect.height-ht:(!bottom?(startRect.height-ht)/2:0)
                        const ax=element.anchorX*(startRect.width-w), ay=element.anchorY*(startRect.height-ht)
                        const nx=startRect.x+cs*l-sn*t+ax-(cs*ax-sn*ay)
                        const ny=startRect.y+sn*l+cs*t+ay-(sn*ax+cs*ay)
                        element.layerModel.edit(element.layerId,{x:nx/element.sceneCanvas.width,y:ny/element.sceneCanvas.height,width:w/element.sceneCanvas.width,height:ht/element.sceneCanvas.height})
                    }
                    onReleased: element.layerModel.endEdit()
                    onCanceled: element.layerModel.endEdit(true)
                }
            }
        }
        Rectangle { x: parent.width/2; y: -43; width: 1; height: 21; color: appearance.colors.accent }
        Rectangle {
            objectName: "canvasRotate-" + element.layerId
            x: parent.width/2-6; y:-49; width:12; height:12; radius:6; color:appearance.colors.panel; border.color:appearance.colors.accent
            MouseArea {
                anchors.fill: parent; anchors.margins:-5; enabled:element.editable; cursorShape:Qt.CrossCursor
                property real initialAngle
                property real pointerAngle
                property point pivot
                onPressed: mouse => {
                    pivot=Qt.point(element.x+element.width*element.anchorX,element.y+element.height*element.anchorY)
                    const p=mapToItem(element.sceneCanvas,mouse.x,mouse.y)
                    pointerAngle=Math.atan2(p.y-pivot.y,p.x-pivot.x); initialAngle=element.angle
                    element.layerModel.beginEdit()
                }
                onPositionChanged: mouse => {
                    if(!pressed)return;const p=mapToItem(element.sceneCanvas,mouse.x,mouse.y)
                    let angle=initialAngle+(Math.atan2(p.y-pivot.y,p.x-pivot.x)-pointerAngle)*180/Math.PI
                    if(mouse.modifiers&Qt.ShiftModifier)angle=Math.round(angle/15)*15
                    element.layerModel.edit(element.layerId,{rotation:angle})
                }
                onReleased:element.layerModel.endEdit()
                onCanceled:element.layerModel.endEdit(true)
                onDoubleClicked:element.layerModel.edit(element.layerId,{rotation:0})
            }
        }
    }
}
