import QtQuick
import QtQuick.Controls
import QtTest
import "../../qml"

Item {
    id: fixture
    width: 1080; height: 800
    property bool mirroredLayout: false
    property int contentClicks: 0
    StudioPalette { id: fixturePalette }
    StudioScrollView {
        id: view
        x: 16; y: 16; width: 350; height: 240
        clip: true
        contentWidth: 1200; contentHeight: 1600
        LayoutMirroring.enabled: fixture.mirroredLayout
        Rectangle {
            width: view.contentWidth; height: view.contentHeight; color: appearance.colors.panel
            MouseArea { anchors.fill: parent; onClicked: fixture.contentClicks++ }
        }
    }
    ListView {
        id: list
        x: 16; y: 500; width: 260; height: 180
        clip: true
        model: 80
        delegate: Item { required property int index; width: list.width; height: 30 }
        ScrollBar.vertical: StudioScrollBar {}
    }
    // Only the message contents are a stub: the actual production Inspector
    // and its ScrollView process geometry, thumb input and scrolling.
    QtObject {
        id: message
        property var topics: ["/layout_test"]
        property string selectedTopic: ""
        property string selectedType: ""
        property bool working: false
        property string status: ""
        property string fields: "value: 123.45\n".repeat(200)
        property url preview: ""
        property int arrayPage: 0
        property int arrayPageCount: 1
    }
    QtObject {
        id: playback
        property real position: 0
        property real duration: 1
        property bool loading: false
    }
    TopicInspector {
        id: topic
        x: 750; y: 16; width: 280; height: 690
        inspector: message
        controller: playback
    }
    TestCase {
        name: "SharedScrollView"
        when: windowShown
        function init() {
            appearance.mode = 0
            fixture.mirroredLayout = false
            view.width = 350; view.height = 240; view.padding = 0
            view.contentWidth = 1200; view.contentHeight = 1600
            fixture.contentClicks = 0
            view.ScrollBar.vertical.policy = ScrollBar.AsNeeded
            view.ScrollBar.horizontal.policy = ScrollBar.AsNeeded
            view.contentItem.contentX = view.contentItem.contentY = 0
            list.contentY = 0
            findChild(topic, "topicFieldsScroll").contentItem.contentY = 0
        }
        function verifyTracks(control) {
            const vertical = control.ScrollBar.vertical
            const horizontal = control.ScrollBar.horizontal
            compare(vertical.parent, control)
            compare(vertical.height, control.availableHeight)
            compare(vertical.y, control.topPadding)
            compare(vertical.x, control.mirrored ? 0 : control.width - vertical.width)
            compare(horizontal.parent, control)
            compare(horizontal.width, control.availableWidth)
            compare(horizontal.x, control.leftPadding)
            compare(horizontal.y, control.height - horizontal.height)
        }
        function dragThumb(bar, dx, dy) {
            const thumb = bar.contentItem
            mouseDrag(thumb, thumb.width / 2, thumb.height / 2, dx, dy)
        }
        function test_geometryAndBothThumbs_data() {
            const rows = []
            for (const mode of [0, 1])
                for (const size of [{width: 216, height: 180}, {width: 640, height: 360}])
                    for (const padding of [0, 8])
                        rows.push({tag: mode + "-" + size.width + "-" + padding,
                                   mode: mode, width: size.width, height: size.height, padding: padding})
            return rows
        }
        function test_geometryAndBothThumbs(data) {
            appearance.mode = data.mode
            view.width = data.width; view.height = data.height; view.padding = data.padding
            verify(waitForPolish(view))
            verifyTracks(view)
            verify(view.contentItem.contentWidth > view.contentItem.width)
            verify(view.contentItem.contentHeight > view.contentItem.height)
            dragThumb(view.ScrollBar.vertical, 0, 60)
            tryVerify(() => view.contentItem.contentY > 0)
            dragThumb(view.ScrollBar.horizontal, 60, 0)
            tryVerify(() => view.contentItem.contentX > 0)
            view.width += 40; view.height += 30
            verify(waitForPolish(view))
            verifyTracks(view)
        }
        function test_mirroredAndPolicies() {
            fixture.mirroredLayout = true
            verify(waitForPolish(view))
            verify(view.mirrored)
            verifyTracks(view)
            view.ScrollBar.vertical.policy = ScrollBar.AlwaysOff
            view.ScrollBar.horizontal.policy = ScrollBar.AlwaysOff
            verify(!view.ScrollBar.vertical.visible)
            verify(!view.ScrollBar.horizontal.visible)
        }
        function test_fittingContentDoesNotInterceptInput() {
            view.contentWidth = view.availableWidth
            view.contentHeight = view.availableHeight
            verify(waitForPolish(view))
            tryCompare(view.ScrollBar.vertical, "visible", false)
            tryCompare(view.ScrollBar.horizontal, "visible", false)
            mouseClick(view, view.width / 2, view.height - 6)
            mouseClick(view, view.width - 6, view.height / 2)
            compare(fixture.contentClicks, 2,
                    "Bars without a scroll range must not cover the content's edge controls")
            view.ScrollBar.vertical.policy = ScrollBar.AlwaysOn
            view.ScrollBar.horizontal.policy = ScrollBar.AlwaysOn
            verify(view.ScrollBar.vertical.visible)
            verify(view.ScrollBar.horizontal.visible)
        }
        function test_actualTopicInspectorScrolls() {
            const fields = findChild(topic, "topicFieldsScroll")
            verify(waitForPolish(topic))
            verifyTracks(fields)
            verify(fields.contentItem.contentHeight > fields.height)
            dragThumb(fields.ScrollBar.vertical, 0, 90)
            tryVerify(() => fields.contentItem.contentY > 0)
        }
        function test_listViewKeepsAttachedGeometryAndInput() {
            const bar = list.ScrollBar.vertical
            compare(bar.height, list.height)
            compare(bar.x, list.width - bar.width)
            dragThumb(bar, 0, 70)
            tryVerify(() => list.contentY > 0)
        }
    }
}
