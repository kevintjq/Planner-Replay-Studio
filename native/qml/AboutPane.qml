import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var applicationInfo
    required property var updateManager
    property url logoSource: "qrc:/brand/uav-studio.svg"
    property string contactFeedback: ""
    readonly property bool opened: details.opened
    readonly property bool notificationOpened: updateNotice.opened
    readonly property bool availableUpdate: !!updateManager?.updateAvailable
    readonly property bool updateBusy: !!updateManager?.busy

    function open() { details.open() }
    function close() { details.close() }
    function copyContact() {
        contactFeedback = applicationInfo.copyEmail()
            ? "Copied email · 已复制邮箱" : "Could not copy email · 邮箱复制失败"
        feedbackTimer.restart()
    }
    Timer { id: feedbackTimer; interval: 3500; onTriggered: root.contactFeedback = "" }

    Popup {
        id: details
        objectName: "aboutDialog"
        parent: root.parent
        anchors.centerIn: parent
        width: Math.min(680, Math.max(280, parent ? parent.width - 40 : 680))
        height: Math.min(690, Math.max(300, parent ? parent.height - 40 : 690))
        padding: 20
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        palette: StudioPalette {}
        onClosed: {
            if (root.updateManager?.notificationVisible) root.updateManager.remindLater()
        }
        background: Rectangle {
            color: appearance.colors.window
            radius: appearance.metrics.panelRadius
            border.color: appearance.colors.separator
        }
        contentItem: ColumnLayout {
            spacing: 16
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Image {
                    objectName: "aboutLogo"
                    source: root.logoSource
                    sourceSize.width: 160; sourceSize.height: 160
                    Layout.preferredWidth: 80; Layout.preferredHeight: 80
                    fillMode: Image.PreserveAspectFit
                    Accessible.name: root.applicationInfo.appName + " logo"
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 4
                    StudioLabel {
                        objectName: "aboutAppName"
                        Layout.fillWidth: true
                        text: root.applicationInfo.appName
                        font.pixelSize: appearance.metrics.title + 4
                        font.weight: Font.DemiBold
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    StudioLabel {
                        objectName: "aboutBranding"
                        Layout.fillWidth: true
                        text: root.applicationInfo.fullBranding
                        color: appearance.colors.secondaryText
                        font.pixelSize: appearance.metrics.caption
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    StudioLabel {
                        objectName: "aboutVersion"
                        Layout.fillWidth: true
                        text: "Version · 版本 " + root.applicationInfo.displayVersion
                        color: appearance.colors.secondaryText
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                }
            }
            StudioScrollView {
                id: aboutScroll
                objectName: "aboutScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ScrollBar.vertical.objectName: "aboutScrollBar"
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: aboutScroll.availableWidth
                    spacing: 12
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: contactColumn.implicitHeight + 32
                        color: appearance.colors.panel
                        radius: appearance.metrics.panelRadius
                        ColumnLayout {
                            id: contactColumn
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10
                            StudioLabel {
                                objectName: "aboutAuthor"
                                Layout.fillWidth: true
                                text: "Created by · 作者 " + root.applicationInfo.author
                                font.pixelSize: appearance.metrics.section
                                font.weight: Font.DemiBold
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            StudioLabel {
                                objectName: "aboutEmail"
                                Layout.fillWidth: true
                                text: root.applicationInfo.email
                                color: appearance.colors.secondaryText
                                textFormat: Text.PlainText
                                wrapMode: Text.WrapAnywhere
                            }
                            StudioButton {
                                objectName: "aboutCopyEmail"
                                text: "Copy email · 复制邮箱"
                                onClicked: root.copyContact()
                            }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: aboutScroll.availableWidth >= 580 ? 4 : aboutScroll.availableWidth >= 370 ? 2 : 1
                                columnSpacing: 8; rowSpacing: 8
                                Repeater {
                                    objectName: "aboutLinkRepeater"
                                    model: [
                                        {key: "github", label: "GitHub · 仓库"},
                                        {key: "issues", label: "Feedback · 反馈"},
                                        {key: "releases", label: "Releases · 发行"},
                                        {key: "roadmap", label: "Roadmap · 路线图"}
                                    ]
                                    delegate: StudioButton {
                                        required property var modelData
                                        objectName: "aboutLink-" + modelData.key
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        text: modelData.label
                                        onClicked: root.applicationInfo.openLink(modelData.key)
                                        StudioToolTip { visible: parent.hovered; text: parent.text }
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: updateColumn.implicitHeight + 32
                        color: appearance.colors.panel
                        radius: appearance.metrics.panelRadius
                        ColumnLayout {
                            id: updateColumn
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10
                            StudioLabel {
                                Layout.fillWidth: true
                                text: "Software Update · 软件更新"
                                font.pixelSize: appearance.metrics.section
                                font.weight: Font.DemiBold
                            }
                            StudioLabel {
                                objectName: "aboutUpdateStatus"
                                Layout.fillWidth: true
                                text: root.updateManager.status
                                color: appearance.colors.secondaryText
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            StudioLabel {
                                objectName: "aboutUpdateError"
                                Layout.fillWidth: true
                                visible: !!root.updateManager.errorString
                                text: "Update error · 更新错误\n" + (root.updateManager.errorString || "")
                                color: appearance.colors.error
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                StudioButton {
                                    objectName: "aboutCheckUpdates"
                                    text: root.updateBusy ? "Checking… · 检查中…" : "Check now · 检查更新"
                                    enabled: !root.updateBusy
                                    onClicked: root.updateManager.checkForUpdates(true)
                                }
                                BusyIndicator {
                                    objectName: "aboutUpdateBusy"
                                    Layout.preferredWidth: appearance.metrics.controlHeight
                                    Layout.preferredHeight: appearance.metrics.controlHeight
                                    visible: running
                                    running: root.updateBusy
                                    palette: StudioPalette {}
                                }
                                Item { Layout.fillWidth: true }
                            }
                            StudioCheckBox {
                                objectName: "aboutAutomaticUpdates"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: "Auto check · 自动检查"
                                checked: root.updateManager.automaticCheck
                                onClicked: root.updateManager.automaticCheck = checked
                            }
                            StudioLabel {
                                objectName: "aboutLatestVersion"
                                Layout.fillWidth: true
                                visible: root.availableUpdate
                                text: "Available · 可用版本 " + root.updateManager.latestVersion
                                font.weight: Font.DemiBold
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            StudioScrollView {
                                id: releaseNotesScroll
                                objectName: "aboutReleaseNotesScroll"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 160
                                visible: root.availableUpdate && root.updateManager.releaseNotes.length > 0
                                contentWidth: availableWidth
                                clip: true
                                ScrollBar.vertical.objectName: "aboutReleaseNotesBar"
                                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                                TextArea {
                                    objectName: "aboutReleaseNotes"
                                    width: releaseNotesScroll.availableWidth
                                    readOnly: true
                                    selectByMouse: true
                                    text: root.updateManager.releaseNotes
                                    textFormat: TextEdit.PlainText
                                    wrapMode: TextEdit.Wrap
                                    color: appearance.colors.text
                                    selectionColor: appearance.colors.selection
                                    selectedTextColor: appearance.colors.text
                                    font.pixelSize: appearance.metrics.body
                                    padding: 10
                                    background: Rectangle { color: appearance.colors.control; radius: appearance.metrics.radius }
                                }
                            }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: aboutScroll.availableWidth >= 490 ? 3 : 1
                                columnSpacing: 8; rowSpacing: 8
                                visible: root.availableUpdate
                                StudioButton {
                                    objectName: "aboutOpenRelease"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: "Open release · 前往发行页"
                                    highlighted: true
                                    onClicked: root.updateManager.openReleasePage()
                                }
                                StudioButton {
                                    objectName: "aboutRemindLater"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: "Later · 稍后提醒"
                                    onClicked: root.updateManager.remindLater()
                                }
                                StudioButton {
                                    objectName: "aboutSkipVersion"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: "Skip version · 忽略此版本"
                                    onClicked: root.updateManager.skipVersion()
                                }
                            }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: licenseColumn.implicitHeight + 32
                        color: appearance.colors.panel
                        radius: appearance.metrics.panelRadius
                        ColumnLayout {
                            id: licenseColumn
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10
                            StudioLabel {
                                Layout.fillWidth: true
                                text: "License · 开源许可证"
                                font.pixelSize: appearance.metrics.section
                                font.weight: Font.DemiBold
                            }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: aboutScroll.availableWidth >= 490 ? 2 : 1
                                columnSpacing: 8; rowSpacing: 8
                                StudioButton {
                                    id: licenseToggle
                                    objectName: "aboutLicenseToggle"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: checked ? "Hide license · 收起许可" : "Read license · 查看许可"
                                    checkable: true
                                }
                                StudioButton {
                                    objectName: "aboutLink-license"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: "Online · 在线许可"
                                    onClicked: root.applicationInfo.openLink("license")
                                }
                            }
                            StudioScrollView {
                                id: licenseScroll
                                objectName: "aboutLicenseScroll"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 180
                                visible: licenseToggle.checked
                                contentWidth: availableWidth
                                clip: true
                                ScrollBar.vertical.objectName: "aboutLicenseBar"
                                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                                TextArea {
                                    objectName: "aboutLicenseText"
                                    width: licenseScroll.availableWidth
                                    readOnly: true
                                    selectByMouse: true
                                    text: root.applicationInfo.licenseText
                                    textFormat: TextEdit.PlainText
                                    wrapMode: TextEdit.Wrap
                                    color: appearance.colors.text
                                    selectionColor: appearance.colors.selection
                                    selectedTextColor: appearance.colors.text
                                    font.pixelSize: appearance.metrics.caption
                                    padding: 10
                                    background: Rectangle { color: appearance.colors.control; radius: appearance.metrics.radius }
                                }
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                StudioLabel {
                    objectName: "aboutContactFeedback"
                    Layout.fillWidth: true
                    text: root.contactFeedback
                    color: appearance.colors.secondaryText
                    font.pixelSize: appearance.metrics.caption
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                StudioButton {
                    objectName: "aboutDone"
                    text: "Done · 完成"
                    onClicked: details.close()
                }
            }
        }
    }

    Popup {
        id: updateNotice
        objectName: "updateNotification"
        parent: root.parent
        width: Math.min(400, Math.max(280, parent ? parent.width - 40 : 400))
        height: noticeContent.implicitHeight + 32
        x: parent ? parent.width - width - 20 : 0
        y: 20
        padding: 16
        modal: false
        dim: false
        focus: false
        closePolicy: Popup.NoAutoClose
        visible: !!root.updateManager?.notificationVisible && root.availableUpdate && !details.visible
        palette: StudioPalette {}
        background: Rectangle {
            color: appearance.colors.panel
            radius: appearance.metrics.panelRadius
            border.color: appearance.colors.separator
        }
        contentItem: ColumnLayout {
            id: noticeContent
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                Image {
                    source: root.logoSource
                    sourceSize.width: 80; sourceSize.height: 80
                    Layout.preferredWidth: 40; Layout.preferredHeight: 40
                    fillMode: Image.PreserveAspectFit
                }
                StudioLabel {
                    Layout.fillWidth: true
                    text: "Update available · 发现新版本\n" + root.updateManager.latestVersion
                    font.weight: Font.DemiBold
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                StudioButton {
                    objectName: "updateNoticeDismiss"
                    text: "×"
                    implicitWidth: appearance.metrics.controlHeight
                    Accessible.name: "Remind later · 稍后提醒"
                    onClicked: root.updateManager.remindLater()
                }
            }
            StudioLabel {
                Layout.fillWidth: true
                text: "Update when you’re ready.\n可在方便时更新，也可以忽略此版本。"
                color: appearance.colors.secondaryText
                font.pixelSize: appearance.metrics.caption
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                StudioButton {
                    objectName: "updateNoticeDetails"
                    Layout.fillWidth: true
                    text: "Details · 查看详情"
                    highlighted: true
                    onClicked: root.open()
                }
                StudioButton {
                    objectName: "updateNoticeLater"
                    Layout.fillWidth: true
                    text: "Later · 稍后提醒"
                    onClicked: root.updateManager.remindLater()
                }
            }
        }
    }
}
