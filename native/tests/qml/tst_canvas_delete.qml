import QtQuick
import QtQuick.Controls
import QtTest
import "../../qml"

Item {
    id: fixture
    width: 760
    height: 600
    property int workspaceIndex: 2
    property bool editingAllowed: true
    readonly property var activeCanvas: workspaceIndex === 3 ? trajectoryCanvas : videoCanvas
    property int playbackClicks: 0

    QtObject {
        id: videoCanvas
        property var selection: []
        property int removed: 0
        function removeSelected() { ++removed; selection = [] }
    }
    QtObject {
        id: trajectoryCanvas
        property var selection: []
        property int removed: 0
        function removeSelected() { ++removed; selection = [] }
    }
    QtObject { id: canvasController; property var canvas: fixture.activeCanvas }
    // Routing fixture only. These counters certify Qt focus/key dispatch,
    // not media decoding, interval storage or a full application workflow.
    QtObject {
        id: trackCanvas
        property int revision: 0
        property int removedIntervals: 0
        function settings(id) { return {} }
        function removeInterval(id, first, last) { ++removedIntervals }
    }
    QtObject {
        id: timelineController
        property bool loading: false
        property real duration: 10
        property real position: 0
        property var canvas: trackCanvas
        property var sceneLayers: []
        property var videoLayers: [{label: "Video", segments: [{first: 1, last: 5, part: 0, leftEdge: true, rightEdge: true}]}]
        property int removedClips: 0
        function pause() {}
        function seek(time) { position = time }
        function deleteVideoSegment(index, at) { ++removedClips }
    }
    CanvasDeleteShortcut {
        id: deleteRoute
        active: fixture.editingAllowed
        focusItem: fixture.Window.window ? fixture.Window.window.activeFocusItem : null
        timelineItem: fixture.workspaceIndex === 2 ? timeline : null
        selectionCount: fixture.activeCanvas.selection.length
        onDeleteCanvasSelection: fixture.activeCanvas.removeSelected()
        onDeleteTimelineSelection: timeline.removeSelectedClip()
    }
    EditableCanvas {
        id: canvasSurface
        x: 10; y: 10; width: 240; height: 130
        controller: canvasController
        MouseArea {
            anchors.fill: parent
            onClicked: { canvasSurface.forceActiveFocus(); fixture.activeCanvas.selection = ["layer-copy"] }
        }
    }
    StudioButton { id: play; x: 270; y: 10; text: "Play"; onClicked: ++fixture.playbackClicks }
    StudioButton { id: propertyButton; x: 270; y: 55; text: "Layout" }
    TextInput { id: input; x: 410; y: 10; width: 200; height: 35; text: "abc" }
    TextEdit { id: edit; x: 410; y: 55; width: 200; height: 35; text: "abc" }
    TextField { id: field; x: 410; y: 100; width: 200; text: "abc" }
    TextArea { id: area; x: 410; y: 145; width: 200; height: 50; text: "abc" }
    VideoTimeline {
        id: timeline
        x: 10; y: 230; width: 700
        controller: timelineController
        selectedIndex: 0
        selectedLayer: ""
        ink: appearance.colors.text
        accent: appearance.colors.accent
    }

    TestCase {
        name: "CanvasDeleteFocus"
        when: windowShown
        function init() {
            fixture.workspaceIndex = 2
            fixture.editingAllowed = true
            fixture.playbackClicks = 0
            videoCanvas.selection = []
            trajectoryCanvas.selection = []
            videoCanvas.removed = 0
            trajectoryCanvas.removed = 0
            timelineController.loading = false
            timelineController.removedClips = 0
            trackCanvas.removedIntervals = 0
            timeline.selectedClip = null
            for (const control of [input, edit, field, area]) control.text = "abc"
            canvasSurface.forceActiveFocus()
        }
        function selectCanvas(workspace) {
            fixture.workspaceIndex = workspace
            mouseClick(canvasSurface, 30, 20)
            tryCompare(canvasSurface, "activeFocus", true)
            compare(fixture.activeCanvas.selection, ["layer-copy"])
        }
        function test_buttonFocusKeepsCanvasDeletion_data() {
            return [
                {tag: "video-play", workspace: 2, button: play},
                {tag: "video-property", workspace: 2, button: propertyButton},
                {tag: "trajectory-play", workspace: 3, button: play},
                {tag: "trajectory-property", workspace: 3, button: propertyButton}
            ]
        }
        function test_buttonFocusKeepsCanvasDeletion(data) {
            selectCanvas(data.workspace)
            mouseClick(data.button)
            tryCompare(data.button, "activeFocus", true)
            keyClick(Qt.Key_Delete)
            compare(fixture.activeCanvas.removed, 1)
            compare(fixture.activeCanvas.selection, [])
            compare(data.workspace === 2 ? trajectoryCanvas.removed : videoCanvas.removed, 0)
            keyClick(Qt.Key_Delete)
            compare(fixture.activeCanvas.removed, 1)
        }
        function test_canvasKeysAndShortcutDeleteOnce_data() {
            return [{tag: "video", workspace: 2}, {tag: "trajectory", workspace: 3}]
        }
        function test_canvasKeysAndShortcutDeleteOnce(data) {
            selectCanvas(data.workspace)
            keyClick(Qt.Key_Delete)
            compare(fixture.activeCanvas.removed, 1)
            compare(fixture.activeCanvas.selection, [])
        }
        function test_textEditorOwnsDelete_data() {
            const rows = []
            for (const workspace of [2, 3]) {
                for (const entry of [{name: "TextInput", item: input}, {name: "TextEdit", item: edit},
                                     {name: "TextField", item: field}, {name: "TextArea", item: area}]) {
                    rows.push({tag: workspace + "-" + entry.name, workspace: workspace, editor: entry.item})
                }
            }
            return rows
        }
        function test_textEditorOwnsDelete(data) {
            selectCanvas(data.workspace)
            data.editor.forceActiveFocus()
            data.editor.cursorPosition = 1
            tryCompare(deleteRoute, "textEditing", true)
            keyClick(Qt.Key_Delete)
            compare(data.editor.text, "ac")
            compare(fixture.activeCanvas.removed, 0)
            compare(fixture.activeCanvas.selection, ["layer-copy"])
        }
        function test_timelineClipAndEmptySelectionNeverDeleteCanvas() {
            selectCanvas(2)
            const clip = findChild(timeline, "videoTimelineClipInput")
            verify(clip !== null)
            mouseClick(clip, clip.width / 2, clip.height / 2)
            tryCompare(deleteRoute, "timelineFocused", true)
            verify(timeline.selectedClip !== null)
            keyClick(Qt.Key_Delete)
            compare(timelineController.removedClips, 1)
            compare(timeline.selectedClip, null)
            compare(videoCanvas.removed, 0)
            keyClick(Qt.Key_Delete)
            compare(timelineController.removedClips, 1)
            compare(videoCanvas.selection, ["layer-copy"])
            compare(videoCanvas.removed, 0)
        }
        function test_timelineChildFocusKeepsClipPriority() {
            selectCanvas(2)
            timeline.selectedClip = {videoIndex: 0, at: 2}
            const button = findChild(timeline, "videoTimelineTrackButton")
            verify(button !== null)
            mouseClick(button)
            tryCompare(button, "activeFocus", true)
            tryCompare(deleteRoute, "timelineFocused", true)
            keyClick(Qt.Key_Delete)
            compare(timelineController.removedClips, 1)
            compare(videoCanvas.removed, 0)
        }
        function test_timelineSceneClipDeletesOnlyInterval() {
            selectCanvas(2)
            timeline.selectedClip = {layerId: "text", first: 1, last: 3}
            timeline.forceActiveFocus()
            keyClick(Qt.Key_Delete)
            compare(trackCanvas.removedIntervals, 1)
            compare(timelineController.removedClips, 0)
            compare(videoCanvas.removed, 0)
        }
        function test_blockedEditingDoesNotDelete() {
            selectCanvas(2)
            fixture.editingAllowed = false
            mouseClick(play)
            keyClick(Qt.Key_Delete)
            compare(videoCanvas.removed, 0)
            compare(videoCanvas.selection, ["layer-copy"])
            fixture.editingAllowed = true
            timeline.selectedClip = {videoIndex: 0, at: 2}
            timelineController.loading = true
            timeline.forceActiveFocus()
            keyClick(Qt.Key_Delete)
            compare(timelineController.removedClips, 0)
            compare(videoCanvas.removed, 0)
        }
    }
}
