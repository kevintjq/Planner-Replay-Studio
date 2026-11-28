import QtQuick
import QtTest
import "../../qml"

// Save sequencing only. Native snapshots and installed-plugin YAML round trips
// are tested separately against the real client and librviz host.
Item {
    id: fixture
    width: 320; height: 180
    property var host: null
    readonly property string source: mainUiSource.replace(/\r\n/g, "\n")
    QtObject {
        id: replayState
        property string projectPath: "current.uavstudio"
        property var nativeReplaySource: ({revision: "recording-a"})
        property bool loading: false
        property var cloudViews: [{id: "orbit-a"}]
        property var saved: []
        property var captured: []
        function setNativeRvizConfig(yaml, metadata) { captured = JSON.parse(yaml) }
        function saveProjectAs(file) { saved = saved.concat([{file: file.toString(), kind: "project", views: captured}]) }
        function exportProjectFolder(file) { saved = saved.concat([{file: file.toString(), kind: "package", views: captured}]) }
    }
    QtObject {
        id: backend
        property bool connected: true
        property bool busy: false
        property var renderViews: [{view_id: "orbit-a", class: "rviz/Orbit"}]
        property var archivedViewClasses: ({"top-b": "rviz/TopDownOrtho"})
        property var syncRequests: []
        property var snapshotRequests: []
        property var pendingViews: []
        signal commandSent(string command)
        signal commandFinished(string command, bool success, string error)
        signal changed()
        signal configSnapshotReady(string jobId, bool success, string yaml, var metadata, string error)
        function syncViews(views) {
            pendingViews = views.map(view => ({view_id: view.view_id, class: view.class}))
            syncRequests = syncRequests.concat([pendingViews])
            busy = true; commandSent("views.sync")
        }
        function installViews() { renderViews = pendingViews }
        function finishSync(success = true, installTree = true) {
            if (success && installTree) installViews()
            busy = false; commandFinished("views.sync", success, success ? "" : "Plugin refused camera")
        }
        function snapshotConfig() {
            const job = "snapshot-" + (snapshotRequests.length + 1)
            snapshotRequests = snapshotRequests.concat([{job: job, views: renderViews.slice()}])
            return job
        }
        function finishSnapshot(success = true) {
            const request = snapshotRequests[snapshotRequests.length - 1]
            configSnapshotReady(request.job, success, JSON.stringify(request.views), ({}), success ? "" : "Snapshot failed")
        }
    }
    QtObject { id: movieState; property bool exporting: false }
    QtObject { id: batchState; property bool active: false }
    QtObject { id: canvasState; readonly property var nativeViewCoordinator: coordinator }
    NativeViewCoordinator {
        id: coordinator
        client: backend
        views: replayState.cloudViews
        // Match the stricter production host contract: the waiting phase must
        // not mark an export busy before this coordinator can install cameras.
        active: !!fixture.host && fixture.host.nativeBackendActive && !fixture.host.exportBusy
        configurationPending: !!fixture.host && fixture.host.nativeConfigurationPending
    }
    TestCase {
        name: "NativeProjectSave"
        when: windowShown
        function productionFunction(name) {
            const start = fixture.source.indexOf("function " + name + "(")
            verify(start >= 0, "Missing production function " + name)
            const open = fixture.source.indexOf("{", start)
            let depth = 1, quote = "", escaped = false
            for (let end = open + 1; end < fixture.source.length; ++end) {
                const ch = fixture.source[end]
                if (quote.length) {
                    if (escaped) escaped = false
                    else if (ch === "\\") escaped = true
                    else if (ch === quote) quote = ""
                } else if (ch === '"' || ch === "'") quote = ch
                else if (ch === "{") depth++
                else if (ch === "}" && --depth === 0) return fixture.source.slice(start, end + 1)
            }
            fail("Unterminated production function " + name)
        }
        function init() {
            backend.connected = true; backend.busy = false
            backend.renderViews = [{view_id: "orbit-a", class: "rviz/Orbit"}]
            backend.syncRequests = []; backend.snapshotRequests = []; backend.pendingViews = []
            replayState.projectPath = "current.uavstudio"; replayState.nativeReplaySource = ({revision: "recording-a"})
            replayState.loading = false; replayState.cloudViews = [{id: "orbit-a"}]
            replayState.saved = []; replayState.captured = []
            coordinator.classOverrides = ({}); coordinator.failedSignature = ""; coordinator.syncError = ""
            coordinator.syncPending = false; coordinator.syncAcknowledged = false; coordinator.syncSignature = ""
            movieState.exporting = false; batchState.active = false
            const start = fixture.source.indexOf("    function projectSignature()")
            const end = fixture.source.indexOf("    Connections {\n        target: replay", start)
            verify(start >= 0 && end > start, "Production save block must be available verbatim")
            const declarations = fixture.source.split("\n").filter(line => /^\s*(?:readonly )?property (?:string|url|bool) (?:nativeSave\w*|pendingNativeYaml|exportBusy):/.test(line)).join("\n")
            const previewGate = fixture.source.match(/nativeConfigurationPending:\s*([^\r\n]+)/)
            verify(previewGate, "Production preview gate is missing")
            fixture.host = Qt.createQmlObject('import QtQuick; Item { id: window;'
                + 'property var replay: replayState; property var nativeRviz: backend; property var canvas: canvasState;'
                + 'property var movie: movieState; property var frameBatch: batchState;'
                + 'property bool nativeBackendSelected: true; readonly property bool nativeBackendActive: nativeBackendSelected && backend.connected;'
                + 'property bool nativeConfigurationPending: ' + previewGate[1] + '\n'
                + declarations + '\n' + fixture.source.slice(start, end)
                + 'Connections { target: backend; ' + productionFunction("onConfigSnapshotReady") + '\n'
                + productionFunction("onChanged") + '\n} }', fixture, "ProductionNativeSaveFixture")
        }
        function cleanup() {
            if (fixture.host) { fixture.host.destroy(); fixture.host = null }
        }
        function save(kind = "project") { fixture.host.saveProject(Qt.resolvedUrl("saved.uavstudio"), kind) }
        function test_beforeFirstTick_data() {
            return [
                {tag: "add", edits: [[{id: "orbit-a"}, {id: "new-fps"}]], plugin: true},
                {tag: "delete", edits: [[]]},
                {tag: "undo-deletion", edits: [[{id: "orbit-a"}, {id: "top-b"}], [], [{id: "orbit-a"}, {id: "top-b"}]]},
                {tag: "redo-deletion", edits: [[{id: "orbit-a"}, {id: "top-b"}], [], [{id: "orbit-a"}, {id: "top-b"}], []]}
            ]
        }
        function test_beforeFirstTick(data) {
            for (const views of data.edits) replayState.cloudViews = views
            if (data.plugin) coordinator.setViewClass("new-fps", "rviz/FPS")
            // No wait()/tick() between the edit and the real production save.
            save()
            compare(backend.snapshotRequests.length, 0, "Save must install changed native cameras before taking YAML")
            verify(fixture.host.nativeSaveWaiting)
            verify(!fixture.host.exportBusy, "The pending save must leave the coordinator able to sync")
            tryCompare(backend, "syncRequests", [coordinator.descriptors])
            backend.finishSync()
            tryVerify(() => backend.snapshotRequests.length === 1)
            compare(backend.snapshotRequests[0].views, coordinator.descriptors)
            verify(fixture.host.exportBusy)
            backend.finishSnapshot()
            compare(replayState.saved.length, 1)
            compare(replayState.saved[0].views, coordinator.descriptors)
        }
        function test_acknowledgementWaitsForInstalledTree() {
            replayState.cloudViews = [{id: "orbit-a"}, {id: "top-b"}]
            save(); backend.finishSync(true, false)
            wait(130)
            compare(backend.snapshotRequests.length, 0, "Acknowledgement alone cannot certify the controller YAML")
            verify(fixture.host.nativeSaveWaiting)
            backend.installViews()
            tryVerify(() => backend.snapshotRequests.length === 1)
            backend.finishSnapshot()
            compare(replayState.saved[0].views[1].class, "rviz/TopDownOrtho")
        }
        function test_pendingConfigurationAndTransportFinishAutomatically() {
            fixture.host.pendingNativeYaml = "queued real project YAML"
            save("package")
            compare(backend.snapshotRequests.length, 0)
            backend.busy = true; fixture.host.pendingNativeYaml = ""
            wait(130)
            compare(backend.snapshotRequests.length, 0)
            backend.busy = false
            tryVerify(() => backend.snapshotRequests.length === 1)
            backend.finishSnapshot()
            compare(replayState.saved.length, 1); compare(replayState.saved[0].kind, "package")
        }
        function test_editDuringWaitingSavesLatestInstalledCameras() {
            replayState.cloudViews = [{id: "orbit-a"}, {id: "top-b"}]
            save()
            replayState.cloudViews = [{id: "top-b"}]
            backend.finishSync()
            tryVerify(() => backend.syncRequests.length === 2)
            compare(backend.snapshotRequests.length, 0)
            backend.finishSync()
            tryVerify(() => backend.snapshotRequests.length === 1)
            backend.finishSnapshot()
            compare(replayState.saved[0].views, [{view_id: "top-b", class: "rviz/TopDownOrtho"}])
        }
        function test_failedSyncDoesNotWriteStaleProject() {
            replayState.cloudViews = []
            save(); backend.finishSync(false)
            tryVerify(() => fixture.host.nativeSaveError.length > 0)
            verify(!fixture.host.nativeSaveWaiting); verify(!fixture.host.exportBusy)
            compare(backend.snapshotRequests.length, 0); compare(replayState.saved.length, 0)
        }
        function test_disconnectWhileWaitingDoesNotWriteStaleProject() {
            backend.busy = true; save()
            backend.connected = false; backend.changed()
            tryVerify(() => fixture.host.nativeSaveError.length > 0)
            verify(!fixture.host.nativeSaveWaiting)
            compare(backend.snapshotRequests.length, 0); compare(replayState.saved.length, 0)
        }
        function test_projectSwitchWhileWaitingDoesNotSaveOtherProject() {
            backend.busy = true; save()
            replayState.projectPath = "different.uavstudio"; backend.busy = false
            tryVerify(() => fixture.host.nativeSaveError.length > 0)
            verify(!fixture.host.nativeSaveWaiting)
            compare(backend.snapshotRequests.length, 0); compare(replayState.saved.length, 0)
        }
        function test_snapshotFailureIsReportedWithoutWriting() {
            save()
            compare(backend.snapshotRequests.length, 1)
            backend.finishSnapshot(false)
            compare(fixture.host.nativeSaveError, "Snapshot failed")
            compare(replayState.saved.length, 0); verify(!fixture.host.exportBusy)
        }
        function test_snapshotHoldsCoordinatorAndRejectsLaterLayoutChange() {
            save()
            compare(backend.snapshotRequests.length, 1)
            verify(fixture.host.nativeConfigurationPending, "The async snapshot must hold native preview/sync requests")
            replayState.cloudViews = []
            coordinator.tick()
            compare(backend.syncRequests.length, 0)
            backend.finishSnapshot()
            verify(fixture.host.nativeSaveError.length > 0)
            compare(replayState.saved.length, 0, "An externally changed layout cannot be combined with an older snapshot")
        }
        function test_projectConfigurationChangedDuringSnapshotDoesNotWrite() {
            save()
            fixture.host.pendingNativeYaml = "replacement YAML"
            backend.finishSnapshot()
            verify(fixture.host.nativeSaveError.length > 0)
            compare(replayState.saved.length, 0)
        }
        function test_independentBackendStillSavesDirectly() {
            fixture.host.nativeBackendSelected = false
            save()
            compare(replayState.saved.length, 1); compare(backend.snapshotRequests.length, 0)
        }
    }
}
