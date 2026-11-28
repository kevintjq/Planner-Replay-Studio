import QtQuick
import QtQuick.Controls
import QtTest
import UAVStudioTest 1.0
import "../../qml"

Item {
    id: fixture
    width: 1100; height: 900
    property string activePanel: ""
    property var canvasSelection: ["old-canvas-layer"]
    property int canvasDeletes: 0

    // Counters verify real Qt focus and production component key forwarding.
    // They do not certify source removal, plugin rendering or frame storage.
    QtObject {
        id: sourceController
        property var sources: [{id: "source-video", name: "Video", path: "input.mp4", kind: "video",
                                referenceCount: 1, references: [], exists: true}]
        property var pendingSourceRemoval: ({})
        property var removalRequests: []
        function removeSources(ids) { removalRequests = removalRequests.concat([ids.slice()]) }
    }
    QtObject {
        id: independentController
        property var independentDisplays: [{id: "grid", name: "abc", class: "rviz/Grid", kind: "grid",
                                           status: "No Data", enabled: true, supported: true}]
        property string selectedIndependentDisplayId: ""
        readonly property var selectedIndependentDisplay: independentDisplays.find(row => row.id === selectedIndependentDisplayId) || ({})
        property var nativeReplaySource: ({bag: "", topics: []})
        property var tfFrames: []
        property string independentFixedFrame: "map"
        property color sceneBackground: "#30343b"
        property bool independentDisplayLoading: false
        property var independentDisplayTypes: []
        property var removalRequests: []
        signal independentDisplaySelectionChanged()
        function selectIndependentDisplay(id) { selectedIndependentDisplayId = id; independentDisplaySelectionChanged() }
        function removeIndependentDisplays(ids) { removalRequests = removalRequests.concat([ids.slice()]) }
        function compatibleIndependentDisplays(type) { return [] }
        function independentDisplayProperties(id) { return [] }
        function editIndependentDisplay(id, patch) {}
    }
    NativeReplayTestState {
        id: backend
        property bool connected: true
        property bool running: true
        property bool busy: false
        property bool indexed: false
        property bool sceneReady: false
        property bool canUndo: false
        property bool canRedo: false
        property var displayTopics: []
        property string targetNs: ""
        property string clockNs: ""
        property string previewTargetNs: ""
        property string preview: ""
        property string status: "Delete key forwarding fixture"
        property string diagnostic: ""
        property string launcher: ""
        property string distribution: ""
        property string selectedId: ""
        property string search: ""
        property var selected: ({})
        property var plugins: []
        property var topics: []
        property var properties: []
        property var removalRequests: []
        signal changed()
        signal modelChanged()
        signal commandFinished(string command, bool success, string error)
        function compatiblePlugins(type) { return [] }
        function requestPreview() {}
        function removeDisplays(ids) { removalRequests = removalRequests.concat([ids.slice()]) }
    }
    QtObject {
        id: layerCanvas
        property var selection: ["local-layer"]
        property var layers: []
        property int deletes: 0
        function removeSelected() { ++deletes; selection = [] }
    }
    QtObject { id: layerController; property var canvas: layerCanvas }
    QtObject {
        id: trajectoryEditor
        property real position: 0
        property real startTime: 0
        property real lastTime: 10
        property int sampleCount: 0
        property var excludedRanges: []
        property var frames: []
        property int deletes: 0
        signal changed()
        function deleteSelectedFrames() { ++deletes }
    }
    CanvasDeleteShortcut {
        id: deleteRoute
        focusItem: fixture.Window.window ? fixture.Window.window.activeFocusItem : null
        selectionCount: fixture.canvasSelection.length
        onDeleteCanvasSelection: { ++fixture.canvasDeletes; fixture.canvasSelection = [] }
    }
    StudioButton { id: outside; x: 10; y: 10; text: "Play" }
    SourceBrowser {
        id: sources; x: 160; width: 360; height: 800
        visible: fixture.activePanel === "sources"
        controller: sourceController
    }
    IndependentDisplays {
        id: independent; x: 160; width: 360; height: 800
        visible: fixture.activePanel === "independent"
        controller: independentController
        showInspector: true
    }
    NativeRvizPanel {
        id: nativePanel; x: 160; width: 360; height: 800
        visible: fixture.activePanel === "native"
        client: backend
        showConnection: false; showPreview: false
    }
    CanvasLayers {
        id: layers; x: 160; width: 360; height: 400
        visible: fixture.activePanel === "layers"
        controller: layerController
    }
    TrajectoryTimeline {
        id: trajectoryTimeline; x: 160; width: 700; height: 350
        visible: fixture.activePanel === "trajectory"
        editor: trajectoryEditor
    }

    TestCase {
        name: "LocalDeleteOwnership"
        when: windowShown
        function init() {
            fixture.activePanel = ""
            fixture.canvasSelection = ["old-canvas-layer"]
            fixture.canvasDeletes = 0
            sources.selectedIds = []
            findChild(sources, "sourceBrowserSearch").text = ""
            sourceController.removalRequests = []
            independentController.selectedIndependentDisplayId = ""
            independentController.removalRequests = []
            independent.selectedIds = []
            backend.removalRequests = []
            backend.selected = ({})
            backend.selectedId = ""
            nativePanel.treeScope = "all"
            nativePanel.displaySelection = []
            layerCanvas.selection = ["local-layer"]
            layerCanvas.deletes = 0
            trajectoryEditor.deletes = 0
            outside.forceActiveFocus()
        }
        function verifyCanvasUnchanged() {
            compare(fixture.canvasDeletes, 0)
            compare(fixture.canvasSelection, ["old-canvas-layer"])
        }
        function focusOwner(panelName, owner, name) {
            fixture.activePanel = panelName
            const child = findChild(owner, name)
            verify(child !== null)
            child.forceActiveFocus()
            tryCompare(child, "activeFocus", true)
            tryCompare(deleteRoute, "localDeleteFocused", true)
            compare(deleteRoute.enabled, false)
            return child
        }
        function test_sourceListDeletesOnlySourceRequest() {
            const list = focusOwner("sources", sources, "sourceBrowserList")
            mouseClick(list, 80, 25)
            compare(sources.selectedIds, ["source-video"])
            keyClick(Qt.Key_Delete)
            compare(sourceController.removalRequests, [["source-video"]])
            verifyCanvasUnchanged()
            sources.selectedIds = []
            keyClick(Qt.Key_Delete)
            compare(sourceController.removalRequests, [["source-video"], []])
            verifyCanvasUnchanged()
        }
        function test_sourceTextEditingProtectsBothSelections() {
            sources.selectedIds = ["source-video"]
            const search = focusOwner("sources", sources, "sourceBrowserSearch")
            search.text = "abc"; search.cursorPosition = 1
            tryCompare(deleteRoute, "textEditing", true)
            keyClick(Qt.Key_Delete)
            compare(search.text, "ac")
            compare(sourceController.removalRequests, [])
            compare(sources.selectedIds, ["source-video"])
            verifyCanvasUnchanged()
        }
        function test_independentTreeDeletesOnlyDisplayRequest() {
            const tree = focusOwner("independent", independent, "independentDisplayTree")
            mouseClick(tree, 150, 30)
            compare(independent.selectedIds, ["grid"])
            keyClick(Qt.Key_Delete)
            compare(independentController.removalRequests, [["grid"]])
            verifyCanvasUnchanged()
            independentController.selectedIndependentDisplayId = ""
            keyClick(Qt.Key_Delete)
            compare(independentController.removalRequests, [["grid"], []])
            verifyCanvasUnchanged()
        }
        function test_independentTextEditingProtectsBothSelections() {
            fixture.activePanel = "independent"
            independentController.selectIndependentDisplay("grid")
            independent.beginRename()
            tryCompare(deleteRoute, "textEditing", true)
            const editor = deleteRoute.focusItem
            editor.text = "abc"; editor.cursorPosition = 1
            keyClick(Qt.Key_Delete)
            compare(editor.text, "ac")
            compare(independentController.removalRequests, [])
            verifyCanvasUnchanged()
        }
        function test_nativeTreeLocalScope_data() {
            return [{tag: "all", scope: "all", removes: true},
                    {tag: "displays", scope: "displays", removes: true},
                    {tag: "views", scope: "views", removes: false},
                    {tag: "tools", scope: "tools", removes: false}]
        }
        function test_nativeTreeLocalScope(data) {
            nativePanel.treeScope = data.scope
            nativePanel.displaySelection = ["native-display"]
            focusOwner("native", nativePanel, "nativePropertyTree")
            keyClick(Qt.Key_Delete)
            compare(backend.removalRequests, data.removes ? [["native-display"]] : [])
            verifyCanvasUnchanged()
        }
        function test_nativePropertyTextProtectsBothSelections() {
            fixture.activePanel = "native"
            backend.selected = {id: "value", name: "Property", editable: true, enabled: true,
                                read_only: false, value_type: "QString", value: "abc"}
            const editor = focusOwner("native", nativePanel, "nativePropertyEditor")
            editor.text = "abc"; editor.cursorPosition = 1
            tryCompare(deleteRoute, "textEditing", true)
            keyClick(Qt.Key_Delete)
            compare(editor.text, "ac")
            compare(backend.removalRequests, [])
            verifyCanvasUnchanged()
        }
        function test_ownerChildButtonDoesNotFallBack_data() {
            return [{tag: "source", panelName: "sources", owner: sources, name: "sourceBrowserRemove"},
                    {tag: "display", panelName: "independent", owner: independent, name: "independentGlobalOptions"}]
        }
        function test_ownerChildButtonDoesNotFallBack(data) {
            sources.selectedIds = ["source-video"]
            focusOwner(data.panelName, data.owner, data.name)
            keyClick(Qt.Key_Delete)
            compare(sourceController.removalRequests, [])
            compare(independentController.removalRequests, [])
            verifyCanvasUnchanged()
        }
        function test_canvasLayerListKeepsLocalCanvasOwner() {
            focusOwner("layers", layers, "canvasLayersList")
            keyClick(Qt.Key_Delete)
            compare(layerCanvas.deletes, 1)
            compare(layerCanvas.selection, [])
            verifyCanvasUnchanged()
        }
        function test_trajectoryFilmstripKeepsFrameOwner() {
            focusOwner("trajectory", trajectoryTimeline, "trajectoryFrameFilmstrip")
            keyClick(Qt.Key_Delete)
            compare(trajectoryEditor.deletes, 1)
            verifyCanvasUnchanged()
        }
        function test_leavingOwnerRestoresCanvasShortcut() {
            focusOwner("sources", sources, "sourceBrowserList")
            outside.forceActiveFocus()
            tryCompare(deleteRoute, "localDeleteFocused", false)
            compare(deleteRoute.enabled, true)
            keyClick(Qt.Key_Delete)
            compare(fixture.canvasDeletes, 1)
            compare(sourceController.removalRequests, [])
        }
    }
}
