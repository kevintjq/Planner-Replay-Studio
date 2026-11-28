import QtQuick

// One actual librviz camera. This component never creates a second scene and
// never substitutes a compatible point-cloud render for a native frame.
Item {
    id: view
    required property var client
    property string viewId: "main"
    property var coordinator: null
    property var exporter: null
    property string targetNs: ""
    property bool requireSynchronized: false
    property bool interactive: true
    property bool showDiagnostics: true
    readonly property var previewFrame: client?.viewPreviews?.[viewId] ?? ({})
    readonly property bool matchesPlayhead: previewFrame.view_id === viewId
        && previewFrame.synchronized === true && targetNs.length > 0
        && previewFrame.clock_ns === targetNs && previewFrame.synchronized_target_ns === targetNs
    readonly property bool outputActive: exporter?.nativeFrameActive === true
    readonly property string previewStatus: !client?.connected
        ? "Native RViz disconnected · 原生场景已断开"
        : !image.source.toString().length
            ? "Waiting for this native camera · 等待此原生视角"
            : requireSynchronized && !matchesPlayhead
                ? "This view is not synchronized to the playhead.\n此视角尚未与当前实验播放头同步。" : ""
    signal selected(string viewId)

    property var registeredCoordinator: null
    function updateRegistration() {
        if (registeredCoordinator) registeredCoordinator.unregisterView(view)
        registeredCoordinator = coordinator
        if (registeredCoordinator) registeredCoordinator.registerView(view)
    }
    function requestPreview() {
        if (coordinator) coordinator.requestRefresh(viewId)
        else if (client?.connected) client.requestViewPreview(viewId)
    }
    Component.onCompleted: updateRegistration()
    Component.onDestruction: if (registeredCoordinator) registeredCoordinator.unregisterView(view)
    onCoordinatorChanged: updateRegistration()
    onViewIdChanged: pointer.cancelPointer()
    onVisibleChanged: if (visible && coordinator) coordinator.requestRefresh(viewId)

    Image {
        id: image
        objectName: view.viewId === "main" ? "nativeMainView" : "nativeView-" + view.viewId
        property bool nativeSceneImage: true
        property string exportViewId: view.viewId
        anchors.fill: parent
        // An absent output view is an error for the exporter, never a stale
        // 960px preview masquerading as a high-resolution output image.
        source: view.outputActive ? view.exporter.nativeFrameSources?.[view.viewId] ?? ""
                                  : view.previewFrame.source ?? ""
        asynchronous: false
        cache: false
        fillMode: Image.PreserveAspectFit
        MouseArea {
            id: pointer
            objectName: "nativePointer-" + view.viewId
            anchors.fill: parent
            enabled: view.interactive && view.client?.connected && !view.client.busy && !view.outputActive
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            property point lastPointer
            property point lastSentPointer
            property point lastSentNormalized
            property int heldButtons: 0
            property int heldModifiers: 0
            property int activeButton: Qt.NoButton
            property string gestureViewId: ""
            function inside(x, y) {
                const left = (width - image.paintedWidth) / 2, top = (height - image.paintedHeight) / 2
                return image.paintedWidth > 0 && image.paintedHeight > 0
                    && x >= left && y >= top && x <= left + image.paintedWidth && y <= top + image.paintedHeight
            }
            function send(type, button, buttons, modifiers, delta = 0, normalized = null) {
                const left = (width - image.paintedWidth) / 2, top = (height - image.paintedHeight) / 2
                const position = normalized ?? Qt.point(
                    Math.max(0, Math.min(1, (lastPointer.x - left) / Math.max(1, image.paintedWidth))),
                    Math.max(0, Math.min(1, (lastPointer.y - top) / Math.max(1, image.paintedHeight))))
                view.client.cameraMouseForView(activeButton !== Qt.NoButton ? gestureViewId : view.viewId, type,
                    position.x, position.y, button, buttons, modifiers, delta)
                lastSentPointer = lastPointer; lastSentNormalized = position
            }
            function flushMove() {
                if (activeButton === Qt.NoButton
                    || (lastPointer.x === lastSentPointer.x && lastPointer.y === lastSentPointer.y)) return
                send("move", Qt.NoButton, heldButtons, heldModifiers)
            }
            function cancelPointer() {
                pointerThrottle.stop()
                // 取消不执行待发移动；输出切换可能已清空Image，沿用上次实际发送的归一化位置。
                if (activeButton !== Qt.NoButton && view.client?.connected)
                    send("release", activeButton, Qt.NoButton, heldModifiers, 0, lastSentNormalized)
                activeButton = Qt.NoButton; gestureViewId = ""
                heldButtons = Qt.NoButton; heldModifiers = Qt.NoModifier
            }
            onEnabledChanged: if (!enabled) cancelPointer()
            onVisibleChanged: if (!visible) cancelPointer()
            Component.onDestruction: cancelPointer()
            onPressed: mouse => {
                if (!inside(mouse.x, mouse.y)) { mouse.accepted = false; return }
                lastPointer = Qt.point(mouse.x, mouse.y)
                heldButtons = mouse.buttons; heldModifiers = mouse.modifiers
                gestureViewId = view.viewId; activeButton = mouse.button
                view.selected(view.viewId)
                send("press", mouse.button, mouse.buttons, mouse.modifiers)
            }
            onPositionChanged: mouse => {
                if (!pressed || activeButton === Qt.NoButton) return
                lastPointer = Qt.point(mouse.x, mouse.y); heldButtons = mouse.buttons; heldModifiers = mouse.modifiers
                if (!pointerThrottle.running) pointerThrottle.start()
            }
            onReleased: mouse => {
                if (activeButton === Qt.NoButton) return
                pointerThrottle.stop(); lastPointer = Qt.point(mouse.x, mouse.y)
                // 先发最后一段移动再释放，快速拖动也必须到达原手势机位。释放事件已不含该按键。
                heldButtons = mouse.buttons | activeButton; heldModifiers = mouse.modifiers
                flushMove()
                send("release", mouse.button, mouse.buttons, mouse.modifiers)
                activeButton = Qt.NoButton
                gestureViewId = ""
                heldButtons = Qt.NoButton; heldModifiers = Qt.NoModifier
                view.requestPreview()
            }
            onCanceled: {
                cancelPointer()
            }
            onWheel: wheel => {
                if (!inside(wheel.x, wheel.y)) { wheel.accepted = false; return }
                lastPointer = Qt.point(wheel.x, wheel.y)
                send("wheel", 0, wheel.buttons, wheel.modifiers, wheel.angleDelta.y)
                wheel.accepted = true
            }
            Timer { id: pointerThrottle; interval: 33; onTriggered: pointer.flushMove() }
        }
    }
    StudioLabel {
        objectName: "nativeViewStatus-" + view.viewId
        anchors.centerIn: parent
        width: Math.max(0, Math.min(implicitWidth, parent.width - 16))
        visible: view.showDiagnostics && !view.outputActive && view.previewStatus.length > 0
        text: view.previewStatus
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: appearance.metrics.caption
        padding: 6
        background: Rectangle { color: appearance.colors.panel; radius: appearance.metrics.radius }
    }
}
