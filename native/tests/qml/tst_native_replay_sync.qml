import QtQuick
import QtTest
import UAVStudioTest 1.0
import "../../qml"

Item {
    id: testRoot
    width: 420; height: 800
    NativeReplayTestState {
        id: backend
        property bool connected: true
        property bool running: true
        property bool detecting: false
        property bool busy: false
        property bool canUndo: false
        property bool canRedo: false
        property bool indexed: false
        property bool sceneReady: false
        property var displayTopics: []
        property string targetNs: ""
        property string clockNs: ""
        property string previewTargetNs: ""
        property string preview: ""
        property string status: "Test transport"
        property string diagnostic: ""
        property string launcher: ""
        property string distribution: ""
        property string selectedId: ""
        property string search: ""
        property var selected: ({})
        property var plugins: []
        property var topics: []
        property var properties: []
        property var seekRequests: []
        property var indexRequests: []
        property int cancellations: 0
        signal modelChanged()
        signal changed()
        signal commandFinished(string command, bool success, string error)
        function compatiblePlugins(type) { return [] }
        function requestPreview() {}
        function indexBag(file, basis, policy, options) {
            indexRequests = indexRequests.concat([{file: file.toString(), basis: basis, policy: policy, topics: options.topics}])
            indexed = false
            busy = true
        }
        function seekAbsolute(ns) { seekRequests = seekRequests.concat([ns]) }
        function cancelReplay() { cancellations++; busy = false }
        function cancelPendingSeek() { cancellations++ }
        function completeIndex() { busy = false; indexed = true; commandFinished("bag.index", true, "") }
    }
    NativeRvizPanel {
        id: panel
        anchors.fill: parent
        client: backend
        showConnection: false; showTree: false; showInspector: false; showPreview: false
    }
    TestCase {
        name: "NativePausedReplay"
        when: windowShown
        function init() {
            testRoot.width = 420
            panel.showConnection = panel.showTree = panel.showInspector = false
            panel.advancedVisible = false
            panel.linkPausedFrame = false
            panel.experimentBag = "file:///C:/sample/flight.bag"
            panel.experimentTopics = ["/cloud", "/tf"]
            panel.experimentRevision = "1"
            panel.experimentPaused = true
            panel.experimentTargetNs = "1768571824347301984"
            panel.pendingSignature = ""
            panel.indexedSignature = ""
            backend.busy = false; backend.indexed = false; backend.synchronized = false
            backend.sceneReady = false; backend.clockNs = ""; backend.targetNs = ""; backend.previewTargetNs = ""; backend.displayTopics = []
            backend.seekRequests = []; backend.indexRequests = []; backend.cancellations = 0
        }
        function test_exactPausedTimestampAndDebounce() {
            panel.syncPausedFrame()
            compare(backend.seekRequests.length, 0)
            panel.indexExperiment()
            compare(backend.indexRequests.length, 1)
            compare(backend.indexRequests[0].basis, "header")
            compare(backend.indexRequests[0].policy, "reject")
            compare(backend.indexRequests[0].topics, ["/cloud", "/tf"])
            backend.completeIndex()
            verify(panel.experimentIndexed)
            panel.syncPausedFrame()
            compare(backend.seekRequests[0], "1768571824347301984")
            panel.linkPausedFrame = true
            panel.experimentTargetNs = "1768571824347301985"
            panel.experimentTargetNs = "1768571824347301986"
            tryCompare(backend, "seekRequests", ["1768571824347301984", "1768571824347301986"])
            backend.targetNs = panel.experimentTargetNs; backend.clockNs = panel.experimentTargetNs
            backend.synchronized = true; backend.sceneReady = true
            verify(!panel.matchesPlayhead)
            backend.previewTargetNs = "1768571824347301985"
            verify(!panel.matchesPlayhead)
            backend.previewTargetNs = panel.experimentTargetNs
            verify(panel.matchesPlayhead)
            backend.clockNs = "1768571824347301985"
            verify(!panel.matchesPlayhead)
            backend.clockNs = panel.experimentTargetNs; backend.busy = true
            verify(!panel.matchesPlayhead)
            backend.busy = false; backend.sceneReady = false
            verify(!panel.matchesPlayhead)
            backend.sceneReady = true
            panel.experimentPaused = false
            compare(backend.cancellations, 1)
            panel.experimentTargetNs = "1768571824347301987"
            panel.syncPausedFrame()
            wait(240)
            compare(backend.seekRequests.length, 2)
            verify(!panel.matchesPlayhead)
            panel.experimentPaused = true
            tryCompare(backend, "seekRequests", ["1768571824347301984", "1768571824347301986", "1768571824347301987"])
        }
        function test_reloadAndSourceChangesRejectStaleIndex() {
            panel.indexExperiment(); backend.completeIndex()
            verify(panel.experimentIndexed)
            panel.experimentRevision = "2"
            verify(!panel.experimentIndexed)
            panel.syncPausedFrame(); compare(backend.seekRequests.length, 0)
            panel.indexExperiment()
            panel.experimentBag = "file:///C:/sample/other.bag"
            compare(backend.cancellations, 1)
            backend.completeIndex()
            verify(!panel.experimentIndexed)
            panel.syncPausedFrame(); compare(backend.seekRequests.length, 0)
        }
        function test_nativePluginTopicsJoinAndInvalidateIndex() {
            backend.displayTopics = ["/marker_plugin_only", "/cloud", "/camera/image"]
            panel.indexExperiment()
            compare(backend.indexRequests[0].topics, ["/camera/image", "/cloud", "/marker_plugin_only", "/tf"])
            backend.completeIndex(); verify(panel.experimentIndexed)
            backend.displayTopics = ["/new_native_topic", "/cloud"]
            verify(!panel.experimentIndexed)
            panel.syncPausedFrame(); compare(backend.seekRequests.length, 0)
        }
        function test_narrowNativeInspectorContainsControls() {
            testRoot.width = 260
            panel.showConnection = panel.showTree = panel.showInspector = true
            panel.advancedVisible = true
            wait(80)
            for (const name of ["nativeDistribution", "nativeLauncherBrowse", "nativeConnect", "nativeImport",
                                "nativeSave", "nativeTreeHeading", "nativePropertyEditor", "nativePropertyApply"]) {
                const item = findChild(panel, name)
                verify(item !== null, name)
                const origin = item.mapToItem(panel, 0, 0)
                verify(origin.x >= -1 && origin.x + item.width <= panel.width + 1, name + " exceeds narrow Inspector")
            }
        }
    }
}
