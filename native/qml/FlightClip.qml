import QtQuick
import QtMultimedia

Item {
    id: frame
    required property var controller
    required property var footage
    required property int clipIndex
    required property var layout
    required property bool videoOnly
    required property bool cloudOnly
    property bool exporting: false
    property bool included: true
    property var exportFrame: null
    property real targetSeconds: -1
    property real lastSeekAt: -1000
    property real requestedPosition: -1
    property bool wasPlaying: false
    property bool producedFrame: false
    readonly property real cropWidth: footage.crop[2] - footage.crop[0]
    readonly property real cropHeight: footage.crop[3] - footage.crop[1]
    readonly property real sourceAspect: exporting && exportFrame && exportFrame.height > 0
                                         ? exportFrame.width / exportFrame.height : output.sourceRect.height > 0
                                         ? output.sourceRect.width / output.sourceRect.height : 16 / 9
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
    readonly property real targetAspect: width / Math.max(1, height)
    readonly property real displayedCropWidth: cropWidth * Math.min(1, targetAspect / cropAspect)
    readonly property real displayedCropHeight: cropHeight * Math.min(1, cropAspect / Math.max(.000001, targetAspect))

    function between(a, b) { return a + (b - a) * blend }

    z: backdrop || (inset && blend === 0) ? 0 : 2
    readonly property rect geometry: {
        if (full)
            return Qt.rect(0, 0, parent.width, parent.height)
        const base = backdrop ? Qt.rect(largeX, largeY, largeWidth, largeHeight)
            : Qt.rect(between(largeX, insetX), between(largeY, insetY),
                      between(largeWidth, insetWidth), between(largeHeight, insetHeight))
        return controller.layerMotions.length
            ? controller.animatedLayer("video", footage.id, controller.position, base, Qt.size(parent.width, parent.height))
            : base
    }
    x: geometry.x
    y: geometry.y
    width: geometry.width
    height: geometry.height
    visible: included && !cloudOnly && (!videoOnly || targetVideo) &&
             (exporting ? (exportFrame && exportFrame.width > 0) :
                      targetSeconds >= 0 && player.hasVideo && (player.duration <= 0 || targetSeconds * 1000 < player.duration))
    opacity: full ? 1 : footage.opacity
    clip: true

    function seekTo(target) {
        if (!player.seekable || target < 0 || target === requestedPosition)
            return
        requestedPosition = target
        lastSeekAt = Date.now()
        if (Math.abs(player.position - target) > 12) {
            producedFrame = false
            player.position = target
        }
    }

    function synchronize(force) {
        if (exporting) {
            pausedSeek.stop()
            if (player.playing)
                player.pause()
            return
        }
        const wasActive = targetSeconds >= 0 && (player.duration <= 0 || targetSeconds * 1000 < player.duration)
        targetSeconds = controller.videoSeconds(clipIndex)
        const target = Math.round(targetSeconds * 1000)
        const active = targetSeconds >= 0 && (player.duration <= 0 || target < player.duration)
        const run = active && controller.playing
        if (!active) {
            pausedSeek.stop()
            requestedPosition = -1
        } else if (run) {
            pausedSeek.stop()
            const drift = Math.abs(player.position - target)
            if (!wasPlaying || !wasActive || requestedPosition < 0
                    || (producedFrame && drift > 240 && Date.now() - lastSeekAt > 750))
                seekTo(target)
        } else if (force || target !== requestedPosition) {
            // Coalesce scrubs and media-load notifications before restarting a decoder.
            pausedSeek.restart()
        }

        wasPlaying = run
        if (run && !player.playing)
            player.play()
        else if (!run && player.playing)
            player.pause()
    }

    Timer {
        id: pausedSeek
        interval: 50
        onTriggered: frame.seekTo(Math.round(frame.targetSeconds * 1000))
    }

    Connections {
        target: frame.controller
        function onPositionChanged() { frame.synchronize(false) }
        function onPlayingChanged() { frame.synchronize(false) }
    }
    Connections {
        target: output.videoSink
        function onVideoFrameChanged() {
            if (output.videoSink.videoSize.width > 0)
                frame.producedFrame = true
        }
    }
    onFootageChanged: synchronize(false)
    onExportingChanged: synchronize(true)

    MediaPlayer {
        id: player
        source: frame.footage.url
        onSourceChanged: {
            frame.requestedPosition = -1
            frame.producedFrame = false
            frame.lastSeekAt = -1000
        }
        playbackRate: frame.footage.rate
        videoOutput: output
        onSeekableChanged: if (player.seekable) frame.synchronize(true)
        onDurationChanged: {
            frame.controller.setVideoDuration(frame.clipIndex, frame.footage.url, player.duration / 1000)
            frame.synchronize(false)
        }
        onMediaStatusChanged: if (player.mediaStatus === MediaPlayer.LoadedMedia) frame.synchronize(true)
        Component.onCompleted: frame.synchronize(true)
    }
    VideoOutput {
        id: output
        objectName: "flightClipOutput"
        visible: !frame.exporting
        x: -(frame.footage.crop[0] + (frame.cropWidth - frame.displayedCropWidth) / 2) * width
        y: -(frame.footage.crop[1] + (frame.cropHeight - frame.displayedCropHeight) / 2) * height
        width: frame.width / Math.max(.000001, frame.displayedCropWidth)
        height: frame.height / Math.max(.000001, frame.displayedCropHeight)
        fillMode: VideoOutput.Stretch
    }
    Image {
        visible: frame.exporting
        source: frame.exporting && frame.exportFrame ? frame.exportFrame.url : ""
        x: output.x
        y: output.y
        width: output.width
        height: output.height
        fillMode: Image.Stretch
        cache: false
        asynchronous: false
    }

}
