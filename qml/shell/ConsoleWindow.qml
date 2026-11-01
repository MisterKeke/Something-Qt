import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../pages" as Pages
import "../components" as Shared
import Something.App as ThemeModule

ApplicationWindow {
    id: consoleWindow
    objectName: "consoleWindow"

    required property var controller
    property var lifecycleController: null
    property string returnRoute: "dashboard"
    property string returnTitle: qsTr("Main Page")
    property bool interactionBlocked: false
    property bool fullApplicationOpened: false
    property int restoreVisibility: Window.Windowed
    signal navigateRequested(string routeId)

    width: 1440
    height: 900
    minimumWidth: 900
    minimumHeight: 620
    visible: false
    // An independent native window with its own taskbar/Alt+Tab entry.
    transientParent: null
    flags: Qt.Window
    title: qsTr("Something — Console")
    color: ThemeModule.Theme.windowBackground
    contentItem.enabled: !interactionBlocked

    onVisibilityChanged: {
        if (visibility === Window.Windowed || visibility === Window.Maximized
                || visibility === Window.FullScreen)
            restoreVisibility = visibility
    }

    onVisibleChanged: if (visible) focusInput()
    onClosing: {
        if (backgroundController.handleWindowClose(consoleWindow)) {
            close.accepted = false
            return
        }
        // With no usable tray, do not leave the Console-first app running with
        // every window closed. Allow a still-visible Main window to remain open.
        Qt.callLater(function() {
            if (!backgroundController.hasVisibleWindow && !backgroundController.inTrayMode
                    && lifecycleController && !lifecycleController.shutdownRequested)
                lifecycleController.requestShutdown()
        })
    }

    function focusInput() {
        Qt.callLater(function() {
            if (consoleWindow.visible && !consoleWindow.interactionBlocked)
                terminalPage.focusInput()
        })
    }

    function present() {
        if (interactionBlocked)
            return
        if (restoreVisibility === Window.Maximized)
            showMaximized()
        else if (restoreVisibility === Window.FullScreen)
            showFullScreen()
        else
            show()
        raise()
        requestActivate()
        focusInput()
    }

    // Closing hides this persistent instance; reopening retains the command draft,
    // completion list and transcript scroll position, including pending output.
    Pages.TerminalPage {
        id: terminalPage
        anchors.fill: parent
        clip: true
        controller: consoleWindow.controller
        lifecycleController: consoleWindow.lifecycleController
        returnRoute: consoleWindow.returnRoute
        returnTitle: consoleWindow.returnTitle
        showReturnButton: consoleWindow.fullApplicationOpened
        onNavigateRequested: function(routeId) { consoleWindow.navigateRequested(routeId) }
    }

    Shared.NotificationToast {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        z: 100
        presentationEnabled: consoleWindow.visible && consoleWindow.active
        presentationKey: backgroundController.notificationSequence
        message: backgroundController.notificationMessage
        onDismissed: backgroundController.clearNotificationMessage()
    }
}
