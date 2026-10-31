import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root

    required property var controller
    required property var importController
    required property var settingsController
    required property var backgroundController
    required property var usageController
    implicitHeight: 640

    property string pendingDeleteId: ""
    property url sourceDatabaseFile: ""
    property string usageExclusionSearch: ""
    property string usageFilenameToExclude: ""
    Component.onCompleted: root.importController.refreshStatus()

    function importReportText(json) {
        if (!json) return ""
        try {
            const report = JSON.parse(json)
            let lines = [report.committed ? qsTr("Imported records:") : qsTr("Source records; no import committed:")]
            if (report.sourceSchemaVersion) lines.unshift(qsTr("Source schema %1").arg(report.sourceSchemaVersion))
            const counts = report.importedCounts || report.sourceCounts || {}
            const keys = Object.keys(counts).sort()
            for (let i = 0; i < keys.length; ++i)
                lines.push(keys[i] + ": " + counts[keys[i]])
            const skipped = report.skippedCounts || {}
            const skippedKeys = Object.keys(skipped).sort()
            for (let j = 0; j < skippedKeys.length; ++j)
                lines.push(qsTr("Skipped %1: %2").arg(skippedKeys[j]).arg(skipped[skippedKeys[j]]))
            const missing = report.missingAssets || []
            if (missing.length) lines.push(qsTr("Missing assets: %1").arg(missing.join(", ")))
            const failures = report.assetFailures || []
            for (let k = 0; k < failures.length; ++k)
                lines.push(failures[k].kind + ":" + failures[k].ownerId + " — " + failures[k].reason)
            lines = lines.concat(report.warnings || [], report.failures || [])
            return lines.join("\n")
        } catch (error) { return qsTr("The stored import report could not be displayed.") }
    }

    function matchingUsageExclusions() {
        const query = usageExclusionSearch.trim().toLowerCase()
        const entries = usageController.exclusions || []
        if (!query) return entries
        return entries.filter(function(entry) {
            return String(entry.executableName || "").toLowerCase().indexOf(query) >= 0
                || String(entry.canonicalPath || "").toLowerCase().indexOf(query) >= 0
        })
    }

    function captureViewState() {
        return { scrollY: pageScroll.contentY }
    }

    function restoreViewState(state) {
        if (state && state.scrollY !== undefined)
            pageScroll.contentY = Number(state.scrollY)
    }

    Connections {
        target: root.settingsController
        function onStateChanged() {
            weekStartChoice.currentIndex = root.settingsController.weekStart
        }
    }

    FileDialog {
        id: wallpaperPicker
        title: qsTr("Choose a wallpaper image")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Images (*.jpg *.jpeg *.png *.webp)")]
        onAccepted: root.controller.importWallpaper(selectedFile)
    }

    FileDialog {
        id: sourceDatabasePicker
        title: qsTr("Choose Something-v2 database.db")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Something-v2 database (database.db)")]
        onAccepted: {
            root.sourceDatabaseFile = selectedFile
            sourceClosedConfirmation.open()
        }
    }

    Dialogs.ConfirmationDialog {
        id: sourceClosedConfirmation
        title: qsTr("Import the retained profile?")
        confirmText: qsTr("Import profile")
        message: qsTr("Close Something-v2 completely, including its tray process, and keep it closed before continuing. Import runs once into a Qt profile without saved records. A built-in Qt wallpaper choice is allowed and kept. It copies retained tasks, favorites, provider state, weather, applications, Setups, Steam tracking, wallpapers, and supported settings. Excluded feature data is ignored.")
        onConfirmed: root.importController.importProfile(root.sourceDatabaseFile, true)
    }

    Dialogs.ConfirmationDialog {
        id: deleteConfirmation
        title: qsTr("Delete wallpaper?")
        message: qsTr("This imported wallpaper will be removed from this device. This cannot be undone.")
        confirmText: qsTr("Delete wallpaper")
        destructive: true
        onConfirmed: {
            root.controller.deleteWallpaper(root.pendingDeleteId)
            root.pendingDeleteId = ""
        }
    }

    Dialogs.ConfirmationDialog {
        id: deleteUsageHistoryConfirmation
        title: qsTr("Delete application usage history?")
        message: qsTr("All saved application usage sessions and time segments will be permanently deleted. Exclusions and tracking settings will remain.")
        confirmText: qsTr("Delete history")
        destructive: true
        onConfirmed: root.usageController.deleteHistory("")
    }

    Flickable {
        id: pageScroll
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: pageColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: pageColumn
            width: pageScroll.width
            spacing: 16

            Shared.AppCard {
                Layout.fillWidth: true
                Layout.preferredHeight: 142

                RowLayout {
                    width: parent.width
                    spacing: 14

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            text: qsTr("Appearance")
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Choose a built-in scene or import your own JPEG, PNG, or WebP image. Imported files are copied into this profile and limited to 20 MB.")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }
                    }
                    Shared.AppButton {
                        text: root.controller.busy ? qsTr("Working…") : qsTr("Upload wallpaper")
                        enabled: root.controller.ready && !root.controller.busy
                        Accessible.name: qsTr("Import a wallpaper image")
                        onClicked: wallpaperPicker.open()
                    }
                }

                Label {
                    width: parent.width
                    visible: root.controller.errorMessage.length > 0
                    text: root.controller.errorMessage
                    color: ThemeModule.Theme.error
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                Layout.preferredHeight: backgroundLayout.implicitHeight + padding * 2

                ColumnLayout {
                    id: backgroundLayout
                    width: parent.width
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Background & System Tray")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    Shared.AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Enable system tray integration")
                        checked: root.backgroundController.systemTrayEnabled
                        onToggled: root.backgroundController.systemTrayEnabled = checked
                    }
                    Shared.AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Minimize to system tray when closing")
                        checked: root.backgroundController.minimizeToTrayOnClose
                        onToggled: root.backgroundController.minimizeToTrayOnClose = checked
                    }
                    Shared.AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Launch Something-Qt when Windows starts")
                        checked: root.backgroundController.launchWithWindows
                        enabled: root.backgroundController.startupSupported
                        onToggled: root.backgroundController.launchWithWindows = checked
                    }
                    Shared.AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Start minimized to system tray")
                        checked: root.backgroundController.startMinimizedToTray
                        onToggled: root.backgroundController.startMinimizedToTray = checked
                    }
                    Shared.AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Show a notification the first time Something is hidden")
                        checked: root.backgroundController.notifyOnFirstHide
                        onToggled: root.backgroundController.notifyOnFirstHide = checked
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.backgroundController.statusMessage.length > 0
                        text: root.backgroundController.statusMessage
                        color: ThemeModule.Theme.warning
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("These preferences are saved in your user settings and are available before the application profile loads. Windows startup is configured for your Windows account only.")
                        color: ThemeModule.Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                Layout.preferredHeight: usageTrackingLayout.implicitHeight + padding * 2

                ColumnLayout {
                    id: usageTrackingLayout
                    width: parent.width
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Application Usage Tracking")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Eligible taskbar applications are tracked automatically, including applications started outside Something-Qt. No catalog registration is required. Tracking is local: screenshots, keystrokes, window contents, and communications are not recorded.")
                        color: ThemeModule.Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Shared.AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Enable automatic application usage tracking")
                        checked: root.usageController.trackingEnabled
                        enabled: root.usageController.ready && root.usageController.supported
                        onToggled: root.usageController.setTrackingEnabled(checked)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.usageController.trackingStatus
                        color: root.usageController.trackingActive
                               ? ThemeModule.Theme.success : ThemeModule.Theme.warning
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Keep usage history")
                            color: ThemeModule.Theme.textSecondary
                        }
                        Shared.AppComboBox {
                            id: usageRetentionChoice
                            Layout.preferredWidth: 190
                            model: [qsTr("Unlimited"), qsTr("90 days"), qsTr("1 year"), qsTr("2 years")]
                            currentIndex: root.usageController.retentionDays === 0 ? 0
                                          : root.usageController.retentionDays <= 90 ? 1
                                          : root.usageController.retentionDays <= 365 ? 2 : 3
                            enabled: root.usageController.ready
                            accessibleLabel: qsTr("Usage history retention period")
                            onActivated: function(index) {
                                root.usageController.setRetentionDays([0, 90, 365, 730][index])
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Excluded applications")
                        color: ThemeModule.Theme.textPrimary
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    Shared.AppTextField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("Search exclusions")
                        accessibleLabel: qsTr("Search excluded applications")
                        text: root.usageExclusionSearch
                        onTextChanged: root.usageExclusionSearch = text
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            placeholderText: qsTr("Executable filename, for example game.exe")
                            accessibleLabel: qsTr("Exclude applications by executable filename")
                            text: root.usageFilenameToExclude
                            onTextChanged: root.usageFilenameToExclude = text
                        }
                        Shared.AppButton {
                            text: qsTr("Exclude filename")
                            enabled: root.usageController.ready && root.usageFilenameToExclude.trim().length > 0
                            onClicked: {
                                root.usageController.addFilenameExclusion(root.usageFilenameToExclude.trim())
                                root.usageFilenameToExclude = ""
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Filename exclusions can match different applications that use the same executable name.")
                        color: ThemeModule.Theme.textSecondary
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: root.matchingUsageExclusions()
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: modelData.executableName || modelData.applicationId
                                color: ThemeModule.Theme.textPrimary
                                elide: Text.ElideMiddle
                            }
                            Label {
                                text: modelData.matchByName ? qsTr("Filename match") : qsTr("Application")
                                color: ThemeModule.Theme.textSecondary
                                font.pixelSize: 11
                            }
                            Shared.AppButton {
                                text: qsTr("Remove")
                                quiet: true
                                onClicked: root.usageController.removeExclusion(modelData.applicationId)
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.matchingUsageExclusions().length === 0
                        text: root.usageController.exclusions.length === 0
                              ? qsTr("No applications are excluded.") : qsTr("No exclusions match this search.")
                        color: ThemeModule.Theme.textSecondary
                        font.pixelSize: 12
                    }
                    Shared.AppButton {
                        text: qsTr("Delete usage history")
                        quiet: true
                        destructive: true
                        enabled: root.usageController.ready
                        onClicked: deleteUsageHistoryConfirmation.open()
                    }
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                Layout.preferredHeight: settingsColumn.implicitHeight + padding * 2

                ColumnLayout {
                    id: settingsColumn
                    width: parent.width
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Retained settings")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("First day of the Dashboard week")
                            color: ThemeModule.Theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        Shared.AppComboBox {
                            id: weekStartChoice
                            Layout.preferredWidth: 180
                            model: [qsTr("Sunday"), qsTr("Monday"), qsTr("Tuesday"),
                                    qsTr("Wednesday"), qsTr("Thursday"), qsTr("Friday"),
                                    qsTr("Saturday")]
                            currentIndex: root.settingsController.weekStart
                            enabled: root.settingsController.ready && root.settingsController.loaded
                                     && !root.settingsController.loading && !root.settingsController.saving
                            accessibleLabel: qsTr("First day of the Dashboard week")
                            onActivated: function(index) {
                                root.settingsController.setWeekStart(index)
                                currentIndex = root.settingsController.weekStart
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Current local time zone")
                            color: ThemeModule.Theme.textSecondary
                        }
                        Label {
                            text: root.settingsController.localTimeZone
                            color: ThemeModule.Theme.textPrimary
                            textFormat: Text.PlainText
                            Accessible.name: qsTr("Current local time zone: %1").arg(text)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.settingsController.errorMessage.length > 0
                        text: root.settingsController.errorMessage
                        color: ThemeModule.Theme.error
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                }
            }

            Shared.AppCard {
                id: importCard
                Layout.fillWidth: true
                Layout.preferredHeight: importLayout.implicitHeight + padding * 2

                ColumnLayout {
                    id: importLayout
                    width: parent.width
                    spacing: 10
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("One-time Something-v2 import")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Supports source schema versions 24 and 25. Built-in Qt wallpaper choices are allowed and kept. The source remains untouched; a consistent copy is made after you confirm it is closed.")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Shared.AppButton {
                            text: root.importController.imported ? qsTr("Import completed")
                                  : root.importController.busy ? root.importController.progressMessage
                                  : qsTr("Choose source database")
                            enabled: root.importController.ready && root.importController.canImport
                                     && !root.importController.busy
                            Accessible.name: qsTr("Choose a closed Something-v2 database.db for one-time import")
                            onClicked: sourceDatabasePicker.open()
                        }
                        Shared.AppButton {
                            text: qsTr("Recheck profile")
                            quiet: true
                            enabled: root.importController.ready && !root.importController.busy
                                     && !root.importController.restartRequired
                            onClicked: root.importController.refreshStatus()
                        }
                        Shared.AppButton {
                            visible: root.importController.canCancel
                            text: qsTr("Cancel import")
                            quiet: true
                            enabled: root.importController.canCancel
                            onClicked: root.importController.cancelImport()
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !root.importController.busy && root.importController.eligibilityMessage.length > 0
                        text: root.importController.eligibilityMessage
                        textFormat: Text.PlainText
                        color: root.importController.canImport ? ThemeModule.Theme.success : ThemeModule.Theme.warning
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.importController.errorMessage.length > 0
                        text: root.importController.errorMessage
                        textFormat: Text.PlainText
                        color: ThemeModule.Theme.error
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(200, reportText.implicitHeight + 16)
                        visible: root.importController.reportJson.length > 0
                        ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
                        TextArea {
                            id: reportText
                            text: root.importReportText(root.importController.reportJson)
                            readOnly: true
                            textFormat: TextEdit.PlainText
                            hoverEnabled: true
                            color: ThemeModule.Theme.textSecondary
                            placeholderTextColor: ThemeModule.Theme.textSecondary
                            selectionColor: ThemeModule.Theme.accentCyan
                            selectedTextColor: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 12
                            wrapMode: TextEdit.Wrap
                            Accessible.name: qsTr("Import report with record counts and skipped assets")
                            background: Rectangle {
                                color: reportText.hovered && !reportText.activeFocus
                                       ? ThemeModule.Theme.raisedSurface : ThemeModule.Theme.insetSurface
                                border.color: reportText.activeFocus ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.borderColor
                                border.width: reportText.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
                                radius: ThemeModule.Theme.controlRadius
                            }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Wallpaper gallery")
                color: ThemeModule.Theme.textPrimary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: ThemeModule.Theme.headingFontSize
                font.weight: Font.Bold
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 850 ? 3 : width >= 560 ? 2 : 1
                rowSpacing: 12
                columnSpacing: 12

                Repeater {
                    model: root.controller.wallpapers
                    delegate: Shared.AppCard {
                        id: wallpaperCard
                        required property int index
                        required property var modelData
                        readonly property var wallpaper: modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 220
                        Layout.preferredHeight: 236
                        padding: 10

                        Item {
                            width: parent.width
                            height: 116
                            clip: true

                            Rectangle {
                                anchors.fill: parent
                                radius: ThemeModule.Theme.panelRadius
                                color: wallpaper.id === "builtin:original"
                                       ? ThemeModule.Theme.insetSurface : ThemeModule.Theme.raisedSurface
                            }

                            Image {
                                id: wallpaperPreview
                                anchors.fill: parent
                                source: wallpaper.imageSource || ""
                                sourceSize: Qt.size(480, 280)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: false
                                visible: status === Image.Ready
                                onStatusChanged: {
                                    if (status === Image.Error && String(wallpaper.id).startsWith("custom:"))
                                        root.controller.fallbackIfMissing(wallpaper.id)
                                }
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.bottom: parent.bottom
                                anchors.margins: 8
                                radius: ThemeModule.Theme.controlRadius
                                color: Qt.alpha(ThemeModule.Theme.windowBackground, 0.9)
                                width: selectedBadge.implicitWidth + 16
                                height: selectedBadge.implicitHeight + 8
                                visible: root.controller.selectedWallpaper === wallpaper.id
                                Shared.StatusBadge {
                                    id: selectedBadge
                                    anchors.centerIn: parent
                                    text: qsTr("Selected")
                                    badgeColor: ThemeModule.Theme.accentCyan
                                }
                            }
                        }

                        RowLayout {
                            width: parent.width
                            spacing: 8
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: wallpaper.label
                                    color: ThemeModule.Theme.textPrimary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: wallpaper.description
                                    color: wallpaper.available ? ThemeModule.Theme.textSecondary : ThemeModule.Theme.error
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                            Shared.AppButton {
                                text: qsTr("Use")
                                quiet: true
                                enabled: root.controller.ready && !root.controller.busy
                                         && wallpaper.available
                                Accessible.name: qsTr("Use wallpaper %1").arg(wallpaper.label)
                                onClicked: root.controller.selectWallpaper(wallpaper.id)
                            }
                        }

                        Shared.AppButton {
                            visible: !wallpaper.builtIn
                            quiet: true
                            destructive: true
                            text: qsTr("Delete")
                            enabled: root.controller.ready && !root.controller.busy
                            Accessible.name: qsTr("Delete wallpaper %1").arg(wallpaper.label)
                            onClicked: {
                                root.pendingDeleteId = wallpaper.id
                                deleteConfirmation.open()
                            }
                        }
                    }
                }
            }

            Shared.StatePanel {
                Layout.fillWidth: true
                visible: !root.controller.ready || root.controller.busy
                height: 78
                state: root.controller.errorMessage.length > 0 ? "error" : "loading"
                title: root.controller.errorMessage.length > 0
                       ? qsTr("Wallpaper storage is unavailable")
                       : root.controller.ready ? qsTr("Updating wallpapers")
                                                : qsTr("Loading wallpaper catalog")
                message: root.controller.ready
                         ? qsTr("The image is checked and copied into the application profile.")
                         : qsTr("Wallpaper settings are waiting for the application storage service.")
            }
        }
    }
}
