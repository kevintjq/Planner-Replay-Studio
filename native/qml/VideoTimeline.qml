import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: timeline
    required property var controller
    required property int selectedIndex
    required property string selectedLayer
    required property color ink
    required property color accent
    property var selectedClip: null
    function removeSelectedClip() {
        if (!selectedClip || controller.loading) return
        if (selectedClip.videoIndex !== undefined) controller.deleteVideoSegment(selectedClip.videoIndex, selectedClip.at)
        else controller.canvas.removeInterval(selectedClip.layerId, selectedClip.first, selectedClip.last)
        selectedClip = null
    }
    Keys.onDeletePressed: removeSelectedClip()
    signal selected(int index)
    signal layerSelected(string id)

    readonly property var trackData: { const revision = controller.canvas.revision; return controller.videoLayers.map((video, index) => ({
        videoIndex: index, label: video.label, segments: video.segments
    })).concat(controller.sceneLayers.map(layer => ({
        layerId: layer.id, kind: layer.kind, label: layer.label,
        segments: (controller.canvas.settings(layer.id).visibilityRangesEnabled || (controller.canvas.settings(layer.id).visibilityRanges || []).length)
            ? (controller.canvas.settings(layer.id).visibilityRanges || []).map((r, i) => ({first: Math.max(layer.start,r[0]), last: Math.min(duration,layer.end,r[1]), storedEnd: r[1],leftEdge:true,rightEdge:true,part:i})).filter(r => r.last > r.first)
            : layer.start >= duration ? [] : [{first: layer.start, last: Math.min(duration,layer.end), storedEnd:layer.end,leftEdge:true,rightEdge:layer.end<=duration,part:0}]
    }))) }

    readonly property real duration: controller.duration
    readonly property real labelWidth: 124
    spacing: 4

    StudioScrollView {
        objectName: "videoTracksScroll"
        id: scroller
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(102, tracks.implicitHeight)
        contentWidth: availableWidth
        clip: true

        Column {
            id: tracks
            width: scroller.availableWidth
            spacing: 4

            Repeater {
                model: timeline.trackData

                delegate: RowLayout {
                    id: row
                    required property int index
                    required property var modelData
                    readonly property bool isVideo: modelData.videoIndex !== undefined
                    readonly property bool isSelected: isVideo ? timeline.selectedIndex === modelData.videoIndex
                                                               : timeline.selectedLayer === modelData.layerId
                    width: tracks.width
                    spacing: 4

                    function select() {
                        if (isVideo)
                            timeline.selected(modelData.videoIndex)
                        else
                            timeline.layerSelected(modelData.layerId)
                    }

                    StudioButton {
                        Layout.preferredWidth: timeline.labelWidth
                        Layout.preferredHeight: 24
                        text: row.modelData.label
                        highlighted: row.isSelected
                        id: trackButton
                        objectName: "videoTimelineTrackButton"
                        contentItem: StudioLabel {
                            text: trackButton.text
                            color: timeline.ink
                            elide: Text.ElideMiddle
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: row.select()
                    }

                    Rectangle {
                        id: lane
                        Layout.fillWidth: true
                        Layout.preferredHeight: 24
                        radius: 4
                        color: appearance.colors.timelineTrack

                        function second(x) {
                            return Math.max(0, Math.min(x / Math.max(1, width), 1)) * timeline.duration
                        }
                        function pixel(seconds) {
                            return seconds * width / Math.max(.000001, timeline.duration)
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: !timeline.controller.loading
                            onPressed: (mouse) => {
                                row.select()
                                timeline.controller.pause()
                                timeline.controller.seek(lane.second(mouse.x))
                            }
                            onPositionChanged: (mouse) => {
                                if (pressed)
                                    timeline.controller.seek(lane.second(mouse.x))
                            }
                        }

                        Repeater {
                            model: row.modelData.segments
                            delegate: Rectangle {
                                id: clip
                                required property int index
                                required property var modelData
                                property real first: modelData.first
                                property real last: modelData.last
                                readonly property real lower: index > 0 ? row.modelData.segments[index - 1].last : 0
                                readonly property real upper: index + 1 < row.modelData.segments.length
                                                              ? row.modelData.segments[index + 1].first : timeline.duration
                                x: lane.pixel(first)
                                width: Math.max(0, lane.pixel(last) - x)
                                y: 3
                                height: lane.height - 6
                                radius: 3
                                color: row.isVideo ? (row.isSelected ? appearance.colors.selectedClip : appearance.colors.videoClip)
                                       : row.modelData.kind === "images" ? appearance.colors.imageClip : appearance.colors.textClip
                                border.color: timeline.accent
                                border.width: row.isSelected ? 1 : 0

                                Menu { palette: StudioPalette {}

                                    id: actions
                                    property real at
                                    MenuItem { palette: StudioPalette {}

                                        visible: row.isVideo
                                        text: "在此分割"
                                        onTriggered: timeline.controller.splitVideo(row.modelData.videoIndex, actions.at)
                                    }
                                    MenuItem { palette: StudioPalette {}

                                        text: row.isVideo ? "删除此片段" : "移除图层"
                                        onTriggered: {
                                            if (row.isVideo)
                                                timeline.controller.deleteVideoSegment(row.modelData.videoIndex, actions.at)
                                            else
                                                timeline.controller.canvas.removeInterval(row.modelData.layerId,clip.first,clip.last)
                                        }
                                    }
                                }

                                MouseArea {
                                    objectName: "videoTimelineClipInput"
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    enabled: !timeline.controller.loading
                                    property int edge: 0
                                    property real pressX: 0
                                    property real start: 0
                                    property real end: 0
                                    property bool moved: false

                                    onPressed: (mouse) => {
                                        row.select()
                                        timeline.forceActiveFocus()
                                        timeline.selectedClip = row.isVideo ? {videoIndex:row.modelData.videoIndex,at:(clip.first+clip.last)/2} : {layerId:row.modelData.layerId,first:clip.first,last:clip.last}
                                        timeline.controller.pause()
                                        pressX = mapToItem(lane, mouse.x, 0).x
                                        if (mouse.button === Qt.RightButton) {
                                            actions.at = lane.second(pressX)
                                            actions.popup()
                                            return
                                        }
                                        start = clip.first
                                        end = clip.last
                                        moved = false
                                        edge = mouse.x < Math.min(7, width / 3) && clip.modelData.leftEdge ? -1
                                            : mouse.x > width - Math.min(7, width / 3) && clip.modelData.rightEdge ? 1 : 0
                                        if (!edge)
                                            timeline.controller.seek(lane.second(pressX))
                                    }
                                    onPositionChanged: (mouse) => {
                                        if (!(pressedButtons & Qt.LeftButton))
                                            return
                                        const pixel = mapToItem(lane, mouse.x, 0).x
                                        if (!moved && Math.abs(pixel - pressX) < 4)
                                            return
                                        moved = true
                                        const at = lane.second(pixel)
                                        const minimum = Math.min(1 / 60, end - start)
                                        if (edge < 0)
                                            clip.first = Math.max(clip.lower, Math.min(at, end - minimum))
                                        else if (edge > 0)
                                            clip.last = Math.min(clip.upper, Math.max(at, start + minimum))
                                        else if (clip.modelData.leftEdge && clip.modelData.rightEdge) {
                                            const length = end - start
                                            const proposed = start + at - lane.second(pressX)
                                            clip.first = Math.max(clip.lower, Math.min(proposed, clip.upper - length))
                                            clip.last = clip.first + length
                                        } else {
                                            timeline.controller.seek(at)
                                        }
                                    }
                                    onReleased: (mouse) => {
                                        if (mouse.button !== Qt.LeftButton || !moved)
                                            return
                                        const first = clip.first
                                        const last = clip.last
                                        clip.first = start
                                        clip.last = end
                                        edge = 0
                                        if (row.isVideo) {
                                            timeline.controller.trimVideoSegment(row.modelData.videoIndex, clip.modelData.part, first, last)
                                        } else if (first !== start || last !== end) {
                                            timeline.controller.editSceneLayer(row.modelData.layerId, {
                                                start: first, end: last === end ? clip.modelData.storedEnd : last
                                            })
                                        }
                                    }
                                    onCanceled: {
                                        if (moved) {
                                            clip.first = start
                                            clip.last = end
                                            moved = false
                                        }
                                        edge = 0
                                    }
                                }
                            }
                        }

                        Rectangle {
                            x: Math.min(lane.width - width, lane.pixel(timeline.controller.position))
                            width: 1
                            height: parent.height
                            color: timeline.accent
                        }
                    }
                }
            }
        }
    }
}
