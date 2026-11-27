import QtQuick

Item {
    id: scene
    required property var controller
    property bool showText: true
    signal selected(string id)

    Repeater {
        model: scene.controller.sceneLayers

        delegate: Item {
            id: layer
            required property var modelData
            readonly property bool picture: modelData.kind === "images"
            readonly property real xRange: Math.max(0, scene.width - width)
            readonly property real yRange: Math.max(0, scene.height - height)
            property real dragX: 0
            property real dragY: 0
            x: Math.max(0, Math.min(xRange, xRange * modelData.horizontal + dragX))
            y: Math.max(0, Math.min(yRange, yRange * modelData.vertical + dragY))
            width: picture ? image.width : Math.min(scene.width, caption.implicitWidth)
            height: picture ? image.height : caption.implicitHeight
            visible: scene.controller.position >= modelData.start
                     && scene.controller.position < modelData.end
                     && (picture ? image.status === Image.Ready : scene.showText)

            Rectangle {
                anchors.fill: parent
                radius: 5
                color: "#182337"
                visible: !layer.modelData.transparent
            }

            Image {
                id: image
                source: layer.picture ? layer.modelData.url : ""
                asynchronous: true
                width: scene.width * layer.modelData.width
                height: sourceSize.width > 0 ? Math.min(scene.height, width * sourceSize.height / sourceSize.width) : 0
                fillMode: Image.PreserveAspectFit
                visible: layer.picture
            }

            Text {
                id: caption
                anchors.fill: parent
                leftPadding: 7
                rightPadding: 7
                topPadding: 6
                bottomPadding: 6
                text: layer.picture ? "" : layer.modelData.text
                textFormat: Text.PlainText
                color: layer.modelData.color
                font.pixelSize: Math.max(10, layer.modelData.size * scene.height / 900)
                font.bold: true
                style: Text.Outline
                styleColor: "#141c2a"
                wrapMode: Text.Wrap
                visible: !layer.picture
            }

            MouseArea {
                anchors.fill: parent
                enabled: !scene.controller.loading
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property point origin
                onPressed: (mouse) => {
                    scene.selected(layer.modelData.id)
                    scene.controller.pause()
                    origin = mapToItem(scene, mouse.x, mouse.y)
                }
                onPositionChanged: (mouse) => {
                    if (pressed) {
                        const at = mapToItem(scene, mouse.x, mouse.y)
                        layer.dragX = at.x - origin.x
                        layer.dragY = at.y - origin.y
                    }
                }
                onReleased: {
                    if (Math.abs(layer.dragX) + Math.abs(layer.dragY) < 3) {
                        layer.dragX = 0
                        layer.dragY = 0
                        return
                    }
                    const position = [layer.xRange ? layer.x / layer.xRange : layer.modelData.horizontal,
                                      layer.yRange ? layer.y / layer.yRange : layer.modelData.vertical]
                    layer.dragX = 0
                    layer.dragY = 0
                    scene.controller.editSceneLayer(layer.modelData.id, {position: position})
                }
                onCanceled: {
                    layer.dragX = 0
                    layer.dragY = 0
                }
            }
        }
    }
}
