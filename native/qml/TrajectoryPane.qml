import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

RowLayout {
    id: pane
    required property var editor
    property rect selection: Qt.rect(0, 0, 0, 0)
    spacing: 16

    FileDialog {
        id: openDialog
        title: "打开固定机位视频"
        nameFilters: ["视频 (*.mp4 *.mov *.mkv *.avi *.webm)", "所有文件 (*)"]
        onAccepted: {
            pane.selection = Qt.rect(0, 0, 0, 0)
            start.text = "0"
            end.clear()
            pane.editor.open(selectedFile)
        }
    }
    FileDialog {
        id: saveDialog
        title: "保存原分辨率拖影图"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["PNG 图像 (*.png)"]
        onAccepted: pane.editor.save(selectedFile)
    }

    ColumnLayout {
        Layout.preferredWidth: 310
        Layout.fillHeight: true
        spacing: 10

        Button {
            text: "打开固定机位视频"
            enabled: !pane.editor.working
            onClicked: openDialog.open()
        }
        Label {
            visible: text.length > 0
            text: pane.editor.fileName
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }
        GridLayout {
            columns: 2
            enabled: !pane.editor.working && pane.editor.fileName.length > 0

            Label { text: "开始时间（秒）" }
            TextField {
                id: start
                text: "0"
                Layout.fillWidth: true
                validator: DoubleValidator { bottom: 0; locale: "C" }
                onAccepted: previewButton.clicked()
            }
            Label { text: "结束时间（秒）" }
            TextField {
                id: end
                placeholderText: "留空至视频结束"
                Layout.fillWidth: true
                validator: DoubleValidator { bottom: 0; locale: "C" }
            }
        }
        RowLayout {
            Button {
                id: previewButton
                text: "查看起始帧"
                enabled: !pane.editor.working && pane.editor.fileName.length > 0 && start.acceptableInput
                onClicked: {
                    pane.selection = Qt.rect(0, 0, 0, 0)
                    pane.editor.preview(Number(start.text))
                }
            }
            Button {
                text: "生成拖影"
                enabled: !pane.editor.working && !pane.editor.hasResult &&
                         pane.selection.width > 0 && pane.selection.height > 0 &&
                         start.acceptableInput && Math.abs(Number(start.text) - pane.editor.startTime) < 0.000001
                onClicked: pane.editor.track(pane.selection, end.text)
            }
        }
        Label {
            text: pane.editor.status
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
        ProgressBar {
            visible: pane.editor.working
            Layout.fillWidth: true
            value: pane.editor.progress
        }
        Button {
            text: "取消跟踪"
            visible: pane.editor.working
            onClicked: pane.editor.cancel()
        }

        RowLayout {
            visible: pane.editor.hasResult
            Label { text: "采样间隔（秒）" }
            TextField {
                id: interval
                text: "0"
                Layout.fillWidth: true
                validator: DoubleValidator { bottom: 0; locale: "C" }
                onTextEdited: updateTimer.restart()
                ToolTip.text: "0 表示自动按画面间距采样"
                ToolTip.visible: hovered
            }
        }
        RowLayout {
            visible: pane.editor.hasResult
            Label { text: "不透明度" }
            Slider {
                id: opacity
                value: .9
                Layout.fillWidth: true
                onMoved: updateTimer.restart()
            }
            Label { text: Math.round(opacity.value * 100) + "%" }
        }
        Timer {
            id: updateTimer
            interval: 70
            onTriggered: if (interval.acceptableInput) pane.editor.configure(Number(interval.text), opacity.value)
        }
        ListView {
            id: samples
            visible: pane.editor.hasResult
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: pane.editor.frames
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property int index
                required property var modelData
                width: samples.width
                text: modelData.seconds.toFixed(3) + " s   " + (modelData.excluded ? "已删除" : modelData.valid ? "已保留" : "无有效轮廓")
                highlighted: index === samples.currentIndex
                onClicked: samples.currentIndex = index
                onDoubleClicked: pane.editor.toggleFrame(index)
            }
            Shortcut {
                sequence: "Delete"
                enabled: samples.activeFocus && samples.currentIndex >= 0 && !pane.editor.working
                onActivated: pane.editor.toggleFrame(samples.currentIndex)
            }
        }
        RowLayout {
            visible: pane.editor.hasResult
            enabled: !pane.editor.working
            Button {
                text: "删除 / 恢复选中帧"
                enabled: samples.currentIndex >= 0
                onClicked: pane.editor.toggleFrame(samples.currentIndex)
            }
            Button { text: "恢复全部"; onClicked: pane.editor.restoreFrames() }
        }
        RowLayout {
            visible: pane.editor.hasResult
            enabled: !pane.editor.working
            Button { text: "重新框选"; onClicked: pane.editor.resetPreview() }
            Button { text: "保存 PNG"; onClicked: saveDialog.open() }
        }
        Item { Layout.fillHeight: !pane.editor.hasResult }
    }

    Rectangle {
        color: "#08101a"
        radius: 8
        clip: true
        Layout.fillWidth: true
        Layout.fillHeight: true

        Image {
            id: picture
            anchors.fill: parent
            anchors.margins: 8
            source: pane.editor.image
            fillMode: Image.PreserveAspectFit
            cache: false
            readonly property real contentLeft: (width - paintedWidth) / 2
            readonly property real contentTop: (height - paintedHeight) / 2

            Rectangle {
                visible: !pane.editor.hasResult && pane.selection.width > 0 && pane.selection.height > 0
                x: picture.contentLeft + pane.selection.x * picture.paintedWidth
                y: picture.contentTop + pane.selection.y * picture.paintedHeight
                width: picture.paintedWidth * pane.selection.width
                height: picture.paintedHeight * pane.selection.height
                color: "#3382d9fd"
                border.color: "#82d9fd"
                border.width: 2
            }
            MouseArea {
                anchors.fill: parent
                enabled: !pane.editor.working && !pane.editor.hasResult && picture.status === Image.Ready
                cursorShape: enabled ? Qt.CrossCursor : Qt.ArrowCursor
                property point origin
                function pointAt(x, y) {
                    const u = (x - picture.contentLeft) / picture.paintedWidth
                    const v = (y - picture.contentTop) / picture.paintedHeight
                    return Qt.point(Math.max(0, Math.min(1, u)), Math.max(0, Math.min(1, v)))
                }
                onPressed: mouse => {
                    origin = pointAt(mouse.x, mouse.y)
                    pane.selection = Qt.rect(origin.x, origin.y, 0, 0)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const current = pointAt(mouse.x, mouse.y)
                    pane.selection = Qt.rect(Math.min(origin.x, current.x), Math.min(origin.y, current.y),
                                         Math.abs(origin.x - current.x), Math.abs(origin.y - current.y))
                }
            }
        }
    }
}
