import QtQuick
import QtQuick.Controls
import QtTest
import UAVStudioTest 1.0
import "../../qml"

Item {
    id: fixture
    width: 760; height: 900
    property var cameraHost: null

    // UI forwarding fixture only. Installed-plugin discovery and ROS rendering
    // are verified separately against librviz; these values certify no sensor.
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
        property string status: "UI contract fixture"
        property string diagnostic: ""
        property string launcher: ""
        property string distribution: ""
        property string selectedId: ""
        property string search: ""
        property var selected: ({})
        property var plugins: []
        property var topics: []
        property var properties: []
        property var viewControllers: [{class: "rviz/Orbit"}, {class: "rviz/TopDownOrtho"}]
        property var resetViews: []
        property var edits: []
        property int deletes: 0
        property int previews: 0
        property var savedConfigs: []
        property int starts: 0
        property string editedYaml: "Visualization Manager:\n  Displays: [{Class: rviz/Grid, Alpha: 0.375}]\n"
        signal modelChanged()
        signal changed()
        signal commandFinished(string command, bool success, string error)
        function compatiblePlugins(type) { return [] }
        function requestPreview() { previews++ }
        function select(id) {
            selectedId = id
            selected = properties.find(row => row.id === id) || ({})
            modelChanged()
        }
        function setPropertyValue(id, value) { edits = edits.concat([{id: id, value: value}]) }
        function removeDisplays(ids) { deletes++ }
        function saveConfig(file) { savedConfigs = savedConfigs.concat([{file: file.toString(), yaml: editedYaml}]) }
        function ensureStarted() { starts++ }
        function resetCameraForView(id) { resetViews = resetViews.concat([id]) }
    }
    QtObject {
        id: backendPreference
        property string renderBackend: "independent"
    }
    QtObject {
        id: routeWindow
        property bool nativeBackendSelected: false
        readonly property bool nativeBackendActive: nativeBackendSelected && backend.connected
    }
    QtObject { id: routeSection; property int currentIndex: 2 }
    QtObject { id: routeScrollContent; property real contentY: 87 }
    QtObject { id: routeScroll; property var contentItem: routeScrollContent }
    QtObject {
        id: offline
        property var savedConfigs: []
        property string importedYaml: "Visualization Manager:\n  Displays: [{Class: rviz/Grid, Alpha: 0.75}]\n"
        function exportRviz(file) { savedConfigs = savedConfigs.concat([{file: file.toString(), yaml: importedYaml}]) }
    }
    NativeRvizPanel {
        id: panel
        width: 360; height: parent.height
        client: backend
        treeScope: "views"
    }
    NativeRvizPanel {
        id: shared
        x: 380; width: 360; height: parent.height
        client: backend
        showConnection: false; showPreview: false
    }
    QtObject {
        id: viewController
        property var canvas: viewCanvas
        property var cameraLabels: ["Overview", "Chase"]
        property int cameraIndex: 0
        property color sceneBackground: "#334455"
        property bool transparentCanvas: false
        property bool capturing: false
        property var mainView: ({pointSize: 1.5, width: 1})
        property var speedGauge: ({visible: false})
        property var videoLayers: []
        property var sceneLayers: []
        property var layerMotions: []
        signal recordingChanged()
        property var cloudViews: [{id: "orbit-a", label: "Native Orbit", width: .32}]
        property var tfFrames: []
        property var topicDisplays: []
        property bool loading: false
        property var lastSettings: ({})
        function setCloudView(id, settings) { lastSettings = settings }
        function addCloudView(camera) { cloudViews = cloudViews.concat([{id: "new-view", label: "New camera", width: .32}]) }
        function removeCloudView(id) { cloudViews = cloudViews.filter(view => view.id !== id) }
    }
    QtObject {
        id: viewCanvas
        property var layers: []
        property int editDepth: 0
        function beginEdit() { editDepth++ }
        function endEdit() { editDepth-- }
        function edit(id, changes) {
            const next = layers.filter(row => row.id !== id)
            layers = next.concat([Object.assign({id: id}, changes)])
        }
    }
    QtObject {
        id: editorCoordinator
        property bool syncPending: false
        property string syncError: ""
        property var classes: []
        function classForId(id) { return "rviz/Orbit" }
        function setViewClass(id, pluginClass) { classes = classes.concat([{id: id, class: pluginClass}]) }
    }
    QtObject {
        id: canvasFixture
        property real pointSize: 1.5
        property bool showPath: true
        property var nativeViewCoordinator: editorCoordinator
    }
    CloudInsetEditor {
        id: nativeEditor
        width: 340
        visible: false
        nativeEnabled: false
        controller: viewController
        nativeClient: backend
        nativeCoordinator: editorCoordinator
    }
    TestCase {
        name: "NativePropertyScope"
        when: windowShown
        function property(id, name, type, depth, children) {
            return {id: id, name: name, native_type: type, depth: depth, children: children || [],
                value: "", value_type: "QString", enabled: true, editable: false,
                read_only: false, checkable: false, expanded: true, display_value: ""}
        }
        function init() {
            const value = property("vf", "Plugin-provided camera number", "rviz::FloatProperty", 2)
            value.value = 8.5; value.value_type = "float"; value.editable = true
            const controller = property("vc", "Current View", "rviz::OrbitViewController", 1, [value])
            const view = property("v", "", "rviz::ViewControllerContainer", 0, [controller])
            const display = property("d", "Renamed display root", "rviz::DisplayGroup", 0)
            const toolValue = property("tt", "Tool-specific property", "rviz::Property", 1)
            const tools = property("t", "", "rviz::Property", 0, [toolValue])
            backend.properties = [display, view, controller, value, tools, toolValue]
            backend.selectedId = ""; backend.selected = ({})
            backend.edits = []; backend.deletes = 0; backend.previews = 0
            backend.connected = true; backend.savedConfigs = []; offline.savedConfigs = []
            backend.viewControllers = [{class: "rviz/Orbit"}, {class: "rviz/TopDownOrtho"}]
            backend.starts = 0; backendPreference.renderBackend = "independent"
            backend.resetViews = []
            nativeEditor.nativeEnabled = false; nativeEditor.visible = false
            nativeEditor.width = 340
            appearance.mode = 0
            viewController.cloudViews = [{id: "orbit-a", label: "Native Orbit", width: .32}]
            viewController.cameraLabels = ["Overview", "Chase"]
            viewController.lastSettings = ({})
            viewCanvas.layers = []; viewCanvas.editDepth = 0
            editorCoordinator.syncPending = false; editorCoordinator.classes = []
            routeWindow.nativeBackendSelected = false
            shared.treeScope = "all"
            panel.width = 360; panel.height = fixture.height
            panel.treeScope = "views"
            panel.viewIdFilter = ""
            panel.showPreview = true
            panel.addingDisplay = true
            panel.displaySelection = ["d"]
            backend.modelChanged()
        }
        function cleanup() {
            if (fixture.cameraHost) {
                fixture.cameraHost.destroy()
                fixture.cameraHost = null
            }
        }
        function createMainCameraHost(width, height) {
            const scrollId = mainUiSource.indexOf("id: settingsScroll")
            const scrollStart = mainUiSource.lastIndexOf("StudioScrollView {", scrollId)
            const contentStart = mainUiSource.indexOf("ColumnLayout {", scrollId)
            const contentSpacing = mainUiSource.indexOf("spacing: 9", contentStart) + "spacing: 9".length
            const cameraBinding = mainUiSource.indexOf("visible: settingsSection.currentIndex === 2")
            const cameraStart = mainUiSource.lastIndexOf("ColumnLayout {", cameraBinding)
            const nextBinding = mainUiSource.indexOf("visible: settingsSection.currentIndex === 6", cameraBinding)
            const cameraEnd = mainUiSource.lastIndexOf("ColumnLayout {", nextBinding)
            verify(scrollStart >= 0 && contentStart > scrollStart && cameraStart > contentSpacing && cameraEnd > cameraStart)
            const definitions = mainUiSource.match(/component (?:PanelTitle|Caption):[^\r\n]+/g) || []
            compare(definitions.length, 2)
            let production = mainUiSource.slice(scrollStart, contentSpacing) + '\n'
                + mainUiSource.slice(cameraStart, cameraEnd)
            // Expand the actual Main inline types for this dynamic host.
            for (const definition of definitions) {
                const type = definition.match(/component (\w+):\s*(\w+)\s*\{(.*)\}/)
                verify(type !== null)
                production = production.replace(new RegExp('\\b' + type[1] + '\\s*\\{', 'g'),
                                                type[2] + ' {' + type[3] + ';')
            }
            const host = Qt.createQmlObject('import QtQuick; import QtQuick.Controls; import QtQuick.Layouts; import "../../qml";'
                + 'Item { property var replay: viewController; property var nativeRviz: backend;'
                + 'property var canvas: canvasFixture; property var window: routeWindow;'
                + 'Item { id: settingsSection; height: 0; property int currentIndex: 2 }'
                + production + '\n} } }', fixture, "MainCameraHost.qml")
            fixture.cameraHost = host
            host.width = width; host.height = height
            verify(waitForPolish(host))
            waitForRendering(host)
            return host
        }
        function test_scopeUsesActualRoots() {
            compare(panel.scopedProperties.map(row => row.id), ["v", "vc", "vf"])
            compare(findChild(panel, "nativePropertyTree").count, 3)
            panel.treeScope = "displays"
            compare(panel.scopedProperties.map(row => row.id), ["d"])
            panel.treeScope = "tools"
            compare(panel.scopedProperties.map(row => row.id), ["t", "tt"])
            panel.treeScope = "all"
            compare(panel.scopedProperties.map(row => row.id), ["d", "v", "vc", "vf", "t", "tt"])
            compare(backend.properties.length, 6)
        }
        function test_additionalViewControllersUseActualViewIdentity() {
            const extraValue = property("ev", "Distance", "rviz::FloatProperty", 1)
            extraValue.view_id = "orbit-a"
            const extra = property("e", "Orbit", "rviz::OrbitViewController", 0, [extraValue])
            extra.view_id = "orbit-a"
            backend.properties = backend.properties.concat([extra, extraValue])
            compare(panel.scopedProperties.map(row => row.id), ["v", "vc", "vf", "e", "ev"])
            panel.viewIdFilter = "orbit-a"
            compare(panel.scopedProperties.map(row => row.id), ["e", "ev"])
            backend.select("vf")
            verify(!panel.selection.id)
            backend.select("ev")
            compare(panel.selection.id, "ev")
            panel.viewIdFilter = "main"
            compare(panel.scopedProperties.map(row => row.id), ["v", "vc", "vf"])
            verify(!panel.selection.id)
        }
        function test_nativeInsetInspectorEditsActualCameraOnly() {
            nativeEditor.nativeEnabled = true; nativeEditor.visible = true
            verify(findChild(nativeEditor, "nativeViewCapabilities").visible)
            verify(!findChild(nativeEditor, "viewApply").visible)
            compare(nativeEditor.choices.map(choice => choice.class), ["rviz/Orbit", "rviz/TopDownOrtho"])
            const properties = findChild(nativeEditor, "nativeInsetProperties")
            verify(properties.active)
            compare(properties.item.viewIdFilter, "orbit-a")
            mouseClick(findChild(nativeEditor, "nativeInsetReset"))
            compare(backend.resetViews, ["orbit-a"])
            const picker = findChild(nativeEditor, "nativeInsetClass")
            picker.chosen("rviz/TopDownOrtho")
            compare(editorCoordinator.classes, [{id: "orbit-a", class: "rviz/TopDownOrtho"}])
            editorCoordinator.syncPending = true
            verify(!picker.enabled)
        }
        function test_nativeAddCreatesStableCanvasIdAndInstalledClass() {
            nativeEditor.nativeEnabled = true; nativeEditor.visible = true
            findChild(nativeEditor, "viewPreset").currentIndex = 1
            const add = findChild(nativeEditor, "viewAdd")
            verify(add.enabled)
            waitForRendering(nativeEditor)
            mouseClick(add)
            compare(viewController.cloudViews.length, 2)
            compare(nativeEditor.selectedId, "new-view")
            compare(editorCoordinator.classes, [{id: "new-view", class: "rviz/TopDownOrtho"}])
            compare(viewController.cloudViews[1].id, "new-view")
            compare(viewCanvas.layers[0].name, "rviz/TopDownOrtho", "Default name comes from the installed plugin, not the legacy rear preset")
            compare(viewCanvas.editDepth, 0)
        }
        function test_cameraSelectionUsesCanvasNames() {
            viewCanvas.layers = [{id: "orbit-a", name: "Research overview"}]
            nativeEditor.nativeEnabled = true; nativeEditor.visible = true
            const picker = findChild(nativeEditor, "viewSelection")
            tryCompare(picker, "currentText", "Research overview")
            viewCanvas.layers = [{id: "orbit-a", name: "Renamed experiment camera"}]
            tryCompare(picker, "currentText", "Renamed experiment camera")
        }
        function test_cameraSelectionSurvivesUndo_data() {
            const rows = []
            for (const existing of [false, true]) {
                for (const layersFirst of [false, true]) {
                    for (const separateTurn of [false, true]) {
                        rows.push({tag: [existing, layersFirst, separateTurn].join("-"),
                            existing: existing, layersFirst: layersFirst, separateTurn: separateTurn})
                    }
                }
            }
            return rows
        }
        function test_cameraSelectionSurvivesUndo(data) {
            nativeEditor.nativeEnabled = true; nativeEditor.visible = true
            nativeEditor.selectedId = ""
            viewController.cloudViews = data.existing ? [{id: "orbit-a", label: "后方跟随", width: .32}] : []
            viewCanvas.layers = data.existing ? [{id: "orbit-a", name: "Orbit overview"}] : []
            backend.viewControllers = [{name: "Top-down", class: "rviz/TopDownOrtho"}]
            const picker = findChild(nativeEditor, "viewSelection")
            findChild(nativeEditor, "viewPreset").currentIndex = 0
            waitForRendering(nativeEditor)
            mouseClick(findChild(nativeEditor, "viewAdd"))
            const savedViews = viewController.cloudViews.map(view => Object.assign({}, view, {label: "后方跟随"}))
            const savedLayers = viewCanvas.layers.slice()
            function verifyRestored(stage) {
                compare(nativeEditor.chosen.id, "new-view")
                if (picker.currentIndex !== (data.existing ? 1 : 0))
                    console.log("Selection mismatch", stage, nativeEditor.selectedId, JSON.stringify(nativeEditor.selectionChoices),
                                picker.currentIndex, picker.currentText, picker.displayText)
                tryCompare(picker, "currentIndex", data.existing ? 1 : 0, 500)
                tryCompare(picker, "currentText", "Top-down", 500)
                tryCompare(picker, "displayText", "Top-down", 500)
                tryCompare(picker.contentItem, "text", "Top-down", 500)
            }
            verifyRestored("added")
            for (let cycle = 0; cycle < 2; cycle++) {
                mouseClick(findChild(nativeEditor, "nativeInsetRemove"))
                viewCanvas.layers = viewCanvas.layers.filter(layer => layer.id !== "new-view")
                tryCompare(picker, "count", data.existing ? 1 : 0, 500)
                // Undo restores two independently notified production models;
                // neither their notification order nor a render turn may leave
                // the real ComboBox text detached from the restored view id.
                if (data.layersFirst) viewCanvas.layers = savedLayers.slice()
                else viewController.cloudViews = savedViews.slice()
                if (data.separateTurn) wait(0)
                if (data.layersFirst) viewController.cloudViews = savedViews.slice()
                else viewCanvas.layers = savedLayers.slice()
                verifyRestored("undo " + cycle)
            }
            if (data.existing) {
                for (const index of [0, 1]) {
                    mouseClick(picker)
                    tryCompare(picker.popup, "visible", true, 500)
                    const list = picker.popup.contentItem
                    list.positionViewAtIndex(index, ListView.Contain)
                    waitForRendering(list)
                    const option = list.itemAtIndex(index)
                    verify(option !== null)
                    mouseClick(option, option.width * .6, option.height / 2)
                    tryCompare(picker.popup, "visible", false, 500)
                    compare(nativeEditor.selectedId, index === 0 ? "orbit-a" : "new-view")
                    compare(picker.contentItem.text, index === 0 ? "Orbit overview" : "Top-down")
                }
                verifyRestored("activated after undo")
            }
        }
        function test_nativeAddFitsNarrowInspector_data() {
            const rows = []
            for (const width of [216, 256]) {
                for (const mode of [0, 1]) {
                    for (const plugin of ["rviz/TopDownOrtho", "vendor_camera_plugins/LongInstalledTopDownOrthographicController"]) {
                        rows.push({tag: width + "-" + mode + "-" + plugin, width: width, mode: mode, plugin: plugin})
                    }
                }
            }
            return rows
        }
        function test_nativeAddFitsNarrowInspector(data) {
            appearance.mode = data.mode
            backend.viewControllers = [{class: data.plugin}]
            nativeEditor.width = data.width
            nativeEditor.nativeEnabled = true; nativeEditor.visible = true
            const preset = findChild(nativeEditor, "viewPreset")
            const add = findChild(nativeEditor, "viewAdd")
            tryCompare(preset, "count", 1)
            preset.currentIndex = 0
            verify(waitForPolish(nativeEditor), "Native view layout finishes polishing")
            waitForRendering(nativeEditor)
            for (const item of [preset, add]) {
                const origin = item.mapToItem(nativeEditor, 0, 0)
                verify(origin.x >= -1 && origin.x + item.width <= nativeEditor.width + 1,
                       item.objectName + " [" + origin.x + ", " + item.width + "] exceeds Inspector "
                       + nativeEditor.width + " with " + data.plugin)
            }
            verify(preset.width > preset.leftPadding + preset.rightPadding, "Preset remains usable")
            verify(add.width >= add.implicitWidth - 1, "Add button keeps its complete control width")
            const center = add.mapToItem(nativeEditor, add.width / 2, add.height / 2)
            verify(center.x >= 0 && center.x < nativeEditor.width, "Add button center is inside the Inspector")
            mouseClick(add, add.width / 2, add.height / 2)
            compare(viewController.cloudViews.length, 2)
            compare(editorCoordinator.classes, [{id: "new-view", class: data.plugin}])
        }
        function test_mainCameraHostFitsViewport_data() {
            const rows = []
            for (const width of [216, 256, 280]) {
                for (const mode of [0, 1]) {
                    for (const plugin of ["rviz/TopDownOrtho", "vendor_camera_plugins/LongInstalledTopDownOrthographicController"]) {
                        rows.push({tag: width + "-" + mode + "-" + plugin, width: width, mode: mode, plugin: plugin, native: true})
                    }
                    rows.push({tag: width + "-" + mode + "-independent", width: width, mode: mode, plugin: "", native: false})
                }
            }
            return rows
        }
        function test_mainCameraHostFitsViewport(data) {
            appearance.mode = data.mode
            backend.viewControllers = [{class: data.plugin}]
            routeWindow.nativeBackendSelected = data.native
            if (!data.native) viewController.cameraLabels = ["Overview", "Long installed compatible chase camera · 兼容跟随机位"]
            // Use the production ScrollView, its content ColumnLayout and the
            // entire Camera section. An isolated editor width misses minimum
            // size propagation from its real parent and sibling layouts.
            const host = createMainCameraHost(data.width, 900)
            // QML ids are not QObject names; get the actual production editor
            // through the already named Add control and its owning pane.
            const add = findChild(host, "viewAdd")
            verify(add !== null)
            const pane = add.parent.parent
            verify(waitForPolish(host), "Main Camera host finishes polishing")
            waitForRendering(host)
            const viewportWidth = data.width - 24
            const origin = pane.mapToItem(host, 0, 0)
            verify(pane.width <= viewportWidth + 1,
                   "Main Camera editor " + pane.width + " exceeds viewport " + viewportWidth)
            for (const container of [findChild(host, "cameraSettingsSection"), findChild(host, "mainCameraSettings"), pane]) {
                verify(container !== null)
                for (const child of container.children) {
                    if (!child.visible || child.width === undefined) continue
                    const pos = child.mapToItem(host, 0, 0)
                    verify(pos.x >= 11 && pos.x + child.width <= host.width - 11,
                           container.objectName + " child " + child + " [" + pos.x + ", " + child.width
                           + "] exceeds Main viewport " + viewportWidth)
                }
            }
            for (const name of ["viewPreset", "viewAdd", "viewSelection", "targetFramePicker", "viewFollowMode",
                                "viewLag", "viewSmoothing", "viewPointSize", "viewCloudColor", "viewPathColor",
                                "viewBackground", "viewApply", "nativeInsetClass", "nativeInsetWidth", "nativeInsetReset", "nativeInsetRemove"]) {
                const item = findChild(host, name)
                verify(item !== null)
                if (!item.visible) continue
                const pos = item.mapToItem(host, 0, 0)
                verify(pos.x >= 11 && pos.x + item.width <= host.width - 11,
                       name + " [" + pos.x + ", " + item.width + "] exceeds Main viewport " + viewportWidth)
            }
            const preset = findChild(host, "viewPreset")
            preset.currentIndex = 0
            verify(add.width >= add.implicitWidth - 1, "Main Add button retains complete width")
            // Scroll to the Add row before the real center click; the viewport
            // clipping must participate in hit testing, as it does in Main.
            let ancestor = pane.parent
            while (ancestor && ancestor.contentY === undefined) ancestor = ancestor.parent
            verify(ancestor !== null, "Camera editor has the real ScrollView Flickable ancestor")
            ancestor.contentY = Math.max(0, origin.y - 12)
            verify(waitForPolish(host))
            mouseClick(add, add.width / 2, add.height / 2)
            compare(viewController.cloudViews.length, 2)
            compare(editorCoordinator.classes, data.native ? [{id: "new-view", class: data.plugin}] : [])
        }
        function test_embeddedInspectorScrollsToApply_data() {
            const rows = []
            for (const mode of [0, 1])
                for (const width of [216, 280])
                    rows.push({tag: mode + "-" + width, mode: mode, width: width})
            return rows
        }
        function test_embeddedInspectorScrollsToApply(data) {
            appearance.mode = data.mode
            routeWindow.nativeBackendSelected = true
            backend.select("vf")
            const host = createMainCameraHost(data.width, 650)
            const outer = findChild(host, "workspaceSettingsScroll")
            const nativePane = findChild(host, "nativeCameraInspector")
            const inner = findChild(nativePane, "nativePanelScroll")
            const apply = findChild(nativePane, "nativePropertyApply")
            const editor = findChild(nativePane, "nativePropertyEditor")
            const before = nativePane.height
            // A real installed controller can provide long rich text help.
            // Growing it after selection must update the outer scroll range.
            backend.selected = Object.assign({}, backend.selected, {
                description: ("<p>Native Scale property help with units and camera behavior. "
                    + "缩放属性来自当前插件，长说明后仍可编辑和应用。</p>").repeat(12)})
            backend.modelChanged()
            verify(waitForPolish(host))
            tryVerify(() => nativePane.height > before + 100,
                      1000, "Native panel must grow with its property help: " + before + " -> " + nativePane.height
                      + ", implicit " + nativePane.implicitHeight + ", selection " + nativePane.selection.id
                      + ", help " + (nativePane.selection.description || "").length)
            compare(nativePane.height, nativePane.implicitHeight)
            verify(!inner.wheelEnabled, "The outer Inspector owns wheel scrolling")
            verify(!inner.contentItem.interactive, "The embedded panel must not steal touch dragging")
            compare(inner.contentItem.contentY, 0)
            compare(outer.contentItem.contentY, 0)
            let visible = false
            for (let step = 0; step < 20; ++step) {
                const point = apply.mapToItem(outer, 0, 0)
                visible = point.y >= 0 && point.y + apply.height <= outer.height
                if (visible) break
                // Real wheel events land on the production panel's visible
                // help area and must continue into the outer ScrollView.
                mouseWheel(outer, outer.width - 18, outer.height - 24, 0, -120)
                wait(120)
            }
            verify(visible, "Apply must become reachable using the outer Inspector wheel; outer Y "
                   + outer.contentItem.contentY + ", inner Y " + inner.contentItem.contentY
                   + ", Apply Y " + apply.mapToItem(outer, 0, 0).y + ", viewport " + outer.height)
            verify(outer.contentItem.contentY > 0)
            compare(inner.contentItem.contentY, 0)
            tryVerify(() => !outer.contentItem.moving)
            waitForRendering(host)
            editor.text = "23.75"
            mouseClick(apply, apply.width / 2, apply.height / 2)
            compare(backend.edits, [{id: "vf", value: 23.75}], "The visible production Apply button must receive input")
        }
        function test_standalonePanelRetainsInternalScroll() {
            panel.width = 280; panel.height = 300
            backend.select("vf")
            const scroll = findChild(panel, "nativePanelScroll")
            verify(waitForPolish(panel))
            compare(panel.implicitHeight, 640)
            verify(scroll.wheelEnabled)
            verify(scroll.contentItem.contentHeight > scroll.height)
            const bar = scroll.ScrollBar.vertical
            verify(bar.visible)
            mouseDrag(bar.contentItem, bar.contentItem.width / 2, bar.contentItem.height / 2, 0, 60)
            tryVerify(() => scroll.contentItem.contentY > 0)
        }
        function test_additionalViewInspectorUsesNaturalHeight() {
            nativeEditor.nativeEnabled = true; nativeEditor.visible = true
            const properties = findChild(nativeEditor, "nativeInsetProperties")
            verify(properties.active)
            const extraValue = property("ev", "Scale", "rviz::FloatProperty", 1)
            extraValue.view_id = "orbit-a"; extraValue.editable = true
            const extra = property("e", "Orbit", "rviz::OrbitViewController", 0, [extraValue])
            extra.view_id = "orbit-a"
            backend.properties = backend.properties.concat([extra, extraValue])
            backend.select("ev")
            verify(waitForPolish(nativeEditor))
            const before = properties.height
            backend.selected = Object.assign({}, backend.selected, {description: "<p>Camera Scale documentation · 原生相机说明</p>".repeat(18)})
            backend.modelChanged()
            verify(waitForPolish(nativeEditor))
            tryVerify(() => properties.height > before + 100, 1000,
                      "Inset panel must grow: " + before + " -> " + properties.height
                      + ", implicit " + properties.item.implicitHeight + ", selection " + properties.item.selection.id)
            compare(properties.height, properties.item.implicitHeight)
            verify(!findChild(properties.item, "nativePanelScroll").wheelEnabled)
        }
        function test_narrowScopeLayouts_data() {
            return [{tag: "all", scope: "all"}, {tag: "views", scope: "views"}]
        }
        function test_narrowScopeLayouts(data) {
            panel.width = 260
            panel.treeScope = data.scope
            panel.showPreview = false
            panel.addingDisplay = false
            wait(60)
            for (const name of ["nativeDistribution", "nativeConnect", "nativeTreeHeading",
                                "nativePropertyEditor", "nativePropertyApply"]) {
                const item = findChild(panel, name)
                if (!item.visible) continue
                const origin = item.mapToItem(panel, 0, 0)
                verify(origin.x >= -1 && origin.x + item.width <= panel.width + 1, name + " overflows " + data.scope)
            }
        }
        function test_viewsHideDisplayAndConnectionTools() {
            verify(!findChild(panel, "nativeConnection").visible)
            verify(!findChild(panel, "nativeAddToggle").visible)
            verify(!findChild(panel, "nativePreview").visible)
            const tree = findChild(panel, "nativePropertyTree")
            tree.forceActiveFocus()
            keyClick(Qt.Key_Delete)
            compare(backend.deletes, 0)
            panel.addByTopic({name: "/unrelated"})
            compare(panel.selectedTopic, "")
            wait(340)
            compare(backend.previews, 0)
        }
        function test_searchCanOmitOtherRoots() {
            backend.properties = backend.properties.slice(1, 4)
            backend.modelChanged()
            compare(panel.scopedProperties.map(row => row.id), ["v", "vc", "vf"])
            panel.treeScope = "displays"
            compare(panel.scopedProperties.length, 0)
        }
        function test_sharedSelectionAndNativeEditor() {
            backend.select("vf")
            compare(panel.selection.id, "vf")
            compare(shared.selection.id, "vf")
            const editor = findChild(panel, "nativePropertyEditor")
            compare(editor.text, "8.5")
            verify(editor.enabled)
            editor.text = "12.75"
            panel.applyValue()
            compare(backend.edits, [{id: "vf", value: 12.75}])
            backend.select("d")
            compare(shared.selection.id, "d")
            verify(!panel.selection.id)
            verify(!editor.enabled)
            compare(editor.text, "")
            compare(backend.selectedId, "d")
            // Collapsing the shared tree must not create an independent selection
            // or lose the selected native child from this Inspector.
            backend.select("vf")
            backend.properties = [backend.properties[0], backend.properties[1], backend.properties[4]]
            backend.modelChanged()
            compare(panel.selection.id, "vf")
        }
        function test_mainPanelExportRouting_data() {
            return [
                {tag: "native-active", selected: true, connected: true, nativeSave: true},
                {tag: "independent-selected", selected: false, connected: true, nativeSave: false},
                {tag: "native-unavailable", selected: true, connected: false, nativeSave: false}
            ]
        }
        function test_mainSelectionRoutesActualPropertyRoot_data() {
            return [
                {tag: "main-view", id: "vf", scope: "views", section: 2, initial: 2, scroll: 87},
                {tag: "additional-view", id: "ev", scope: "views", section: 2, initial: 2, scroll: 87, view: "orbit-a"},
                {tag: "view-from-display", id: "vf", scope: "views", section: 2, initial: 5, scroll: 0},
                {tag: "display", id: "d", scope: "displays", section: 5, initial: 2, scroll: 0},
                {tag: "tool", id: "tt", scope: "tools", section: 5, initial: 2, scroll: 0}
            ]
        }
        function test_mainSelectionRoutesActualPropertyRoot(data) {
            const source = mainUiSource.replace(/\r\n/g, "\n")
            const start = source.indexOf("        function onModelChanged()")
            const end = source.indexOf("        function onConfigSnapshotReady(", start)
            verify(start >= 0 && end > start, "The real Main selection handler must be available")
            const extraValue = property("ev", "Vendor-specific number", "rviz::FloatProperty", 1)
            extraValue.view_id = "orbit-a"; extraValue.editable = true
            const extra = property("e", "Custom installed camera", "vendor::ActualViewController", 0, [extraValue])
            extra.view_id = "orbit-a"
            backend.properties = backend.properties.concat([extra, extraValue])
            routeSection.currentIndex = data.initial; routeScrollContent.contentY = 87
            nativeEditor.selectedId = ""
            fixture.cameraHost = Qt.createQmlObject('import QtQml; QtObject {'
                + 'property var nativeRviz: backend; property var nativeInspector: shared;'
                + 'property var settingsSection: routeSection; property var settingsScroll: routeScroll;'
                + 'property var insetEditor: nativeEditor;'
                + 'property var handler: Connections { target: backend; property string lastSelected: "";'
                + source.slice(start, end) + '\n} }', fixture, "ProductionNativeSelectionRoute")
            const clickedPanel = data.scope === "views" ? panel : shared
            clickedPanel.treeScope = data.scope
            clickedPanel.viewIdFilter = data.view || ""
            const tree = findChild(clickedPanel, "nativePropertyTree")
            const row = clickedPanel.scopedProperties.findIndex(item => item.id === data.id)
            verify(row >= 0)
            tryVerify(() => tree.itemAtIndex(row) !== null)
            const delegate = tree.itemAtIndex(row)
            waitForRendering(clickedPanel)
            mouseClick(delegate, delegate.width * .6, delegate.height / 2)
            compare(backend.selectedId, data.id, "The real property row must receive the click")
            compare(routeSection.currentIndex, data.section, "Camera properties must stay in Camera; displays and tools use their actual scope")
            compare(routeScrollContent.contentY, data.scroll, "Editing another property within the same Camera must not jump to the top")
            if (data.scope !== "views") compare(shared.treeScope, data.scope)
            if (data.view) compare(nativeEditor.selectedId, data.view)
        }
        function test_mainRestoresRequestedBackendAndKeepsFallbackHonest() {
            const selected = mainUiSource.match(/property bool nativeBackendSelected:\s*[^\r\n]+/)
            const active = mainUiSource.match(/readonly property bool nativeBackendActive:\s*[^\r\n]+/)
            const restoring = mainUiSource.match(/function onRenderBackendChanged\(\)\s*\{[\s\S]*?\n\s*\}/)
            const activation = mainUiSource.match(/onActivated:\s*\{ replay\.renderBackend[^\r\n]+/)
            verify(selected && active && restoring && activation, "Production Main backend preference bindings are missing")
            const shell = Qt.createQmlObject('import QtQml; QtObject { id: shell;'
                + 'property var replay: backendPreference; property var nativeRviz: backend; property bool automaticNativeBackend: true;'
                + selected[0] + '\n' + active[0] + '\n'
                + 'property var restoring: Connections { target: shell.replay; ' + restoring[0] + ' } }', fixture)
            backend.connected = false
            backendPreference.renderBackend = "native-rviz"
            compare(shell.nativeBackendSelected, true)
            compare(shell.nativeBackendActive, false)
            compare(backend.starts, 1)
            compare(backendPreference.renderBackend, "native-rviz", "Unavailable runtime must not overwrite project preference")
            backend.connected = true
            compare(shell.nativeBackendActive, true)
            backend.connected = false
            compare(shell.nativeBackendActive, false)
            compare(shell.nativeBackendSelected, true)
            const picker = Qt.createQmlObject('import QtQml; QtObject { property var replay: backendPreference;'
                + 'property var nativeRviz: backend; property int currentIndex: 0; signal activated(int index);'
                + activation[0] + '\n}', fixture)
            picker.activated(0)
            compare(backendPreference.renderBackend, "independent")
            compare(shell.nativeBackendSelected, false)
            picker.currentIndex = 1; picker.activated(1)
            compare(backendPreference.renderBackend, "native-rviz")
            compare(shell.nativeBackendSelected, true)
            picker.destroy(); shell.destroy()
        }
        function test_mainPanelExportRouting(data) {
            // Exercise the real Main.qml signal bindings without substituting a
            // copied routing implementation or pretending to load a ROS scene.
            const bindings = mainUiSource.match(/onExportRequested:\s*[^\r\n]+/g) || []
            compare(bindings.length, 3, "Every native panel export entry must be covered")
            routeWindow.nativeBackendSelected = data.selected
            backend.connected = data.connected
            for (let index = 0; index < bindings.length; ++index) {
                const forwarder = Qt.createQmlObject(
                    'import QtQml; QtObject {'
                    + 'property var window: routeWindow; property var nativeRviz: backend; property var replay: offline;'
                    + 'signal exportRequested(url file); ' + bindings[index] + '\n}', fixture)
                const file = Qt.resolvedUrl('route-' + index + '.rviz')
                forwarder.exportRequested(file)
                forwarder.destroy()
                const actual = data.nativeSave ? backend.savedConfigs : offline.savedConfigs
                compare(actual.length, index + 1)
                compare(actual[index].file, file.toString())
                compare(actual[index].yaml, data.nativeSave ? backend.editedYaml : offline.importedYaml)
                compare(data.nativeSave ? offline.savedConfigs.length : backend.savedConfigs.length, 0,
                        "Export must not silently save the stale configuration from the other backend")
            }
        }
    }
}
