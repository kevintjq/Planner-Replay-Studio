import QtQuick

// All visible cameras share the client's one in-flight preview slot. The round
// robin only requests previews; it never seeks the experiment or recreates it.
Item {
    id: coordinator
    required property var client
    property bool active: false
    property bool configurationPending: false
    property var views: []
    property var participants: []
    property var classOverrides: ({})
    property string syncError: ""
    property string failedSignature: ""
    property bool syncPending: false
    property bool syncAcknowledged: false
    property string syncSignature: ""
    property bool framePending: false
    property int nextIndex: 0
    property var requestedAt: ({})
    readonly property var descriptors: views.map(view => ({view_id: view.id, class: classForId(view.id)}))
        .sort((a, b) => a.view_id.localeCompare(b.view_id))
    readonly property string desiredSignature: JSON.stringify(descriptors)
    readonly property string actualSignature: JSON.stringify((client?.renderViews ?? []).map(view => ({view_id: view.view_id, class: view.class}))
        .sort((a, b) => a.view_id.localeCompare(b.view_id)))

    function classForId(id) {
        return classOverrides[id] || (client?.renderViews ?? []).find(view => view.view_id === id)?.class
            || client?.archivedViewClasses?.[id] || "rviz/Orbit"
    }
    function setViewClass(id, pluginClass) {
        if (!views.some(view => view.id === id)) return
        const next = Object.assign({}, classOverrides); next[id] = pluginClass
        classOverrides = next; failedSignature = ""
    }
    function registerView(view) {
        if (participants.indexOf(view) < 0) participants = participants.concat([view])
    }
    function unregisterView(view) {
        participants = participants.filter(item => item !== view)
    }
    function requestRefresh(viewId) {
        const eligible = participants.filter(view => view.visible)
        const index = eligible.findIndex(view => view.viewId === viewId)
        if (index >= 0) nextIndex = index
    }
    function finishInstalledSync() {
        if (!syncPending || !syncAcknowledged || actualSignature !== syncSignature) return
        syncPending = false; syncAcknowledged = false; syncSignature = ""
        // The command acknowledgement can precede the client's tree refresh.
        // Keep requested classes until the real native controllers confirm them.
        const overrides = {}
        for (const id of Object.keys(classOverrides)) {
            if (!(client?.renderViews ?? []).some(view => view.view_id === id && view.class === classOverrides[id]))
                overrides[id] = classOverrides[id]
        }
        classOverrides = overrides
    }
    function tick() {
        if (!active || configurationPending || !client?.connected || client.busy || framePending || syncPending) return
        if (desiredSignature !== actualSignature) {
            if (desiredSignature !== failedSignature) {
                syncPending = true
                syncAcknowledged = false; syncSignature = desiredSignature
                client.syncViews(descriptors)
                return
            }
        }
        const eligible = participants.filter(view => view.visible && view.width > 0 && view.height > 0
            && (view.viewId === "main" || (client.renderViews ?? []).some(actual => actual.view_id === view.viewId)))
        if (!eligible.length) return
        for (let step = 0; step < eligible.length; ++step) {
            const index = (nextIndex + step) % eligible.length, view = eligible[index]
            const now = Date.now()
            if (now - (requestedAt[view.viewId] ?? 0) < 270) continue
            const next = Object.assign({}, requestedAt); next[view.viewId] = now; requestedAt = next
            nextIndex = (index + 1) % eligible.length
            client.requestViewPreview(view.viewId)
            return
        }
    }
    onActualSignatureChanged: finishInstalledSync()
    onDesiredSignatureChanged: failedSignature = ""
    onViewsChanged: {
        const ids = ["main"].concat(views.map(view => view.id)), times = {}, overrides = {}
        for (const id of ids) {
            if (requestedAt[id] !== undefined) times[id] = requestedAt[id]
        }
        // Add/Undo/Redo can finish before the host installs the new camera.
        // Keep its unacknowledged class until a real controller can be archived.
        // Bound removed identities to the same 4096-entry session Undo budget.
        const absent = Object.keys(classOverrides).filter(id => ids.indexOf(id) < 0)
        const capacity = Math.max(0, 4096 - ids.length)
        for (const id of absent.slice(Math.max(0, absent.length - capacity)).concat(ids))
            if (classOverrides[id] !== undefined) overrides[id] = classOverrides[id]
        requestedAt = times; classOverrides = overrides
    }
    onActiveChanged: if (!active) { framePending = false; syncPending = false; syncAcknowledged = false; syncSignature = "" }
    Timer { interval: 100; running: coordinator.active && !coordinator.configurationPending && !!coordinator.client?.connected; repeat: true; onTriggered: coordinator.tick() }
    Connections {
        target: coordinator.client
        ignoreUnknownSignals: true
        function onCommandSent(command) {
            if (command === "frame") coordinator.framePending = true
            if (command === "views.sync") {
                coordinator.syncPending = true; coordinator.syncAcknowledged = false
                if (!coordinator.syncSignature.length) coordinator.syncSignature = coordinator.desiredSignature
            }
        }
        function onCommandFinished(command, success, error) {
            if (command === "frame") coordinator.framePending = false
            if (command === "views.sync") {
                coordinator.syncError = success ? "" : error
                coordinator.failedSignature = success ? "" : coordinator.desiredSignature
                coordinator.syncAcknowledged = success
                if (success) coordinator.finishInstalledSync()
                else { coordinator.syncPending = false; coordinator.syncSignature = "" }
            }
            if ((command === "load" || command === "load_yaml" || command === "undo" || command === "redo") && success) {
                coordinator.classOverrides = ({})
                coordinator.failedSignature = ""
                coordinator.syncPending = false; coordinator.syncAcknowledged = false; coordinator.syncSignature = ""
            }
        }
        function onChanged() {
            if (!coordinator.client.connected) {
                coordinator.framePending = false; coordinator.syncPending = false
                coordinator.syncAcknowledged = false; coordinator.syncSignature = ""
                coordinator.failedSignature = ""; coordinator.requestedAt = ({})
            }
        }
    }
}
