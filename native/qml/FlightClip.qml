import QtQuick
import UAVStudio 1.0

CanvasElement {
    id: frame
    layerId: footage.id
    layerName: footage.label
    required property var footage
    required property int clipIndex
    required property var layout
    required property bool videoOnly
    required property bool cloudOnly
    property bool exporting: false
    interactive: !exporting && (sceneCanvas.interactive ?? true)
    property bool included: true
    property var exportFrame: null
    property real targetSeconds: -1
    readonly property var crop: layerSettings.crop || footage.crop
    readonly property real cropWidth: Math.max(.0001, crop[2] - crop[0])
    readonly property real cropHeight: Math.max(.0001, crop[3] - crop[1])
    readonly property real sourceAspect: exporting && exportFrame && exportFrame.height > 0
                                         ? exportFrame.width / exportFrame.height : output.imageSize.height > 0
                                         ? output.imageSize.width / output.imageSize.height : 16 / 9
    readonly property real cropAspect: sourceAspect * cropWidth / cropHeight

    readonly property bool targetVideo: footage.id === layout.video_id
    readonly property bool full: videoOnly && targetVideo
    readonly property bool backdrop: !videoOnly && !cloudOnly && footage.id === layout.backdropId
    readonly property bool inset: layout.mode === "inset" && targetVideo
    readonly property real blend: inset ? layout.progress : 1
    readonly property real insetWidth: Math.min(parent.width * (inset ? (layout.width ?? footage.width) : footage.width),
                                               parent.height * .9 * cropAspect)
    readonly property real insetHeight: insetWidth / cropAspect
    readonly property real insetX: (parent.width - insetWidth) * (inset ? (layout.position?.[0] ?? footage.horizontal) : footage.horizontal)
    readonly property real insetY: (parent.height - insetHeight) * (inset ? (layout.position?.[1] ?? footage.vertical) : footage.vertical)
    readonly property real largeWidth: parent.width * footage.backdropScale
    readonly property real largeHeight: parent.height * footage.backdropScale
    readonly property real largeX: (parent.width - largeWidth) * footage.backdropX
    readonly property real largeY: (parent.height - largeHeight) * footage.backdropY
    readonly property real sourceWidth: exporting && exportFrame?.width > 0 ? exportFrame.width : Math.max(1, output.imageSize.width)
    readonly property real sourceHeight: exporting && exportFrame?.height > 0 ? exportFrame.height : Math.max(1, output.imageSize.height)
    readonly property string fitMode: layerSettings.fit || "fit"
    readonly property real fittedScale: fitMode === "original" ? sceneCanvas.width / layerModel.outputWidth
        : fitMode === "fill" ? Math.max(width / (sourceWidth * cropWidth), height / (sourceHeight * cropHeight))
        : Math.min(width / (sourceWidth * cropWidth), height / (sourceHeight * cropHeight))
    readonly property real imageWidth: fitMode === "stretch" ? width / cropWidth : sourceWidth * fittedScale
    readonly property real imageHeight: fitMode === "stretch" ? height / cropHeight : sourceHeight * fittedScale

    function between(a, b) { return a + (b - a) * blend }

    baseZ: backdrop || (inset && blend === 0) ? 0 : 200 + clipIndex
    baseGeometry: {
        if (full)
            return Qt.rect(0, 0, parent.width, parent.height)
        const base = backdrop ? Qt.rect(largeX, largeY, largeWidth, largeHeight)
            : Qt.rect(between(largeX, insetX), between(largeY, insetY),
                      between(largeWidth, insetWidth), between(largeHeight, insetHeight))
        return controller.layerMotions.length
            ? controller.animatedLayer("video", footage.id, controller.position, base, Qt.size(parent.width, parent.height))
            : base
    }
    contentAllowed: included && !cloudOnly && (!videoOnly || targetVideo) &&
             (exporting ? (exportFrame && exportFrame.width > 0) :
                      targetSeconds >= 0 && (output.ready || output.error.length > 0) && (output.duration <= 0 || targetSeconds < output.duration))
    baseOpacity: full ? 1 : footage.opacity

    function synchronize() { if (controller) targetSeconds = controller.videoSeconds(clipIndex) }
    Connections {
        target: frame.controller
        function onPositionChanged() { frame.synchronize() }
        function onPlayingChanged() { frame.synchronize() }
    }
    onFootageChanged: synchronize()
    onClipIndexChanged: synchronize()
    Component.onCompleted: synchronize()

    ClipPreview {
        id: output
        objectName: "flightClipOutput"
        source: frame.footage.url
        sourceSeconds: Math.max(0, frame.targetSeconds)
        decodingEnabled: !frame.exporting && frame.targetSeconds >= 0 && frame.included && !frame.cloudOnly && frame.layerModel.active(frame.layerId,frame.controller.position)
        precise: !frame.controller.playing
        visible: !frame.exporting
        onDurationChanged: if (duration > 0) frame.controller.setVideoDuration(frame.clipIndex, frame.footage.url, duration)
        x: (frame.width - frame.imageWidth * frame.cropWidth) / 2 - frame.crop[0] * frame.imageWidth
        y: (frame.height - frame.imageHeight * frame.cropHeight) / 2 - frame.crop[1] * frame.imageHeight
        width: frame.imageWidth
        height: frame.imageHeight
    }
    Image {
        visible: frame.exporting
        source: frame.exporting && frame.exportFrame ? frame.exportFrame.url : ""
        x: output.x; y: output.y; width: output.width; height: output.height
        fillMode: Image.Stretch
        cache: false
        asynchronous: false
    }
    StudioToolTip {
        text: output.error
        visible: output.error.length > 0 && errorHover.hovered
    }
    HoverHandler { id: errorHover }
    Rectangle {
        anchors.fill: parent
        visible: !frame.exporting && output.error.length > 0 && !output.ready
        color: appearance.colors.panel
        StudioLabel { anchors.fill: parent; anchors.margins: 12; text: output.error; wrapMode: Text.Wrap; color: appearance.colors.text }
    }
}
