import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: timeline
    required property var controller
    required property int selectedIndex
    required property color ink
    required property color accent
    signal selected(int index)

    readonly property real duration: controller.duration
    readonly property real labelWidth: 124
    spacing: 4

    ScrollView {
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
                model: timeline.controller.videoLayers

                delegate: RowLayout {
                    id: row
                    required property int index
                    required property var modelData
                    width: tracks.width
                    spacing: 4

                    Button {
                        Layout.preferredWidth: timeline.labelWidth
                        Layout.preferredHeight: 24
                        text: row.modelData.label
                        highlighted: timeline.selectedIndex === row.index
                        id: trackButton
                        contentItem: Label {
                            text: trackButton.text
                            color: timeline.ink
                            elide: Text.ElideMiddle
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: timeline.selected(row.index)
                    }

                    Rectangle {
                        id: lane
                        Layout.fillWidth: true
                        Layout.preferredHeight: 24
                        radius: 4
                        color: "#202d40"

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
                                timeline.selected(row.index)
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
                                color: timeline.selectedIndex === row.index ? "#33668a" : "#294a61"
                                border.color: timeline.accent
                                border.width: timeline.selectedIndex === row.index ? 1 : 0

                                Menu {
                                    id: actions
                                    property real at
                                    MenuItem { text: "在此分割"; onTriggered: timeline.controller.splitVideo(row.index, actions.at) }
                                    MenuItem { text: "删除此片段"; onTriggered: timeline.controller.deleteVideoSegment(row.index, actions.at) }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    enabled: !timeline.controller.loading
                                    property int edge: 0
                                    property real pressX: 0
                                    property real start: 0
                                    property real end: 0
                                    property bool moved: false

                                    onPressed: (mouse) => {
                                        timeline.selected(row.index)
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
                                        timeline.controller.trimVideoSegment(row.index, clip.modelData.part, first, last)
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
