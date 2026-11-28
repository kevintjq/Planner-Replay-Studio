import QtQuick
import QtTest

TestCase {
    id: suite
    name: "TrajectoryMaskInput"
    width: 540
    height: 420
    visible: true
    when: windowShown
    property var paintArea: null
    property var maskStatusLabel: null

    QtObject {
        id: editor
        property bool working: false
        property var calls: []
        property var maskImages: ({})
        function paintMask(points, radius, erase) {
            calls = calls.concat([{points: points.map(point => Qt.point(point.x, point.y)), radius: radius, erase: erase}])
        }
    }
    QtObject { id: pane; property var editor: suiteEditor() }
    function suiteEditor() { return editor }
    QtObject { id: brushRadius; property real value: .025 }
    QtObject { id: maskTool; property int currentIndex: 1 }
    Item {
        id: maskPicture
        x: 60; y: 60; width: 400; height: 300
        property real paintedWidth: 200
        property real paintedHeight: 100
        readonly property real leftEdge: (width-paintedWidth)/2
        readonly property real topEdge: (height-paintedHeight)/2
        property int status: Image.Ready
    }

    function initTestCase() {
        const imageStart = trajectoryPaneSource.indexOf("id: maskPicture")
        const inputStart = trajectoryPaneSource.indexOf("MouseArea {", imageStart)
        verify(imageStart >= 0 && inputStart >= 0, "Production mask input must be present")
        let depth = 0, end = inputStart
        for (; end < trajectoryPaneSource.length; ++end) {
            const character = trajectoryPaneSource[end]
            if (character === "{") ++depth
            if (character === "}" && --depth === 0) { ++end; break }
        }
        paintArea = Qt.createQmlObject("import QtQuick\n" + trajectoryPaneSource.slice(inputStart, end), maskPicture, "ProductionTrajectoryMaskInput")
        verify(paintArea !== null)
        waitForRendering(paintArea)
        const statusId = trajectoryPaneSource.indexOf("id: maskStatus")
        const statusStart = trajectoryPaneSource.lastIndexOf("StudioLabel {", statusId)
        verify(statusId >= 0 && statusStart >= 0, "Production mask status must be present")
        depth = 0; end = statusStart
        for (; end < trajectoryPaneSource.length; ++end) {
            const character = trajectoryPaneSource[end]
            if (character === "{") ++depth
            if (character === "}" && --depth === 0) { ++end; break }
        }
        maskStatusLabel = Qt.createQmlObject("import QtQuick\nimport QtQuick.Layouts\n" +
            trajectoryPaneSource.slice(statusStart, end).replace("StudioLabel {", "Text {"), suite, "ProductionTrajectoryMaskStatus")
        verify(maskStatusLabel !== null)
    }
    function init() {
        if (paintArea.pressed) mouseRelease(paintArea, 200, 150)
        paintArea.stroke = []; paintArea.drawing = false
        editor.calls = []; editor.working = false
        editor.maskImages = ({})
        maskTool.currentIndex = 1; brushRadius.value = .025
    }
    function test_maskStatus_data() {
        return [
            {tag: "automatic-visible", edited: false, available: true, pixels: 12, reason: "", origin: "自动结果", current: "含可见像素"},
            {tag: "automatic-empty", edited: false, available: true, pixels: 0, reason: "Automatic extraction failed", origin: "自动结果", current: "当前蒙版为空"},
            {tag: "manual-visible", edited: true, available: true, pixels: 7, reason: "Automatic extraction failed", origin: "已手动修改", current: "含可见像素"},
            {tag: "manual-empty", edited: true, available: true, pixels: 0, reason: "Automatic extraction failed", origin: "已手动修改", current: "当前蒙版为空"},
            {tag: "unavailable", edited: true, available: false, pixels: 0, reason: "Automatic extraction failed", origin: "已手动修改", current: "当前蒙版不可读取"}
        ]
    }
    function test_maskStatus(data) {
        editor.maskImages = {source: "image://trajectory/source", seconds: 1.25, manuallyEdited: data.edited,
            currentMaskAvailable: data.available, currentAlphaPixels: data.pixels, reason: data.reason}
        tryVerify(() => maskStatusLabel.text.includes(data.current))
        verify(maskStatusLabel.text.includes(data.origin))
        verify(maskStatusLabel.text.includes("1.250 s"))
        if (data.reason) verify(maskStatusLabel.text.includes("原自动提取原因 / Original automatic reason: " + data.reason))
        if (data.available && data.pixels > 0) verify(maskStatusLabel.text.includes("需检查边缘"))
        verify(!maskStatusLabel.text.includes("有效") && !maskStatusLabel.text.includes("干净"), "Alpha must not certify clean extraction")
    }
    function test_maskStatusWithoutSelection() {
        tryVerify(() => maskStatusLabel.text.includes("在下方缩略图选择一帧"))
    }
    function test_letterboxDoesNotPaint_data() {
        return [{tag: "left", x: 20, y: 150}, {tag: "right", x: 370, y: 150},
                {tag: "top", x: 200, y: 20}, {tag: "bottom", x: 200, y: 270}]
    }
    function test_letterboxDoesNotPaint(data) {
        mousePress(paintArea, data.x, data.y)
        mouseMove(paintArea, 200, 150)
        mouseRelease(paintArea, 200, 150)
        compare(editor.calls.length, 0, "A press in letterbox space must not become an edge or entering stroke")
    }
    function test_insideStrokeUsesSourceCoordinates() {
        mousePress(paintArea, 150, 125)
        mouseMove(paintArea, 250, 175)
        mouseRelease(paintArea, 250, 175)
        compare(editor.calls.length, 1)
        const call = editor.calls[0]
        compare(call.points[0], Qt.point(.25, .25))
        compare(call.points[call.points.length-1], Qt.point(.75, .75))
        compare(call.radius, .025); compare(call.erase, true)
    }
    function test_exitClipsAndDoesNotConnectOnReturn() {
        mousePress(paintArea, 200, 150)
        mouseMove(paintArea, 350, 175)
        compare(editor.calls.length, 1, "Exiting the image should finish exactly one stroke")
        mouseMove(paintArea, 150, 125)
        mouseRelease(paintArea, 150, 125)
        compare(editor.calls.length, 1, "Reentering must not draw a connecting line")
        const points = editor.calls[0].points
        fuzzyCompare(points[points.length-1].x, 1, 1.e-8)
        fuzzyCompare(points[points.length-1].y, 2/3, 1.e-8)
    }
    function test_diagonalExitStopsAtFirstBoundary() {
        mousePress(paintArea, 200, 150)
        mouseMove(paintArea, 0, -50)
        mouseRelease(paintArea, 0, -50)
        compare(editor.calls.length, 1)
        const points = editor.calls[0].points
        compare(points[points.length-1], Qt.point(.25, 0))
        for (const point of points)
            verify(point.x >= 0 && point.x <= 1 && point.y >= 0 && point.y <= 1)
    }
    function test_brushToolAndImageBoundary() {
        maskTool.currentIndex = 0; brushRadius.value = .01
        mousePress(paintArea, 300, 200)
        mouseRelease(paintArea, 280, 190)
        compare(editor.calls.length, 1)
        compare(editor.calls[0].points[0], Qt.point(1, 1))
        compare(editor.calls[0].erase, false); compare(editor.calls[0].radius, .01)
    }
}
