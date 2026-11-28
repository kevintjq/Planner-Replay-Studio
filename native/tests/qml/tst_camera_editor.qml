import QtQuick
import QtTest
import "../../qml"

TestCase {
    id: suite
    name: "CameraEditor"
    width: 380
    height: 1100
    visible: true
    when: windowShown

    // An editor interaction fixture, not ROS/TF reception or GPU acceptance.
    QtObject {
        id: controller
        property var cloudViews: [{id: "inset", label: "Rear", camera: 1, width: .32, zoom: 1,
                                   transparent: true, overlays: ["actual"], targetFrame: "gimbal",
                                   hiddenDisplays: ["hidden"]}]
        property var cameraLabels: ["Overview", "Rear", "Oblique"]
        property var tfFrames: ["map", "gimbal"]
        property var topicDisplays: [{id: "path", label: "Path"}, {id: "hidden", label: "Other"}]
        property bool loading: false
        property var applied: ({})
        function addCloudView(camera) {}
        function removeCloudView(id) {}
        function setCloudView(id, settings) { applied = settings }
    }
    CloudInsetEditor {
        id: editor
        x: 10; y: 10; width: 340
        controller: controller
        pointSize: 2.5
    }

    function init() {
        controller.applied = ({})
        appearance.mode = 0
    }
    function test_duplicatePresetAllowed() {
        compare(editor.choices.length, 2)
        compare(editor.choices[0].camera, 1)
    }
    function test_applyCameraSettings() {
        const target = findChild(editor, "targetFramePicker")
        verify(target !== null)
        target.chosen("map")
        findChild(editor, "viewFollowMode").currentIndex = 3
        findChild(editor, "viewLag").text = "0.25"
        findChild(editor, "viewSmoothing").text = "0.5"
        findChild(editor, "viewPointSize").text = "3"
        findChild(editor, "viewCloudColor").text = "#adbecf"
        findChild(editor, "viewPathColor").text = "#bb7744"
        findChild(editor, "viewBackground").text = "#32363a"
        const apply = findChild(editor, "viewApply")
        verify(apply.enabled)
        mouseClick(apply)
        compare(controller.applied.targetFrame, "map")
        compare(controller.applied.followMode, 3)
        compare(controller.applied.followLag, .25)
        compare(controller.applied.smoothing, .5)
        compare(controller.applied.pointSize, 3)
        compare(controller.applied.cloudColor, "#adbecf")
        compare(controller.applied.pathColor, "#bb7744")
        compare(controller.applied.viewBackground, "#32363a")
        compare(controller.applied.hiddenDisplays.length, 1)
        compare(controller.applied.hiddenDisplays[0], "hidden")
        findChild(editor, "viewLag").text = "NaN"
        verify(!apply.enabled, "Invalid numeric input must not reach persisted camera settings")
        findChild(editor, "viewLag").text = "0"
    }
    function test_fitsInspector_data() {
        return [{tag: "light", mode: 0}, {tag: "dark", mode: 1}]
    }
    function test_fitsInspector(data) {
        appearance.mode = data.mode
        wait(30)
        verify(editor.implicitWidth <= editor.width, "Camera settings exceed inspector width")
        for (let i = 0; i < editor.children.length; ++i) {
            const child = editor.children[i]
            if (child.visible && child.width !== undefined)
                verify(child.x + child.width <= editor.width + 1, "Camera editor child overflows")
        }
    }
}
