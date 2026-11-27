import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: timeline
    required property var controller
    required property real labelWidth
    required property color accent
    property real selectedTime: -1
    readonly property var currentMarker: controller.markers.find(marker => marker.time === selectedTime) ?? null
    spacing: 4

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        StudioButton {
            Layout.preferredWidth: timeline.labelWidth
            Layout.preferredHeight: 24
            text: "添加标记"
            enabled: !timeline.controller.loading && timeline.controller.duration > 0
            onClicked: {
                timeline.controller.pause()
                timeline.selectedTime = timeline.controller.position
                timeline.controller.addMarker()
            }
        }

        Rectangle {
            id: lane
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            color: appearance.colors.timelineTrack
            radius: 4

            Repeater {
                model: timeline.controller.markers
                delegate: MouseArea {
                    id: pin
                    required property var modelData
                    readonly property bool active: modelData.time === timeline.selectedTime
                    width: 16
                    height: lane.height
                    x: Math.max(0, Math.min(lane.width - width,
                        modelData.time / Math.max(.000001, timeline.controller.duration) * lane.width - width / 2))
                    visible: modelData.time >= 0 && modelData.time <= timeline.controller.duration
                    enabled: !timeline.controller.loading
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    z: active ? 1 : 0
                    Rectangle {
                        anchors.centerIn: parent
                        width: 8
                        height: 8
                        rotation: 45
                        color: pin.active ? timeline.accent : appearance.colors.imageClip
                    }
                    StudioToolTip { visible: parent.containsMouse; text: modelData.label + " · " + modelData.time.toFixed(3) + " s" }
                    onClicked: {
                        timeline.selectedTime = modelData.time
                        timeline.controller.pause()
                        timeline.controller.seek(modelData.time)
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: timeline.currentMarker !== null
        enabled: !timeline.controller.loading

        StudioTextField {
            id: nameEntry
            Layout.fillWidth: true
            text: timeline.currentMarker?.label ?? ""
            onAccepted: if (timeline.currentMarker) timeline.controller.renameMarker(timeline.selectedTime, text)
        }
        StudioButton {
            text: "重命名"
            onClicked: timeline.controller.renameMarker(timeline.selectedTime, nameEntry.text)
        }
        StudioButton {
            text: "删除标记"
            onClicked: {
                timeline.controller.removeMarker(timeline.selectedTime)
                timeline.selectedTime = -1
            }
        }
        StudioButton {
            text: "收起"
            onClicked: timeline.selectedTime = -1
        }
    }
}
