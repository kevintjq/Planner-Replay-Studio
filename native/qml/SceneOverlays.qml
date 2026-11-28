import QtQuick

Item {
    id: scene
    required property var controller
    property bool interactive: true
    property bool showText: true
    property bool includeText: true
    property var captureLayerIds: null
    signal selected(string id)

    Repeater {
        parent: scene.parent
        model: scene.controller.sceneLayers.length

        delegate: CanvasElement {
            id: sceneLayer
            required property int index
            readonly property var modelData: scene.controller.sceneLayers[index] || ({})
            parent: scene.parent
            sceneCanvas: scene.parent
            controller: scene.controller
            layerId: modelData.id
            layerName: modelData.label
            interactive: scene.interactive
            baseZ: 400 + index
            readonly property bool picture: modelData.kind === "images"
            readonly property real naturalWidth: picture ? scene.width*modelData.width : Math.min(scene.width,measure.advanceWidth+14)
            readonly property real naturalHeight: picture ? (image.sourceSize.width>0?naturalWidth*image.sourceSize.height/image.sourceSize.width:0) : measure.height+12
            baseGeometry: {
                const base=Qt.rect((scene.width-naturalWidth)*modelData.horizontal,(scene.height-naturalHeight)*modelData.vertical,naturalWidth,naturalHeight)
                return scene.controller.layerMotions.length ? scene.controller.animatedLayer(picture?"image":"text",modelData.id,scene.controller.position,base,Qt.size(scene.width,scene.height)) : base
            }
            contentAllowed: (scene.captureLayerIds===null || scene.captureLayerIds.indexOf(modelData.id)>=0)
                && (picture || scene.includeText) && scene.controller.position>=modelData.start
                && scene.controller.position<modelData.end && (picture?image.status===Image.Ready:scene.showText)
            TextMetrics { id:measure; text:sceneLayer.picture?"":sceneLayer.modelData.text; font.pixelSize:Math.max(1,sceneLayer.modelData.size*scene.height/900);font.bold:true }

            Rectangle {
                anchors.fill: parent
                radius: 5
                color: appearance.outputColors().panel
                visible: !sceneLayer.modelData.transparent
            }

            Image {
                id: image
                source: sceneLayer.picture ? sceneLayer.modelData.url : ""
                asynchronous: scene.interactive
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                visible: sceneLayer.picture
            }

            Text {
                id: caption
                anchors.fill: parent
                leftPadding: 7
                rightPadding: 7
                topPadding: 6
                bottomPadding: 6
                text: sceneLayer.picture ? "" : sceneLayer.modelData.text
                textFormat: Text.PlainText
                color: sceneLayer.modelData.color
                font.pixelSize: Math.max(1, sceneLayer.modelData.size * scene.height / 900 * Math.min(sceneLayer.width/Math.max(1,sceneLayer.naturalWidth),sceneLayer.height/Math.max(1,sceneLayer.naturalHeight)))
                font.bold: true
                style: Text.Outline
                styleColor: appearance.outputColors().textOutline
                wrapMode: Text.Wrap
                visible: !sceneLayer.picture
            }

        }
    }
}
