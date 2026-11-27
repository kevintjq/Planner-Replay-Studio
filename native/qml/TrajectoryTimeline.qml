import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: timeline
    required property var editor
    property real cursor: editor.startTime
    property real inTime: editor.startTime
    property real outTime: editor.lastTime
    property real windowStart: editor.startTime
    property real windowSeconds: 10
    readonly property real duration: Math.max(.001, editor.lastTime - editor.startTime)
    readonly property real detailEnd: Math.min(editor.lastTime, windowStart + windowSeconds)
    spacing: 5

    function repaint() { overview.requestPaint(); detail.requestPaint() }
    function focusTime(seconds) {
        cursor = Math.max(editor.startTime, Math.min(editor.lastTime, seconds))
        if (cursor < windowStart || cursor > detailEnd)
            windowStart = Math.max(editor.startTime, cursor - windowSeconds / 2)
        repaint()
    }
    Connections {
        target: timeline.editor
        function onChanged() { timeline.repaint() }
    }
    Connections { target: appearance; function onChanged() { timeline.repaint() } }
    RowLayout {
        StudioLabel { text: "双层时间轴 · " + timeline.editor.sampleCount + " 帧" }
        Item { Layout.fillWidth: true }
        StudioLabel { text: "源时间 " + timeline.cursor.toFixed(3) + " s" }
        StudioButton {
            text: "适应全部"
            onClicked: { timeline.windowStart = timeline.editor.startTime; timeline.windowSeconds = timeline.duration; timeline.repaint() }
        }
    }
    Canvas {
        id: overview
        Layout.fillWidth: true
        Layout.preferredHeight: 38
        onWidthChanged: requestPaint()
        onPaint: {
            const c = getContext("2d")
            c.clearRect(0, 0, width, height)
            c.fillStyle = appearance.colors.timelineTrack; c.fillRect(0, 0, width, height)
            for (const r of timeline.editor.excludedRanges) {
                c.fillStyle = r.parts === 2 ? appearance.colors.lineExcluded : appearance.colors.excluded
                c.fillRect((r.begin - timeline.editor.startTime) / timeline.duration * width, 3,
                           (r.end - r.begin) / timeline.duration * width, height - 6)
            }
            c.fillStyle = appearance.colors.selection
            c.fillRect((timeline.windowStart - timeline.editor.startTime) / timeline.duration * width, 0,
                       Math.min(timeline.windowSeconds, timeline.duration) / timeline.duration * width, height)
            c.strokeStyle = appearance.colors.accent; c.lineWidth = 2
            const x = (timeline.cursor - timeline.editor.startTime) / timeline.duration * width
            c.beginPath(); c.moveTo(x, 0); c.lineTo(x, height); c.stroke()
        }
        MouseArea {
            anchors.fill: parent
            function move(x) {
                const time = timeline.editor.startTime + Math.max(0, Math.min(1, x / width)) * timeline.duration
                timeline.windowStart = Math.max(timeline.editor.startTime, Math.min(timeline.editor.lastTime - timeline.windowSeconds, time - timeline.windowSeconds / 2))
                timeline.cursor = time; timeline.repaint()
            }
            onPressed: mouse => move(mouse.x)
            onPositionChanged: mouse => { if (pressed) move(mouse.x) }
        }
    }
    Canvas {
        id: detail
        Layout.fillWidth: true
        Layout.preferredHeight: 62
        onWidthChanged: requestPaint()
        onPaint: {
            const c = getContext("2d"), span = Math.max(.001, timeline.detailEnd - timeline.windowStart)
            c.clearRect(0, 0, width, height)
            c.fillStyle = appearance.colors.panel; c.fillRect(0, 0, width, height)
            for (const r of timeline.editor.excludedRanges) {
                c.fillStyle = r.parts === 2 ? appearance.colors.lineExcludedFill : appearance.colors.excludedFill
                c.fillRect((r.begin - timeline.windowStart) / span * width, 0, (r.end - r.begin) / span * width, height)
            }
            c.fillStyle = appearance.colors.selection
            c.fillRect((timeline.inTime - timeline.windowStart) / span * width, 0, (timeline.outTime - timeline.inTime) / span * width, height)
            const desired = span / Math.max(1, width / 85)
            const step = Math.max(.01, Math.pow(10, Math.ceil(Math.log10(desired))))
            c.font = "11px sans-serif"; c.fillStyle = appearance.colors.text; c.strokeStyle = appearance.colors.border
            for (let t = Math.ceil(timeline.windowStart / step) * step; t <= timeline.detailEnd; t += step) {
                const x = (t - timeline.windowStart) / span * width
                c.fillText(t.toFixed(step < 1 ? 2 : 1), x + 3, 16)
                c.beginPath(); c.moveTo(x, 22); c.lineTo(x, height); c.stroke()
            }
            c.strokeStyle = appearance.colors.accent; c.lineWidth = 2
            const x = (timeline.cursor - timeline.windowStart) / span * width
            c.beginPath(); c.moveTo(x, 0); c.lineTo(x, height); c.stroke()
        }
        MouseArea {
            anchors.fill: parent
            property real origin: 0
            function timeAt(x) { return timeline.windowStart + Math.max(0, Math.min(1, x / width)) * (timeline.detailEnd - timeline.windowStart) }
            onPressed: mouse => {
                origin = timeAt(mouse.x); timeline.cursor = origin
                timeline.inTime = origin; timeline.outTime = origin; timeline.repaint()
            }
            onPositionChanged: mouse => {
                if (!pressed) return
                const current = timeAt(mouse.x)
                timeline.inTime = Math.min(origin, current); timeline.outTime = Math.max(origin, current)
                timeline.cursor = current; timeline.repaint()
            }
            onWheel: wheel => {
                if (wheel.modifiers & Qt.ControlModifier)
                    timeline.windowSeconds = Math.max(.1, Math.min(timeline.duration, timeline.windowSeconds * (wheel.angleDelta.y > 0 ? .8 : 1.25)))
                else
                    timeline.windowStart = Math.max(timeline.editor.startTime, Math.min(timeline.editor.lastTime - timeline.windowSeconds, timeline.windowStart - wheel.angleDelta.y / 120 * timeline.windowSeconds / 5))
                timeline.repaint(); wheel.accepted = true
            }
        }
    }
    ListView {
        id: filmstrip
        Layout.fillWidth: true
        Layout.preferredHeight: 114
        orientation: ListView.Horizontal
        clip: true
        reuseItems: true
        cacheBuffer: 220
        spacing: 6
        model: timeline.editor.frames
        ScrollBar.horizontal: StudioScrollBar {}
        delegate: Rectangle {
            required property int index
            required property real seconds
            required property bool excluded
            required property bool selected
            required property real frameOpacity
            required property url thumbnail
            width: 126; height: 98
            radius: 5
            color: selected ? appearance.colors.selection : appearance.colors.thumbnail
            border.width: selected ? 2 : 1
            border.color: excluded ? appearance.colors.excluded : selected ? appearance.colors.accent : appearance.colors.border
            Image {
                anchors.top: parent.top; anchors.topMargin: 5
                anchors.horizontalCenter: parent.horizontalCenter
                width: 114; height: 60
                source: thumbnail; sourceSize: Qt.size(160, 100)
                asynchronous: true; cache: false
                fillMode: Image.PreserveAspectFit
                opacity: excluded ? .25 : frameOpacity
            }
            StudioLabel {
                anchors.bottom: parent.bottom; anchors.bottomMargin: 6
                anchors.horizontalCenter: parent.horizontalCenter
                text: seconds.toFixed(3) + (excluded ? " s ×" : " s")
                font.pixelSize: 11
            }
            MouseArea {
                anchors.fill: parent
                onClicked: mouse => {
                    filmstrip.forceActiveFocus(); filmstrip.currentIndex = index
                    timeline.editor.selectFrame(index, Boolean(mouse.modifiers & Qt.ShiftModifier), Boolean(mouse.modifiers & Qt.ControlModifier))
                    timeline.focusTime(seconds)
                }
                onDoubleClicked: timeline.editor.toggleFrame(index)
            }
        }
        Keys.onDeletePressed: timeline.editor.editSelectedFrames(true)
    }
    StudioLabel { text: "拖动精细时间轴选择区间 · Ctrl + 滚轮缩放 · Shift / Ctrl 多选 · 缩略图显示提取后的前景"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
}
