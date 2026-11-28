import QtQuick
import QtTest
import "../../qml"

// Transport/UI contract fixtures only. These tests neither certify sensor
// content nor replace the separately executed real librviz / ROS bag gates.
Item {
    id: fixture
    width: 760; height: 200
    readonly property string exactNs: "1768571820557415724"
    readonly property string sampleImage: "data:image/svg+xml;utf8," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50"><rect width="100" height="50" fill="#6080a0"/></svg>')
    QtObject {
        id: backend
        property bool connected: true
        property bool busy: false
        property var renderViews: []
        property var archivedViewClasses: ({})
        property var viewPreviews: ({})
        property var syncRequests: []
        property var previewRequests: []
        property var pointerRequests: []
        property var pendingViews: []
        signal commandSent(string command)
        signal commandFinished(string command, bool success, string error)
        signal changed()
        signal modelChanged()
        function syncViews(views) {
            pendingViews = views.map(view => ({view_id: view.view_id, class: view.class}))
            syncRequests = syncRequests.concat([pendingViews])
            commandSent("views.sync")
        }
        function completeViews(success = true, installTree = true) {
            if (success && installTree) installViews()
            commandFinished("views.sync", success, success ? "" : "Installed native plugin refused")
        }
        function installViews() {
            const archived = Object.assign({}, archivedViewClasses)
            for (const view of renderViews)
                if (!pendingViews.some(next => next.view_id === view.view_id)) archived[view.view_id] = view.class
            archivedViewClasses = archived
            renderViews = pendingViews; modelChanged()
        }
        function requestViewPreview(id) {
            previewRequests = previewRequests.concat([id]); commandSent("frame")
        }
        function completeFrame(id, time = fixture.exactNs, exact = true) {
            const frames = Object.assign({}, viewPreviews)
            frames[id] = {view_id: id, source: fixture.sampleImage, clock_ns: time,
                synchronized_target_ns: time, synchronized: exact}
            viewPreviews = frames; commandFinished("frame", true, "")
        }
        function cameraMouseForView(id, type, x, y, button, buttons, modifiers, delta) {
            pointerRequests = pointerRequests.concat([{view_id: id, type: type, x: x, y: y,
                button: button, buttons: buttons, modifiers: modifiers, wheel_delta: delta}])
        }
    }
    QtObject {
        id: exporter
        property bool nativeFrameActive: false
        property var nativeFrameSources: ({})
    }
    NativeViewCoordinator {
        id: coordinator
        client: backend
        views: [{id: "orbit-a"}, {id: "top-b"}]
    }
    NativeSceneView {
        id: mainView
        width: 240; height: 120
        client: backend; coordinator: coordinator; exporter: exporter
        targetNs: fixture.exactNs; requireSynchronized: true
    }
    NativeSceneView {
        id: orbitView
        x: 250; width: 240; height: 120
        client: backend; coordinator: coordinator; exporter: exporter; viewId: "orbit-a"
        targetNs: fixture.exactNs; requireSynchronized: true
    }
    NativeSceneView {
        id: topView
        x: 500; width: 240; height: 120
        client: backend; coordinator: coordinator; exporter: exporter; viewId: "top-b"
        targetNs: fixture.exactNs; requireSynchronized: true
    }
    TestCase {
        name: "NativeMultiViewContract"
        when: windowShown
        function init() {
            coordinator.active = false
            coordinator.configurationPending = false
            coordinator.classOverrides = ({})
            coordinator.views = [{id: "orbit-a"}, {id: "top-b"}]
            coordinator.syncPending = coordinator.framePending = false
            coordinator.syncAcknowledged = false; coordinator.syncSignature = ""
            coordinator.failedSignature = coordinator.syncError = ""
            coordinator.requestedAt = ({})
            coordinator.nextIndex = 0
            backend.connected = true; backend.busy = false
            backend.renderViews = [{view_id: "orbit-a", class: "rviz/Orbit"}, {view_id: "top-b", class: "rviz/TopDownOrtho"}]
            backend.viewPreviews = ({})
            backend.archivedViewClasses = ({})
            backend.syncRequests = backend.previewRequests = backend.pointerRequests = []
            exporter.nativeFrameActive = false; exporter.nativeFrameSources = ({})
            mainView.width = 240; mainView.height = 120; mainView.interactive = true
            mainView.visible = orbitView.visible = topView.visible = true
        }
        function cleanup() { coordinator.active = false }
        function readyPointer(camera, id) {
            backend.completeFrame(id)
            const image = findChild(camera, id === "main" ? "nativeMainView" : "nativeView-" + id)
            tryCompare(image, "status", Image.Ready)
            return findChild(camera, "nativePointer-" + id)
        }
        function pointerTypes() { return backend.pointerRequests.map(event => event.type) }
        function verifyPointer(event, id, type, x, y, button, buttons, modifiers) {
            compare(event.view_id, id)
            compare(event.type, type)
            fuzzyCompare(event.x, x, .00001)
            fuzzyCompare(event.y, y, .00001)
            compare(event.button, button)
            compare(event.buttons, buttons)
            compare(event.modifiers, modifiers)
        }
        function test_roundRobinSharesOnePendingSlot() {
            coordinator.active = true
            coordinator.tick()
            compare(backend.previewRequests.length, 1)
            coordinator.tick()
            compare(backend.previewRequests.length, 1, "A pending frame must block the next camera")
            backend.completeFrame(backend.previewRequests[0])
            coordinator.tick()
            compare(backend.previewRequests.length, 2)
            backend.completeFrame(backend.previewRequests[1])
            coordinator.tick()
            compare(backend.previewRequests.length, 3)
            compare(backend.previewRequests.slice().sort(), ["main", "orbit-a", "top-b"])
        }
        function test_layoutEditsDoNotRecreateNativeCameras() {
            coordinator.views = [{id: "orbit-a", x: .2, width: .3}, {id: "top-b", x: .8}]
            coordinator.active = true
            coordinator.tick()
            compare(backend.syncRequests.length, 0)
            backend.completeFrame(backend.previewRequests[0])
            coordinator.views = [{id: "top-b", x: .1}, {id: "orbit-a", x: .8, width: .5}]
            coordinator.tick()
            compare(backend.syncRequests.length, 0, "Order, position and size must not reset cameras")
            compare(coordinator.classForId("top-b"), "rviz/TopDownOrtho")
        }
        function test_addAndClassChangeAreAtomicUntilAcknowledged() {
            backend.renderViews = []
            coordinator.setViewClass("top-b", "rviz/TopDownOrtho")
            coordinator.active = true; coordinator.tick(); coordinator.tick()
            compare(backend.syncRequests.length, 1)
            compare(backend.previewRequests.length, 0)
            compare(backend.syncRequests[0], [{view_id: "orbit-a", class: "rviz/Orbit"}, {view_id: "top-b", class: "rviz/TopDownOrtho"}])
            backend.completeViews()
            coordinator.tick()
            compare(backend.syncRequests.length, 1)
            compare(backend.previewRequests.length, 1)
        }
        function test_classAcknowledgementWaitsForRealTree() {
            coordinator.setViewClass("orbit-a", "rviz/TopDownOrtho")
            coordinator.active = true; coordinator.tick()
            compare(backend.syncRequests.length, 1)
            backend.completeViews(true, false)
            compare(coordinator.classForId("orbit-a"), "rviz/TopDownOrtho",
                    "A successful command precedes the client's asynchronous tree refresh")
            coordinator.tick()
            compare(backend.syncRequests.length, 1, "Do not resynchronize from a stale native property tree")
            compare(backend.previewRequests.length, 0)
            backend.installViews(); coordinator.tick()
            compare(backend.syncRequests.length, 1)
            compare(backend.previewRequests.length, 1)
            compare(Object.keys(coordinator.classOverrides).length, 0)
        }
        function test_deletedCameraUsesNativeArchiveWhenLayoutUndoRestoresId() {
            coordinator.views = [{id: "orbit-a"}]
            coordinator.active = true; coordinator.tick(); backend.completeViews()
            compare(backend.archivedViewClasses["top-b"], "rviz/TopDownOrtho")
            coordinator.views = [{id: "orbit-a"}, {id: "top-b"}]
            coordinator.tick()
            compare(backend.syncRequests.length, 2)
            compare(backend.syncRequests[1][1].class, "rviz/TopDownOrtho",
                    "Undo restores the stable ID of a deleted real controller, not a default Orbit camera")
            backend.completeViews()
            coordinator.views = [{id: "orbit-a"}]
            coordinator.tick(); backend.completeViews()
            coordinator.views = [{id: "orbit-a"}, {id: "top-b"}]
            coordinator.tick()
            compare(backend.syncRequests[3][1].class, "rviz/TopDownOrtho", "Redo/delete followed by Undo keeps the same controller class")
        }
        function test_rapidAddUndoRedoKeepsUninstalledNativeClass() {
            backend.renderViews = []
            coordinator.views = [{id: "new-top"}]
            coordinator.setViewClass("new-top", "rviz/TopDownOrtho")
            coordinator.views = []
            coordinator.views = [{id: "new-top"}]
            coordinator.active = true; coordinator.tick()
            compare(backend.syncRequests[0], [{view_id: "new-top", class: "rviz/TopDownOrtho"}],
                    "Undo/Redo before the first native sync must preserve the requested plugin")
        }
        function test_removedUnacknowledgedClassesHaveBoundedStorage() {
            const overrides = {}
            for (let i = 0; i < 5000; ++i) overrides["pending-" + i] = "rviz/TopDownOrtho"
            overrides["orbit-a"] = "rviz/FPS"
            coordinator.classOverrides = overrides
            coordinator.views = [{id: "orbit-a"}]
            verify(Object.keys(coordinator.classOverrides).length <= 4096)
            compare(coordinator.classForId("orbit-a"), "rviz/FPS", "Current camera overrides survive cache trimming")
        }
        function test_queuedProjectDoesNotCreateDefaultCameras() {
            backend.renderViews = []
            coordinator.setViewClass("orbit-a", "rviz/FPS")
            coordinator.configurationPending = true
            coordinator.active = true; coordinator.tick()
            compare(backend.syncRequests.length, 0, "New layout IDs must wait for their saved native controllers")
            compare(backend.previewRequests.length, 0)
            backend.busy = true; backend.commandSent("load_yaml")
            coordinator.configurationPending = false; coordinator.tick()
            compare(backend.syncRequests.length, 0, "An in-flight native configuration must remain gated")
            backend.renderViews = [{view_id: "orbit-a", class: "rviz/TopDownOrtho"}, {view_id: "top-b", class: "rviz/Orbit"}]
            backend.modelChanged(); backend.busy = false
            backend.commandFinished("load_yaml", true, "")
            coordinator.tick()
            compare(backend.syncRequests.length, 0)
            compare(coordinator.classForId("orbit-a"), "rviz/TopDownOrtho")
            compare(backend.previewRequests.length, 1)
        }
        function test_laterClassEditSurvivesEarlierTreeAcknowledgement() {
            coordinator.setViewClass("orbit-a", "rviz/TopDownOrtho")
            coordinator.active = true; coordinator.tick()
            coordinator.setViewClass("orbit-a", "rviz/FPS")
            backend.completeViews(true, false)
            backend.installViews(); coordinator.tick()
            compare(backend.syncRequests.length, 2)
            compare(backend.syncRequests[1][0].class, "rviz/FPS")
            backend.completeViews(); coordinator.tick()
            compare(coordinator.classForId("orbit-a"), "rviz/FPS")
            compare(backend.syncRequests.length, 2)
            compare(backend.previewRequests.length, 1)
        }
        function test_disconnectReleasesAcknowledgedSync() {
            coordinator.setViewClass("orbit-a", "rviz/TopDownOrtho")
            coordinator.active = true; coordinator.tick()
            backend.completeViews(true, false)
            verify(coordinator.syncPending)
            backend.connected = false; backend.changed()
            verify(!coordinator.syncPending)
            verify(!coordinator.syncAcknowledged)
            compare(coordinator.syncSignature, "")
            backend.installViews(); backend.connected = true
            coordinator.tick()
            compare(backend.previewRequests.length, 1)
        }
        function test_failedConfigurationDoesNotHoldTransportGate() {
            backend.busy = true; backend.commandSent("load_yaml")
            coordinator.active = true; coordinator.tick()
            compare(backend.previewRequests.length, 0)
            backend.busy = false; backend.commandFinished("load_yaml", false, "Invalid native YAML")
            coordinator.tick()
            compare(backend.previewRequests.length, 1)
            compare(backend.syncRequests.length, 0)
        }
        function test_hiddenViewsDoNotConsumePreviewRequests() {
            orbitView.visible = false
            coordinator.active = true; coordinator.tick()
            backend.completeFrame(backend.previewRequests[0]); coordinator.tick()
            compare(backend.previewRequests.slice().sort(), ["main", "top-b"])
            orbitView.visible = true
            backend.completeFrame(backend.previewRequests[1]); coordinator.tick()
            compare(backend.previewRequests[2], "orbit-a")
        }
        function test_exactReadinessBelongsToEachView() {
            backend.completeFrame("main")
            backend.completeFrame("orbit-a", "1768571814557415724")
            verify(mainView.matchesPlayhead)
            verify(!orbitView.matchesPlayhead)
            verify(!topView.matchesPlayhead)
            backend.completeFrame("top-b", fixture.exactNs, false)
            verify(!topView.matchesPlayhead, "A correct clock alone must not certify native TF/Display readiness")
        }
        function test_exportNeverFallsBackToPreview() {
            backend.completeFrame("orbit-a")
            const image = findChild(orbitView, "nativeView-orbit-a")
            verify(image.nativeSceneImage)
            compare(image.exportViewId, "orbit-a")
            compare(decodeURIComponent(image.source.toString()), decodeURIComponent(fixture.sampleImage))
            exporter.nativeFrameActive = true
            compare(image.source.toString(), "", "Missing output must remain missing, even with a cached preview")
            exporter.nativeFrameSources = {"orbit-a": fixture.sampleImage + "#output"}
            compare(decodeURIComponent(image.source.toString()), decodeURIComponent(fixture.sampleImage + "#output"))
        }
        function test_mouseUsesSelectedCameraAndCancelsCorrectButton() {
            backend.completeFrame("orbit-a")
            const image = findChild(orbitView, "nativeView-orbit-a")
            tryCompare(image, "status", Image.Ready)
            const pointer = findChild(orbitView, "nativePointer-orbit-a")
            mousePress(pointer, 120, 60, Qt.RightButton)
            orbitView.interactive = false
            compare(backend.pointerRequests.length, 2)
            compare(backend.pointerRequests[0].view_id, "orbit-a")
            compare(backend.pointerRequests[0].type, "press")
            compare(backend.pointerRequests[1].type, "release")
            compare(backend.pointerRequests[1].button, Qt.RightButton)
            orbitView.interactive = true
            mouseRelease(pointer, 120, 60, Qt.RightButton)
        }
        function test_fastDragFlushesMoveBeforeRelease() {
            const pointer = readyPointer(orbitView, "orbit-a")
            mousePress(pointer, 60, 30, Qt.LeftButton, Qt.ShiftModifier, 0)
            mouseMove(pointer, 120, 60, 0, Qt.LeftButton, Qt.ShiftModifier)
            compare(pointerTypes(), ["press"], "This gesture finishes before the 33ms throttle")
            mouseRelease(pointer, 180, 90, Qt.LeftButton, Qt.ShiftModifier, 0)
            compare(pointerTypes(), ["press", "move", "release"])
            verifyPointer(backend.pointerRequests[1], "orbit-a", "move", .75, .75,
                          Qt.NoButton, Qt.LeftButton, Qt.ShiftModifier)
            verifyPointer(backend.pointerRequests[2], "orbit-a", "release", .75, .75,
                          Qt.LeftButton, Qt.NoButton, Qt.ShiftModifier)
            wait(45)
            compare(pointerTypes(), ["press", "move", "release"], "No deferred move may follow release")
        }
        function test_longDragFlushesOnlyUnsentTail() {
            const pointer = readyPointer(orbitView, "orbit-a")
            mousePress(pointer, 60, 30, Qt.MiddleButton, Qt.ControlModifier, 0)
            mouseMove(pointer, 120, 60, 0, Qt.MiddleButton, Qt.ControlModifier)
            tryVerify(() => backend.pointerRequests.length === 2)
            verifyPointer(backend.pointerRequests[1], "orbit-a", "move", .5, .5,
                          Qt.NoButton, Qt.MiddleButton, Qt.ControlModifier)
            mouseMove(pointer, 180, 90, 0, Qt.MiddleButton, Qt.ControlModifier)
            mouseRelease(pointer, 180, 90, Qt.MiddleButton, Qt.ControlModifier, 0)
            compare(pointerTypes(), ["press", "move", "move", "release"])
            verifyPointer(backend.pointerRequests[2], "orbit-a", "move", .75, .75,
                          Qt.NoButton, Qt.MiddleButton, Qt.ControlModifier)
            wait(45)
            compare(backend.pointerRequests.length, 4)
        }
        function test_stationaryReleaseDoesNotDuplicateMove() {
            const pointer = readyPointer(orbitView, "orbit-a")
            mousePress(pointer, 60, 30, Qt.LeftButton, Qt.NoModifier, 0)
            mouseMove(pointer, 120, 60, 0, Qt.LeftButton, Qt.NoModifier)
            tryVerify(() => backend.pointerRequests.length === 2)
            mouseRelease(pointer, 120, 60, Qt.LeftButton, Qt.NoModifier, 0)
            compare(pointerTypes(), ["press", "move", "release"])
            backend.pointerRequests = []
            mouseClick(pointer, 120, 60, Qt.LeftButton, Qt.NoModifier, 0)
            compare(pointerTypes(), ["press", "release"], "A click is not a camera move")
        }
        function test_cancelDropsPendingMoveAndKeepsGestureIdentity() {
            const pointer = readyPointer(orbitView, "orbit-a")
            mousePress(pointer, 60, 30, Qt.RightButton, Qt.AltModifier, 0)
            mouseMove(pointer, 180, 90, 0, Qt.RightButton, Qt.AltModifier)
            pointer.cancelPointer()
            compare(pointerTypes(), ["press", "release"])
            verifyPointer(backend.pointerRequests[1], "orbit-a", "release", .25, .25,
                          Qt.RightButton, Qt.NoButton, Qt.AltModifier)
            wait(45)
            mouseRelease(pointer, 180, 90, Qt.RightButton, Qt.AltModifier, 0)
            compare(pointerTypes(), ["press", "release"], "Canceled physical release must not be sent twice")
        }
        function test_disabledGestureDropsPendingMove_data() {
            return [{tag: "camera-mode-off", reason: "mode"},
                    {tag: "output-active", reason: "output"},
                    {tag: "view-hidden", reason: "hidden"},
                    {tag: "client-busy", reason: "busy"},
                    {tag: "camera-id-changed", reason: "id"}]
        }
        function test_disabledGestureDropsPendingMove(data) {
            const pointer = readyPointer(orbitView, "orbit-a")
            mousePress(pointer, 60, 30, Qt.RightButton, Qt.ControlModifier, 0)
            mouseMove(pointer, 180, 90, 0, Qt.RightButton, Qt.ControlModifier)
            if (data.reason === "mode") orbitView.interactive = false
            else if (data.reason === "output") exporter.nativeFrameActive = true
            else if (data.reason === "hidden") orbitView.visible = false
            else if (data.reason === "id") orbitView.viewId = "replacement-camera"
            else backend.busy = true
            compare(pointerTypes(), ["press", "release"])
            verifyPointer(backend.pointerRequests[1], "orbit-a", "release", .25, .25,
                          Qt.RightButton, Qt.NoButton, Qt.ControlModifier)
            wait(45)
            orbitView.interactive = true; orbitView.visible = true
            orbitView.viewId = "orbit-a"
            exporter.nativeFrameActive = false; backend.busy = false
            mouseRelease(pointer, 180, 90, Qt.RightButton, Qt.ControlModifier, 0)
            compare(pointerTypes(), ["press", "release"])
        }
        function test_letterboxIsNotAnEdgeCameraClick() {
            backend.completeFrame("main")
            mainView.height = 180
            const image = findChild(mainView, "nativeMainView")
            tryCompare(image, "status", Image.Ready)
            const pointer = findChild(mainView, "nativePointer-main")
            mouseClick(pointer, 120, 5)
            compare(backend.pointerRequests.length, 0)
            mouseClick(pointer, 120, 90)
            compare(backend.pointerRequests[0].view_id, "main")
            compare(backend.pointerRequests[0].x, .5)
            compare(backend.pointerRequests[0].y, .5)
        }
        function test_disconnectResetsPendingTransportState() {
            coordinator.active = true; coordinator.tick()
            verify(coordinator.framePending)
            backend.connected = false; backend.changed()
            verify(!coordinator.framePending)
            verify(!coordinator.syncPending)
            compare(Object.keys(coordinator.requestedAt).length, 0)
        }
    }
}
