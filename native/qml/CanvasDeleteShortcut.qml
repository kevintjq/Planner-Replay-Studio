import QtQuick

Shortcut {
    id: route
    property bool active: true
    property Item focusItem: null
    property Item timelineItem: null
    property int selectionCount: 0
    readonly property bool textEditing: hasTextFocus(focusItem)
    readonly property bool timelineFocused: containsFocus(timelineItem, focusItem)
    readonly property bool localDeleteFocused: hasLocalDeleteOwner(focusItem)
    signal deleteCanvasSelection()
    signal deleteTimelineSelection()

    function hasTextFocus(item) {
        for (let current = item; current; current = current.parent) {
            if (current instanceof TextInput || current instanceof TextEdit)
                return true
        }
        return false
    }
    function containsFocus(owner, item) {
        if (!owner) return false
        for (let current = item; current; current = current.parent) {
            if (current === owner) return true
        }
        return false
    }
    function hasLocalDeleteOwner(item) {
        for (let current = item; current; current = current.parent) {
            if (current.ownsDeleteKey === true) return true
        }
        return false
    }

    sequences: [StandardKey.Delete]
    context: Qt.WindowShortcut
    autoRepeat: false
    enabled: active && !textEditing && !localDeleteFocused && (timelineFocused || selectionCount > 0)
    onActivated: {
        // Timeline focus owns Delete, including the no-selected-clip case.
        // Never fall back to an older canvas selection in another editor.
        if (timelineFocused) deleteTimelineSelection()
        else deleteCanvasSelection()
    }
}
