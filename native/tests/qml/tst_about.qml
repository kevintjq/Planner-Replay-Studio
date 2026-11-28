import QtQuick
import QtQuick.Controls
import QtTest
import "../../qml"

Item {
    id: fixture
    width: 960; height: 760
    QtObject {
        id: metadata
        property string appName: "UAV Studio"
        property string projectName: "UAV-Studio"
        property string executableName: "UAVStudio"
        property string fullBranding: "UAV Studio — All-in-One Robotics Research Studio"
        property string version: "0.1.1"
        property string displayVersion: "0.1.1 · development"
        property string author: "kevin"
        property string email: "contact@example.test"
        property string githubUrl: "https://example.test/repository"
        property string issuesUrl: githubUrl + "/issues"
        property string releasesUrl: githubUrl + "/releases"
        property string roadmapUrl: githubUrl + "#roadmap"
        property string licenseUrl: githubUrl + "/blob/main/LICENSE"
        property string licenseText: "MIT License\n\nCopyright test fixture\n\nPermission is hereby granted."
        property var openedLinks: []
        property int copyCalls: 0
        property bool copySucceeds: true
        function openLink(key) { openedLinks = openedLinks.concat([key]); return true }
        function copyEmail() { copyCalls++; return copySucceeds }
    }
    QtObject {
        id: updates
        property string status: "No automatic check has run · 尚未检查更新"
        property string errorString: ""
        property bool busy: false
        property bool automaticCheck: true
        property bool updateAvailable: false
        property bool notificationVisible: false
        property string latestVersion: "0.2.0"
        property string releaseNotes: "<b>Literal release text</b>\nSecond line\n" + "Changes and fixes. ".repeat(120)
        property string releaseUrl: "https://example.test/repository/releases/tag/v0.2.0"
        property var checkCalls: []
        property int skipCalls: 0
        property int laterCalls: 0
        property int releaseCalls: 0
        property int automaticStarts: 0
        function checkForUpdates(manual) {
            checkCalls = checkCalls.concat([manual]); busy = true; status = "Checking · 检查中"
        }
        function startAutomaticCheck() { automaticStarts++ }
        function skipVersion() { skipCalls++; notificationVisible = false }
        function remindLater() { laterCalls++; notificationVisible = false }
        function openReleasePage() { releaseCalls++ }
    }
    AboutPane {
        id: pane
        applicationInfo: metadata
        updateManager: updates
        logoSource: Qt.resolvedUrl("../../../assets/icons/uav-studio.svg")
    }
    TestCase {
        name: "AboutAndUpdates"
        when: windowShown
        function init() {
            updates.notificationVisible = false
            pane.close()
            tryCompare(pane, "opened", false)
            fixture.width = 960; fixture.height = 760
            appearance.mode = 0
            pane.contactFeedback = ""
            metadata.openedLinks = []; metadata.copyCalls = 0; metadata.copySucceeds = true
            updates.busy = false; updates.automaticCheck = true; updates.updateAvailable = false
            updates.status = "No automatic check has run · 尚未检查更新"
            updates.errorString = ""
            updates.checkCalls = []; updates.skipCalls = updates.laterCalls = updates.releaseCalls = updates.automaticStarts = 0
            findChild(pane, "aboutLicenseToggle").checked = false
            findChild(pane, "aboutScroll").contentItem.contentY = 0
        }
        function cleanup() {
            updates.notificationVisible = false
            pane.close()
            tryCompare(pane, "opened", false)
        }
        function openAbout() {
            pane.open()
            tryCompare(pane, "opened", true)
            verify(waitForPolish(findChild(pane, "aboutDialog").contentItem))
        }
        function clickControl(name) {
            const item = control(name)
            verify(item !== null, name)
            const scroll = findChild(pane, "aboutScroll")
            if (pane.opened && name !== "aboutDone") {
                const flick = scroll.contentItem
                const point = item.mapToItem(flick.contentItem, 0, 0)
                flick.contentY = Math.max(0, Math.min(point.y - 8, flick.contentHeight - flick.height))
                verify(waitForPolish(findChild(pane, "aboutDialog").contentItem))
            }
            waitForRendering(item)
            mouseClick(item, item.width * .6, item.height / 2)
        }
        function control(name) {
            const found = findChild(pane, name)
            if (found) return found
            const links = findChild(pane, "aboutLinkRepeater")
            for (let index = 0; index < links.count; ++index) {
                const item = links.itemAt(index)
                if (item && item.objectName === name) return item
            }
            return null
        }
        function test_metadataAndCopyFeedback() {
            openAbout()
            compare(findChild(pane, "aboutAppName").text, metadata.appName)
            compare(findChild(pane, "aboutBranding").text, metadata.fullBranding)
            compare(findChild(pane, "aboutVersion").text, "Version · 版本 " + metadata.displayVersion)
            compare(findChild(pane, "aboutAuthor").text, "Created by · 作者 kevin")
            compare(findChild(pane, "aboutEmail").text, metadata.email)
            tryCompare(findChild(pane, "aboutLogo"), "status", Image.Ready)
            clickControl("aboutCopyEmail")
            compare(metadata.copyCalls, 1)
            compare(findChild(pane, "aboutContactFeedback").text, "Copied email · 已复制邮箱")
            metadata.copySucceeds = false
            clickControl("aboutCopyEmail")
            compare(metadata.copyCalls, 2)
            compare(findChild(pane, "aboutContactFeedback").text, "Could not copy email · 邮箱复制失败")
            compare(updates.automaticStarts, 0, "The UI must not start a second automatic updater")
        }
        function test_linksAndLicenseUseProvidedMetadata() {
            openAbout()
            for (const key of ["github", "issues", "releases", "roadmap", "license"])
                clickControl("aboutLink-" + key)
            compare(metadata.openedLinks, ["github", "issues", "releases", "roadmap", "license"])
            clickControl("aboutLicenseToggle")
            verify(findChild(pane, "aboutLicenseScroll").visible)
            const license = findChild(pane, "aboutLicenseText")
            compare(license.text, metadata.licenseText)
            compare(license.textFormat, TextEdit.PlainText)
            verify(license.readOnly)
            verifyScrollBar("aboutLicenseScroll", "aboutLicenseBar")
        }
        function test_manualUpdateAndPreference() {
            openAbout()
            clickControl("aboutAutomaticUpdates")
            compare(updates.automaticCheck, false)
            clickControl("aboutAutomaticUpdates")
            compare(updates.automaticCheck, true)
            clickControl("aboutCheckUpdates")
            compare(updates.checkCalls, [true])
            verify(!findChild(pane, "aboutCheckUpdates").enabled)
            verify(findChild(pane, "aboutUpdateBusy").running)
            clickControl("aboutCheckUpdates")
            compare(updates.checkCalls, [true], "Busy update requests are not repeated")
            updates.busy = false
            updates.status = "Offline · 网络不可用"
            compare(findChild(pane, "aboutUpdateStatus").text, updates.status)
            verify(findChild(pane, "aboutCheckUpdates").enabled)
            verify(!findChild(pane, "aboutUpdateBusy").running)
        }
        function test_availableReleaseActionsAndPlainNotes() {
            updates.updateAvailable = true
            openAbout()
            compare(findChild(pane, "aboutLatestVersion").text, "Available · 可用版本 " + updates.latestVersion)
            const notes = findChild(pane, "aboutReleaseNotes")
            compare(notes.text, updates.releaseNotes)
            compare(notes.textFormat, TextEdit.PlainText)
            verify(notes.readOnly)
            verifyScrollBar("aboutReleaseNotesScroll", "aboutReleaseNotesBar")
            clickControl("aboutOpenRelease")
            compare(updates.releaseCalls, 1)
            clickControl("aboutRemindLater")
            compare(updates.laterCalls, 1)
            verify(updates.updateAvailable, "Dismissing notification retains the manual release information")
            clickControl("aboutSkipVersion")
            compare(updates.skipCalls, 1)
            verify(updates.updateAvailable)
        }
        function test_updateFailureReason_data() {
            const rows = []
            for (const mode of [0, 1])
                for (const reason of [
                    {tag: "timeout", text: "Network request timed out · 网络请求超时。Please check the connection and retry when convenient."},
                    {tag: "403", text: "GitHub HTTP 403 · 请求受限 <literal server message>. The application remains available for editing."}
                ]) rows.push({tag: mode + "-" + reason.tag, mode: mode, reason: reason.text})
            return rows
        }
        function test_updateFailureReason(data) {
            fixture.width = 360
            appearance.mode = data.mode
            updates.status = "Update check failed · 更新检查失败"
            updates.errorString = data.reason
            openAbout()
            const error = findChild(pane, "aboutUpdateError")
            verify(error.visible)
            compare(error.text, "Update error · 更新错误\n" + data.reason)
            compare(error.textFormat, Text.PlainText)
            compare(error.wrapMode, Text.Wrap)
            compare(error.color.toString().toLowerCase(), appearance.colors.error.toLowerCase())
            verify(error.lineCount > 1)
            const scroll = findChild(pane, "aboutScroll")
            const position = error.mapToItem(scroll, 0, 0)
            verify(position.x >= 0 && position.x + error.width <= scroll.width)
            updates.errorString = ""
            verify(!error.visible)
        }
        function test_scrollBarDragsActualContent() {
            updates.updateAvailable = true
            openAbout()
            const scroll = findChild(pane, "aboutScroll")
            const flick = scroll.contentItem
            const bar = findChild(pane, "aboutScrollBar")
            verifyScrollBar("aboutScroll", "aboutScrollBar")
            verify(flick.contentHeight > flick.height)
            compare(flick.contentY, 0)
            mouseDrag(bar.contentItem, bar.contentItem.width / 2, bar.contentItem.height / 2, 0, 100)
            tryVerify(() => flick.contentY > 0, 1000,
                      "Dragging the actual vertical thumb must move the About content")
        }
        function test_notificationDoesNotTakeFocusAndCanDismiss() {
            const editor = Qt.createQmlObject('import QtQuick.Controls; TextField { width: 160; text: "Editing experiment" }', fixture)
            editor.forceActiveFocus()
            verify(editor.activeFocus)
            updates.updateAvailable = true
            updates.notificationVisible = true
            tryCompare(pane, "notificationOpened", true)
            const notice = findChild(pane, "updateNotification")
            verify(!notice.modal)
            verify(!notice.focus)
            verify(editor.activeFocus, "Automatic update notification preserves the current editor focus")
            clickControl("updateNoticeLater")
            tryCompare(pane, "notificationOpened", false)
            compare(updates.laterCalls, 1)
            verify(updates.updateAvailable)
            updates.notificationVisible = true
            tryCompare(pane, "notificationOpened", true)
            clickControl("updateNoticeDetails")
            tryCompare(pane, "opened", true)
            tryCompare(pane, "notificationOpened", false)
            clickControl("aboutDone")
            tryCompare(pane, "opened", false)
            compare(updates.laterCalls, 2, "Closing update details postpones the nonmandatory notification")
            editor.destroy()
        }
        function test_themeAndNarrowLayout_data() {
            const rows = []
            for (const width of [360, 640, 960])
                for (const mode of [0, 1]) rows.push({tag: width + "-" + mode, width: width, mode: mode})
            return rows
        }
        function verifyScrollBar(scrollName, barName) {
            const scroll = findChild(pane, scrollName)
            const bar = findChild(pane, barName)
            compare(bar.parent, scroll)
            compare(bar.height, scroll.availableHeight)
            compare(bar.y, scroll.topPadding)
            compare(bar.x, scroll.mirrored ? 0 : scroll.width - bar.width)
            verify(bar.height > appearance.metrics.controlHeight,
                   "The customized ScrollView bar must retain its track geometry")
        }
        function test_themeAndNarrowLayout(data) {
            fixture.width = data.width; fixture.height = 640
            appearance.mode = data.mode
            updates.updateAvailable = true
            openAbout()
            const dialog = findChild(pane, "aboutDialog")
            const scroll = findChild(pane, "aboutScroll")
            verifyScrollBar("aboutScroll", "aboutScrollBar")
            compare(dialog.background.color.toString().toLowerCase(), appearance.colors.window.toLowerCase())
            compare(findChild(pane, "aboutAppName").color.toString().toLowerCase(), appearance.colors.text.toLowerCase())
            verify(dialog.x >= 0 && dialog.x + dialog.width <= fixture.width)
            verify(dialog.y >= 0 && dialog.y + dialog.height <= fixture.height)
            for (const name of ["aboutCopyEmail", "aboutCheckUpdates", "aboutAutomaticUpdates", "aboutOpenRelease",
                                "aboutRemindLater", "aboutSkipVersion", "aboutLicenseToggle", "aboutLink-license",
                                "aboutLink-github", "aboutLink-issues", "aboutLink-releases", "aboutLink-roadmap"]) {
                const item = control(name)
                verify(item !== null, name)
                const position = item.mapToItem(scroll, 0, 0)
                verify(position.x >= -1 && position.x + item.width <= scroll.width + 1,
                       name + " exceeds the About scroll viewport at width " + data.width)
                verify(item.width > 0 && item.height >= appearance.metrics.controlHeight - 1)
            }
            clickControl("aboutSkipVersion")
            compare(updates.skipCalls, 1, "A control below the scroll fold remains reachable")
        }
    }
}
