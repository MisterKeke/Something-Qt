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
    implicitHeight: 640

    property string pendingDeleteId: ""
    property url sourceDatabaseFile: ""
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

    Flickable {
        id: pageScroll
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: pageColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds

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
                            color: ThemeModule.Theme.text
                            font.family: ThemeModule.Theme.displayFont
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Choose a built-in scene or import your own JPEG, PNG, or WebP image. Imported files are copied into this profile and limited to 20 MB.")
                            color: ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
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
                    font.family: ThemeModule.Theme.bodyFont
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
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
                        color: ThemeModule.Theme.text
                        font.family: ThemeModule.Theme.displayFont
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("First day of the Dashboard week")
                            color: ThemeModule.Theme.textDim
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
                            color: ThemeModule.Theme.textDim
                        }
                        Label {
                            text: root.settingsController.localTimeZone
                            color: ThemeModule.Theme.text
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
                        color: ThemeModule.Theme.text
                        font.family: ThemeModule.Theme.displayFont
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Supports source schema versions 24 and 25. Built-in Qt wallpaper choices are allowed and kept. The source remains untouched; a consistent copy is made after you confirm it is closed.")
                        color: ThemeModule.Theme.textDim
                        font.family: ThemeModule.Theme.bodyFont
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
                        font.family: ThemeModule.Theme.bodyFont
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(200, reportText.implicitHeight + 16)
                        visible: root.importController.reportJson.length > 0
                        TextArea {
                            id: reportText
                            text: root.importReportText(root.importController.reportJson)
                            readOnly: true
                            textFormat: TextEdit.PlainText
                            color: ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
                            font.pixelSize: 12
                            wrapMode: TextEdit.Wrap
                            Accessible.name: qsTr("Import report with record counts and skipped assets")
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Wallpaper gallery")
                color: ThemeModule.Theme.text
                font.family: ThemeModule.Theme.displayFont
                font.pixelSize: 20
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
                                radius: 10
                                color: wallpaper.id === "builtin:original" ? "#0d101a" : "#1b2034"
                                gradient: Gradient {
                                    GradientStop { position: 0.0; color: "#171b2a" }
                                    GradientStop { position: 0.55; color: "#10141f" }
                                    GradientStop { position: 1.0; color: "#260f20" }
                                }
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
                                radius: 9
                                color: "#c906070d"
                                width: selectedBadge.implicitWidth + 16
                                height: selectedBadge.implicitHeight + 8
                                visible: root.controller.selectedWallpaper === wallpaper.id
                                Shared.StatusBadge {
                                    id: selectedBadge
                                    anchors.centerIn: parent
                                    text: qsTr("Selected")
                                    badgeColor: ThemeModule.Theme.cyan
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
                                    color: ThemeModule.Theme.text
                                    font.family: ThemeModule.Theme.displayFont
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: wallpaper.description
                                    color: wallpaper.available ? ThemeModule.Theme.textDim : ThemeModule.Theme.error
                                    font.family: ThemeModule.Theme.bodyFont
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
