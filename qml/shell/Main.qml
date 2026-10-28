import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../pages" as Pages
import Something.App as ThemeModule

ApplicationWindow {
    id: window

    width: 1440
    height: 960
    minimumWidth: 900
    minimumHeight: 620
    // C++ fits and centers the native frame before its first show.
    visible: false
    title: qsTr("Something")
    color: ThemeModule.Theme.windowBackground
    palette.window: ThemeModule.Theme.windowBackground
    palette.windowText: ThemeModule.Theme.textPrimary
    palette.base: ThemeModule.Theme.insetSurface
    palette.alternateBase: ThemeModule.Theme.panelSurface
    palette.text: ThemeModule.Theme.textPrimary
    palette.button: ThemeModule.Theme.raisedSurface
    palette.buttonText: ThemeModule.Theme.textPrimary
    palette.highlight: ThemeModule.Theme.selectionBlue
    palette.highlightedText: ThemeModule.Theme.textPrimary
    palette.placeholderText: ThemeModule.Theme.textSecondary
    palette.light: ThemeModule.Theme.strongBorder
    palette.mid: ThemeModule.Theme.borderColor
    palette.dark: ThemeModule.Theme.shadowBorder
    palette.shadow: ThemeModule.Theme.windowBackground

    Dialog {
        title: legacyDataImportController.restartRequired ? qsTr("Profile imported") : qsTr("Importing profile")
        modal: true
        focus: true
        visible: legacyDataImportController.importActive
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: Math.min(480, window.width - 40)
        padding: 22
        background: Rectangle {
            color: ThemeModule.Theme.raisedSurface
            border.color: ThemeModule.Theme.accentCyan
            radius: ThemeModule.Theme.panelRadius
        }
        contentItem: Label {
            text: legacyDataImportController.busy ? legacyDataImportController.progressMessage
                  : qsTr("The retained data is saved in this Qt profile. Close Something and start it again to load it. The import report is available in Settings.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: ThemeModule.Theme.textPrimary
            font.family: ThemeModule.Theme.fontFamily
        }
        footer: DialogButtonBox {
            Shared.AppButton {
                text: qsTr("Cancel import")
                visible: legacyDataImportController.canCancel
                onClicked: legacyDataImportController.cancelImport()
            }
            Shared.AppButton {
                text: qsTr("Close Something")
                visible: legacyDataImportController.restartRequired
                enabled: !legacyDataImportController.busy
                onClicked: appController.requestShutdown()
            }
        }
    }

    property string currentRoute: "dashboard"
    property bool pageActive: true
    property var routeState: ({})
    readonly property var routeTitles: routeCatalog.routeTitles
    readonly property var navigationItems: routeCatalog.navigationItems

    function focusCurrentTab() {
        Qt.callLater(function() {
            if (workspaceTabs.currentIndex === 1)
                terminalPage.focusInput()
            else if (window.currentRoute === "dashboard" && pageLoader.item
                     && typeof pageLoader.item.restoreShellFocus === "function")
                pageLoader.item.restoreShellFocus()
            else if (window.currentRoute !== "dashboard")
                pageNavigationSelector.forceActiveFocus()
            else
                mainTabButton.forceActiveFocus()
        })
    }

    function shortcutIsBlocked() {
        if (legacyDataImportController.importActive || setupLaunchResult.visible)
            return true
        let item = window.activeFocusItem
        let remaining = 32
        let insideWindowContent = false
        while (item && remaining-- > 0) {
            if (item === window.contentItem) insideWindowContent = true
            if (item.modal === true && item.visible !== false)
                return true
            if (item.inputMethodComposing !== undefined
                    || item.maximumLength !== undefined
                    || item.editable === true)
                return true
            item = item.parent
        }
        if (window.activeFocusItem && !insideWindowContent)
            return true
        return false
    }

    function navigateTo(routeId) {
        if (!routeTitles[routeId])
            return
        if (routeId === "terminal") {
            workspaceTabs.currentIndex = 1
            focusCurrentTab()
            return
        }

        workspaceTabs.currentIndex = 0
        if (routeId === currentRoute)
            return

        if (pageLoader.item && typeof pageLoader.item.captureViewState === "function") {
            const nextState = Object.assign({}, routeState)
            nextState[currentRoute] = pageLoader.item.captureViewState()
            routeState = nextState
        }
        routeScrollPositions[currentRoute] = shellScroll.contentItem.contentY
        pageActive = false
        currentRoute = routeId
        Qt.callLater(function() { window.pageActive = true })
    }

    property var routeScrollPositions: ({})

    Component {
        id: wallpaperPageComponent
        Pages.WallpaperSettingsPage {
            controller: wallpaperController
            importController: legacyDataImportController
            settingsController: appSettingsController
        }
    }

    Component {
        id: tasksPageComponent
        Pages.TasksPage {
            controller: taskController
        }
    }

    Component {
        id: dashboardPageComponent
        Pages.DashboardPage {
            controller: dashboardController
            navigationItems: window.navigationItems
            onNavigateRequested: function(routeId) { window.navigateTo(routeId) }
        }
    }

    Component {
        id: favoritesPageComponent
        Pages.FavoritesPage {
            providerSource: window.currentRoute
        }
    }

    Component {
        id: calendarPageComponent
        Pages.CalendarPage {
            controller: calendarController
        }
    }

    Component {
        id: weatherPageComponent
        Pages.WeatherPage {
            controller: weatherController
        }
    }

    Component {
        id: currenciesPageComponent
        Pages.CurrenciesPage {}
    }

    Component {
        id: desktopApplicationsPageComponent
        Pages.DesktopApplicationsPage { controller: desktopApplicationsController }
    }

    Component {
        id: runningApplicationsPageComponent
        Pages.RunningApplicationsPage { controller: runningApplicationsController }
    }

    Component {
        id: setupsPageComponent
        Pages.SetupsPage {
            controller: setupsController
            onNavigateRequested: function(routeId) { window.navigateTo(routeId) }
        }
    }

    Component {
        id: fileExplorerPageComponent
        Pages.FileExplorerPage { controller: fileExplorerController; viewportHeight: window.height - 190 }
    }

    Component {
        id: steamPageComponent
        Pages.SteamGamesPage { controller: steamGamesController }
    }

    background: Item {
        clip: true

        Image {
            id: wallpaperBackground
            anchors.fill: parent
            source: wallpaperController.wallpaperImageSource
            sourceSize: Qt.size(Math.min(2560, window.width * Screen.devicePixelRatio),
                                Math.min(1600, window.height * Screen.devicePixelRatio))
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: false
            smooth: true
            onStatusChanged: {
                if (status === Image.Error && wallpaperController.selectedWallpaper.startsWith("custom:"))
                    wallpaperController.fallbackIfMissing(wallpaperController.selectedWallpaper)
            }
        }

        Rectangle {
            anchors.fill: parent
            color: ThemeModule.Theme.scrim
        }

    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: workspaceTabs
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.topMargin: 12
            Layout.bottomMargin: 6
            spacing: 8
            onCurrentIndexChanged: window.focusCurrentTab()
            background: Rectangle { color: "transparent" }

            TabButton {
                id: mainTabButton
                text: qsTr("Main Page")
                width: Math.max(140, implicitWidth)
                font.family: ThemeModule.Theme.fontFamily
                font.weight: Font.DemiBold
                contentItem: Label {
                    text: mainTabButton.text
                    font: mainTabButton.font
                    color: mainTabButton.checked ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 42
                    radius: ThemeModule.Theme.panelRadius
                    color: mainTabButton.checked ? ThemeModule.Theme.selectionBlue
                           : mainTabButton.down ? ThemeModule.Theme.shadowBorder
                           : mainTabButton.hovered ? ThemeModule.Theme.hoverSurface : ThemeModule.Theme.panelSurface
                    border.color: mainTabButton.activeFocus ? ThemeModule.Theme.focusHighlight
                                  : mainTabButton.checked ? ThemeModule.Theme.strongBorder : ThemeModule.Theme.borderColor
                    border.width: mainTabButton.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
                }
            }
            TabButton {
                id: consoleTabButton
                text: qsTr("Console")
                width: Math.max(140, implicitWidth)
                font.family: ThemeModule.Theme.fontFamily
                font.weight: Font.DemiBold
                contentItem: Label {
                    text: consoleTabButton.text
                    font: consoleTabButton.font
                    color: consoleTabButton.checked ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 42
                    radius: ThemeModule.Theme.panelRadius
                    color: consoleTabButton.checked ? ThemeModule.Theme.selectionBlue
                           : consoleTabButton.down ? ThemeModule.Theme.shadowBorder
                           : consoleTabButton.hovered ? ThemeModule.Theme.hoverSurface : ThemeModule.Theme.panelSurface
                    border.color: consoleTabButton.activeFocus ? ThemeModule.Theme.focusHighlight
                                  : consoleTabButton.checked ? ThemeModule.Theme.strongBorder : ThemeModule.Theme.borderColor
                    border.width: consoleTabButton.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: workspaceTabs.currentIndex

            ScrollView {
                id: shellScroll
                clip: true
                contentWidth: availableWidth
                ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
                ColumnLayout {
                    width: shellScroll.availableWidth
                    spacing: 18
                    RowLayout {
                        visible: window.currentRoute !== "dashboard"
                        Layout.fillWidth: true
                        Layout.leftMargin: 24
                        Layout.rightMargin: 24
                        Layout.topMargin: 18
                        spacing: 12
                        Shared.AppButton {
                            text: qsTr("‹ Main Page")
                            quiet: true
                            onClicked: window.navigateTo("dashboard")
                        }
                        Shared.AppComboBox {
                            id: pageNavigationSelector
                            Layout.preferredWidth: 220
                            model: window.navigationItems
                            textRole: "title"
                            valueRole: "id"
                            currentIndex: {
                                for (let i = 0; i < window.navigationItems.length; ++i)
                                    if (window.navigationItems[i].id === window.currentRoute) return i
                                return -1
                            }
                            Accessible.name: qsTr("Navigate to a page")
                            onActivated: window.navigateTo(currentValue)
                        }
                        Item { Layout.fillWidth: true }
                        Shared.StatusBadge {
                            text: appController.lifecycleState === "running" ? qsTr("Ready") : qsTr("Starting")
                            badgeColor: appController.lifecycleState === "running" ? ThemeModule.Theme.success : ThemeModule.Theme.warning
                        }
                        Shared.AppButton {
                            visible: setupsController.launching
                            text: qsTr("Stop setup")
                            quiet: true
                            onClicked: setupsController.cancelLaunch()
                        }
                    }
                    ColumnLayout {
                        visible: window.currentRoute !== "dashboard"
                        Layout.fillWidth: true
                        Layout.leftMargin: 24
                        Layout.rightMargin: 24
                        spacing: 4
                        Label {
                            text: window.routeTitles[window.currentRoute] || qsTr("Something")
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 30
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: window.currentRoute === "fileExplorer" ? qsTr("Your drives, folders, and files in one place")
                                : window.currentRoute === "tasks" ? qsTr("Search, plan, and track task completion")
                                : window.currentRoute === "calendar" ? qsTr("Your schedule at a glance")
                                : window.currentRoute === "weather" ? qsTr("Your local forecast and city weather")
                                : window.currentRoute === "currencies" ? qsTr("Rates, conversions, and favorite pairs")
                                : window.currentRoute === "setups" ? qsTr("Launch your saved application groups")
                                : window.currentRoute === "applications" ? qsTr("Your favorite desktop applications")
                                : ""
                            visible: text.length > 0
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }
                    }
                    Shared.AppCard {
                        Layout.fillWidth: true
                        Layout.leftMargin: 24
                        Layout.rightMargin: 24
                        visible: appController.startupErrorMessage.length > 0
                        ColumnLayout {
                            width: parent.width
                            Label { text: qsTr("Startup could not complete"); color: ThemeModule.Theme.error; font.bold: true }
                            Label { Layout.fillWidth: true; text: appController.startupErrorMessage; color: ThemeModule.Theme.textSecondary; wrapMode: Text.Wrap }
                        }
                    }
                    Loader {
                        id: pageLoader
                        Layout.fillWidth: true
                        Layout.leftMargin: 24
                        Layout.rightMargin: 24
                        Layout.topMargin: window.currentRoute === "dashboard" ? 22 : 0
                        Layout.bottomMargin: 28
                        Layout.preferredHeight: item ? item.implicitHeight : 640
                        active: window.pageActive
                        asynchronous: true
                        opacity: status === Loader.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: ThemeModule.Theme.pageMotion; easing.type: Easing.OutCubic } }
                        sourceComponent: window.currentRoute === "settings" ? wallpaperPageComponent
                            : window.currentRoute === "tasks" ? tasksPageComponent
                            : window.currentRoute === "calendar" ? calendarPageComponent
                            : window.currentRoute === "dashboard" ? dashboardPageComponent
                            : window.currentRoute === "weather" ? weatherPageComponent
                            : window.currentRoute === "currencies" ? currenciesPageComponent
                            : window.currentRoute === "applications" ? desktopApplicationsPageComponent
                            : window.currentRoute === "runningApplications" ? runningApplicationsPageComponent
                            : window.currentRoute === "setups" ? setupsPageComponent
                            : window.currentRoute === "fileExplorer" ? fileExplorerPageComponent
                            : window.currentRoute === "steam" ? steamPageComponent
                            : (window.currentRoute === "telegram" || window.currentRoute === "youtube") ? favoritesPageComponent
                            : dashboardPageComponent
                        onLoaded: {
                            if (item && typeof item.restoreViewState === "function")
                                item.restoreViewState(window.routeState[window.currentRoute] || {})
                            shellScroll.contentItem.contentY = Number(window.routeScrollPositions[window.currentRoute] || 0)
                        }
                    }
                }
            }

            ScrollView {
                id: consoleScroll
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
                    Pages.TerminalPage {
                        id: terminalPage
                        Layout.fillWidth: true
                        Layout.preferredHeight: implicitHeight
                        Layout.leftMargin: 24
                        Layout.rightMargin: 24
                        Layout.bottomMargin: 28
                        controller: terminalController
                        returnRoute: window.currentRoute
                        returnTitle: window.routeTitles[window.currentRoute] || qsTr("Main Page")
                        onNavigateRequested: function(routeId) { window.navigateTo(routeId) }
                    }
                }
            }
        }
    }
    Connections {
        target: setupsController
        function onLaunchFinished(summary) {
            setupLaunchResult.summary = summary
            setupLaunchResult.open()
        }
    }

    Dialog {
        id: setupLaunchResult
        property var summary: ({ attempted: 0, launched: 0, cancelled: false, failures: [] })
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, parent ? parent.width - 32 : 520)
        padding: 20
        title: qsTr("Setup launch results")
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: ThemeModule.Theme.raisedSurface; border.color: ThemeModule.Theme.strongBorder; radius: ThemeModule.Theme.panelRadius }
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("Launched %1 of %2 attempted applications.")
                    .arg(setupLaunchResult.summary.launched || 0)
                    .arg(setupLaunchResult.summary.attempted || 0)
                color: ThemeModule.Theme.textPrimary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: Boolean(setupLaunchResult.summary.cancelled)
                text: qsTr("The remaining launch attempts were cancelled. Applications already opened keep running.")
                color: ThemeModule.Theme.warning
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: (setupLaunchResult.summary.failures || []).length > 0
                text: qsTr("Applications that could not be opened")
                color: ThemeModule.Theme.error
                font.weight: Font.DemiBold
            }
            Repeater {
                model: setupLaunchResult.summary.failures || []
                delegate: ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        text: modelData.applicationName || qsTr("Application")
                        color: ThemeModule.Theme.textPrimary
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.message
                        color: ThemeModule.Theme.textSecondary
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        footer: RowLayout {
            Item { Layout.fillWidth: true }
            Shared.AppButton { text: qsTr("Close"); onClicked: setupLaunchResult.close() }
        }
    }

    Shortcut {
        sequence: "Ctrl+`"
        context: Qt.WindowShortcut
        enabled: !window.shortcutIsBlocked()
        onActivated: window.navigateTo("terminal")
    }

    Connections {
        target: terminalController
        function onRouteRequested(routeId) { window.navigateTo(routeId) }
    }

    Shared.NotificationToast {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        z: 100
        message: taskController.notification.length > 0
                 ? taskController.notification
                 : setupsController.errorMessage.length > 0
                   ? setupsController.errorMessage : wallpaperController.notification
        onDismissed: {
            if (taskController.notification.length > 0)
                taskController.clearNotification()
            else if (setupsController.errorMessage.length > 0)
                setupsController.clearMessages()
            else
                wallpaperController.clearNotification()
        }
    }

    Shared.CursorTrail {
        parent: Overlay.overlay
        tracker: cursorTrailController
    }
}
