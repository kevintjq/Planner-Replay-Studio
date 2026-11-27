import QtQuick
import QtMultimedia

Item {
    id: frame
    required property var controller
    required property var footage
    required property int clipIndex
    property real targetSeconds: -1
    property real lastSeekAt: -1000
    property real requestedPosition: -1
    property bool wasPlaying: false
    readonly property real cropWidth: footage.crop[2] - footage.crop[0]
    readonly property real cropHeight: footage.crop[3] - footage.crop[1]
    readonly property real sourceAspect: output.sourceRect.height > 0
                                         ? output.sourceRect.width / output.sourceRect.height : 16 / 9
    readonly property real cropAspect: sourceAspect * cropWidth / cropHeight

    x: (parent.width - width) * footage.horizontal
    y: (parent.height - height) * footage.vertical
    width: Math.min(parent.width * footage.width, parent.height * .9 * cropAspect)
    height: width / cropAspect
    visible: targetSeconds >= 0 && player.hasVideo
             && (player.duration <= 0 || targetSeconds * 1000 < player.duration)
    opacity: footage.opacity
    clip: true

    function synchronize(force) {
        targetSeconds = controller.videoSeconds(clipIndex)
        const target = Math.round(targetSeconds * 1000)
        const active = targetSeconds >= 0 && (player.duration <= 0 || target < player.duration)
        const run = active && controller.playing
        if (!active) {
            requestedPosition = -1
        } else {
            const now = Date.now()
            const drift = Math.abs(player.position - target)
            const seek = force || requestedPosition < 0 || run !== wasPlaying
                         || (!run && target !== requestedPosition)
                         || (run && drift > 140 && now - lastSeekAt > 180)
            if (player.seekable && seek) {
                requestedPosition = target
                lastSeekAt = now
                player.position = target
            }
        }
        wasPlaying = run
        if (run && !player.playing)
            player.play()
        else if (!run && player.playing)
            player.pause()
    }

    Connections {
        target: frame.controller
        function onPositionChanged() { frame.synchronize(false) }
        function onPlayingChanged() { frame.synchronize(false) }
    }
    MediaPlayer {
        id: player
        source: frame.footage.url
        playbackRate: frame.footage.rate
        videoOutput: output
        onSeekableChanged: if (seekable) frame.synchronize(true)
        onDurationChanged: frame.synchronize(false)
        onMediaStatusChanged: if (mediaStatus === MediaPlayer.LoadedMedia) frame.synchronize(true)
        Component.onCompleted: frame.synchronize(true)
    }
    VideoOutput {
        id: output
        objectName: "flightClipOutput"
        x: -frame.footage.crop[0] * frame.width / frame.cropWidth
        y: -frame.footage.crop[1] * frame.height / frame.cropHeight
        width: frame.width / frame.cropWidth
        height: frame.height / frame.cropHeight
        fillMode: VideoOutput.Stretch
    }
}
