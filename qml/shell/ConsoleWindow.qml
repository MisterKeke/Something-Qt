import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../pages" as Pages
import Something.App as ThemeModule

ApplicationWindow {
    id: consoleWindow
    objectName: "consoleWindow"

    required property var controller
    property string returnRoute: "dashboard"
    property string returnTitle: qsTr("Main Page")
    property bool interactionBlocked: false
    property bool fullApplicationOpened: false
    signal navigateRequested(string routeId)

    width: 1000
    height: 800
    minimumWidth: 600
    minimumHeight: 420
    visible: false
    // An independent native window with its own taskbar/Alt+Tab entry.
    transientParent: null
    flags: Qt.Window
    title: qsTr("Something — Console")
    color: ThemeModule.Theme.windowBackground
    contentItem.enabled: !interactionBlocked

    onVisibleChanged: if (visible) focusInput()

    function focusInput() {
        Qt.callLater(function() {
            if (consoleWindow.visible && !consoleWindow.interactionBlocked)
                terminalPage.focusInput()
        })
    }

    function present() {
        if (interactionBlocked)
            return
        if (visibility === Window.Minimized)
            showNormal()
        else
            show()
        raise()
        requestActivate()
        focusInput()
    }

    // Closing hides this persistent instance; reopening retains the command draft,
    // completion list and transcript scroll position, including pending output.
    // If the main window is still hidden, C++ shuts down on lastWindowClosed.
    ScrollView {
        id: consoleScroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            width: consoleScroll.availableWidth
            spacing: 18
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.topMargin: 18
                text: qsTr("Console")
                color: ThemeModule.Theme.textPrimary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 30
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                visible: !consoleWindow.fullApplicationOpened
                text: qsTr("Type run full to open the full application window.")
                textFormat: Text.PlainText
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                wrapMode: Text.Wrap
            }
            Pages.TerminalPage {
                id: terminalPage
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.bottomMargin: 28
                controller: consoleWindow.controller
                returnRoute: consoleWindow.returnRoute
                returnTitle: consoleWindow.returnTitle
                showReturnButton: consoleWindow.fullApplicationOpened
                onNavigateRequested: function(routeId) { consoleWindow.navigateRequested(routeId) }
            }
        }
    }
}
