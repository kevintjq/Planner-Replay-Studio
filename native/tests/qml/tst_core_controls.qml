import QtQuick
import QtTest
import "../../qml"

TestCase {
    id: suite
    name: "CoreUiControls"
    width: 480
    height: 700
    visible: true
    when: windowShown
    property string chosenValue: ""

    StudioSearchComboBox {
        id: picker
        x: 10; y: 10; width: 216
        sourceModel: ["/cloud", "/odometry", "/tf"]
        selectedText: "/odometry"
        onChosen: value => suite.chosenValue = value
    }
    // Layout fixture only: no simulated ROS reception or rendering is certified here.
    QtObject {
        id: inspectorData
        property var topics: ["/cloud", "/tf"]
        property string selectedTopic: ""
        property string selectedType: ""
        property var lastInspection: []
        property bool working: false
        property string status: "Message status"
        property string fields: "field: 1\nfield_name_with_long_identifier: []"
        property url preview: ""
        property int arrayPage: 0
        property int arrayPageCount: 3
        signal changed()
        function inspect(topic, type) { selectedTopic = topic; selectedType = type || ""; lastInspection = [topic, type || ""] }
        function cancel() {}
        function setArrayPage(page) { arrayPage = page }
    }
    QtObject {
        id: controller
        property real position: 1
        property real duration: 2
        property bool loading: false
        function seek(seconds) { position = seconds }
        function pause() {}
    }
    TopicInspector {
        id: inspectorPane
        x: 240; y: 10; width: 216; height: 660
        inspector: inspectorData
        controller: controller
    }
    function init() {
        appearance.mode = 0
        picker.popup.close()
        suite.chosenValue = ""
        inspectorData.selectedTopic = ""
        inspectorData.selectedType = ""
        inspectorData.lastInspection = []
    }
    function test_searchKeyboard() {
        mouseClick(picker, 20, 15)
        tryCompare(picker.popup, "visible", true)
        const search = findChild(picker, "searchField")
        const choices = findChild(picker, "searchChoices")
        verify(search !== null && choices !== null)
        tryCompare(choices, "currentIndex", 1)
        keyClick(Qt.Key_Down)
        tryCompare(choices, "currentIndex", 2)
        keyClick(Qt.Key_Return)
        compare(suite.chosenValue, "/tf")
        tryCompare(picker.popup, "visible", false)
    }
    function test_emptySearchAndEscape() {
        mouseClick(picker, 20, 15)
        tryCompare(picker.popup, "visible", true)
        keyClick(Qt.Key_Z)
        keyClick(Qt.Key_Z)
        const choices = findChild(picker, "searchChoices")
        tryCompare(choices, "count", 0)
        keyClick(Qt.Key_Return)
        compare(suite.chosenValue, "")
        verify(picker.popup.visible)
        keyClick(Qt.Key_Escape)
        tryCompare(picker.popup, "visible", false)
    }
    function test_topicInspectorFitsNarrowPanel_data() {
        return [{ tag: "light", mode: 0 }, { tag: "dark", mode: 1 }]
    }
    function test_externalTopicSelectionAndReload() {
        const topicPicker = findChild(inspectorPane, "inspectTopicPicker")
        const reload = findChild(inspectorPane, "reloadTopicMessage")
        verify(topicPicker !== null && reload !== null)
        compare(reload.enabled, false)
        // Topic Browser and message Inspector share the backend selection,
        // including the type discriminator for bags with multiple connections.
        inspectorData.selectedTopic = "/cloud"
        inspectorData.selectedType = "sensor_msgs/PointCloud2"
        tryCompare(topicPicker, "selectedText", "/cloud")
        tryCompare(reload, "enabled", true)
        mouseClick(reload)
        compare(inspectorData.lastInspection, ["/cloud", "sensor_msgs/PointCloud2"])
        topicPicker.chosen("/tf")
        tryCompare(topicPicker, "selectedText", "/tf")
        inspectorData.selectedTopic = "/cloud"
        tryCompare(topicPicker, "selectedText", "/cloud")
        inspectorData.selectedTopic = ""
        tryCompare(reload, "enabled", false)
    }
    function test_topicInspectorFitsNarrowPanel(data) {
        appearance.mode = data.mode
        wait(50)
        verify(inspectorPane.implicitWidth <= 216, "Inspector minimum width exceeds narrow panel")
        for (let i = 0; i < inspectorPane.children.length; ++i) {
            const child = inspectorPane.children[i]
            if (child.visible && child.width !== undefined)
                verify(child.x + child.width <= inspectorPane.width + 1, "Inspector child overflows panel")
        }
    }
}
