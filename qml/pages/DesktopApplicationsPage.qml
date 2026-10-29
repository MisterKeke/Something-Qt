import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root
    property var controller
    implicitHeight: 600

    property int selectedId: 0
    property string selectedName: ""
    property bool selectedHasCustomIcon: false
    property int relocateId: 0
    property int iconId: 0

    Component.onCompleted: controller.setPageVisible(visible)
    onVisibleChanged: if (controller) controller.setPageVisible(visible)
    Component.onDestruction: controller.setPageVisible(false)

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Shared.AppButton {
                text: qsTr("Add application")
                enabled: root.controller.supported && !root.controller.busy
                onClicked: executablePicker.open()
            }
            Shared.AppButton {
                text: qsTr("Refresh")
                quiet: true
                enabled: !root.controller.busy
                onClicked: root.controller.reload()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: root.controller.loading || root.controller.busy; visible: running }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: !root.controller.supported || root.controller.errorMessage.length > 0
                     || (root.controller.loading && root.controller.applications.count === 0)
            state: !root.controller.supported ? "error"
                   : root.controller.errorMessage.length > 0 ? "error" : "loading"
            title: !root.controller.supported ? qsTr("Application launching unavailable")
                   : root.controller.errorMessage.length > 0 ? qsTr("Action could not be completed")
                   : qsTr("Loading saved applications")
            message: !root.controller.supported
                     ? qsTr("Saved application launching is supported on Windows.")
                     : root.controller.errorMessage
        }

        Shared.AppCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.controller.applications.count === 0 && !root.controller.loading
                     && root.controller.supported && root.controller.errorMessage.length === 0
            Shared.StatePanel {
                anchors.fill: parent
                state: "empty"
                title: qsTr("No saved applications")
                message: qsTr("Add a Windows executable to keep a launch shortcut here.")
            }
        }

        ListView {
            id: applicationList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.controller.applications.count > 0
            clip: true
            spacing: 9
            model: root.controller.applications
            boundsBehavior: Flickable.StopAtBounds

            delegate: Shared.AppCard {
                required property int applicationId
                required property string displayName
                required property string executablePath
                required property string iconFilename
                required property string iconSource
                required property string nativeIconDataUrl
                required property bool available

                width: applicationList.width
                implicitHeight: row.implicitHeight + 28

                RowLayout {
                    id: row
                    anchors.fill: parent
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 48
                        Layout.preferredHeight: 48
                        radius: ThemeModule.Theme.panelRadius
                        color: ThemeModule.Theme.raisedSurface
                        Image {
                            id: applicationIcon
                            anchors.fill: parent
                            anchors.margins: 4
                            visible: source.toString().length > 0
                            readonly property string requestedIconSource: iconSource
                            property bool customIconFailed: false
                            source: customIconFailed ? nativeIconDataUrl : requestedIconSource
                            sourceSize: Qt.size(80, 80)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            onStatusChanged: {
                                if (status === Image.Error && requestedIconSource.startsWith("image://application-icons/"))
                                    customIconFailed = true
                            }
                            onRequestedIconSourceChanged: customIconFailed = false
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: (applicationIcon.customIconFailed
                                      ? nativeIconDataUrl : iconSource).length === 0
                            text: displayName.length ? Array.from(displayName)[0].toUpperCase() : "?"
                            color: ThemeModule.Theme.focusHighlight
                            font.pixelSize: 23
                            font.weight: Font.Bold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            Layout.fillWidth: true
                            text: displayName
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: executablePath
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            Accessible.name: qsTr("Executable path")
                        }
                        Shared.StatusBadge {
                            text: available ? qsTr("Available") : qsTr("Location missing")
                            badgeColor: available ? ThemeModule.Theme.success : ThemeModule.Theme.warning
                        }
                    }

                    Shared.AppButton {
                        text: qsTr("Launch")
                        enabled: available && root.controller.supported && !root.controller.busy
                        Accessible.name: qsTr("Launch %1").arg(displayName)
                        onClicked: root.controller.launchApplication(applicationId)
                    }
                    Shared.AppButton {
                        text: qsTr("Manage")
                        quiet: true
                        enabled: !root.controller.busy
                        Accessible.name: qsTr("Edit %1").arg(displayName)
                        onClicked: {
                            root.selectedId = applicationId
                            root.selectedName = displayName
                            root.selectedHasCustomIcon = iconFilename.length > 0
                            editDialog.open()
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.notification.length > 0
            text: root.controller.notification
            color: ThemeModule.Theme.success
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            Accessible.name: text
        }
    }

    FileDialog {
        id: executablePicker
        title: qsTr("Select an application")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Windows applications (*.exe)")]
        onAccepted: root.controller.addFromFile(selectedFile)
    }

    FileDialog {
        id: relocationPicker
        title: qsTr("Locate the application executable")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Windows applications (*.exe)")]
        onAccepted: root.controller.relocateFromFile(root.relocateId, selectedFile)
    }

    FileDialog {
        id: iconPicker
        title: qsTr("Choose an application icon")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Images (*.ico *.jpg *.jpeg *.png *.webp)")]
        onAccepted: root.controller.importIconFile(root.iconId, selectedFile)
    }

    Dialog {
        id: renameDialog
        modal: true
        focus: true
        title: qsTr("Rename application")
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, parent ? parent.width - 40 : 420)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: ThemeModule.Theme.raisedSurface; radius: ThemeModule.Theme.panelRadius; border.color: ThemeModule.Theme.strongBorder }
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("Choose a display name. It may contain up to 120 characters.")
                color: ThemeModule.Theme.textSecondary
                wrapMode: Text.Wrap
            }
            Shared.AppTextField {
                id: nameField
                Layout.fillWidth: true
                accessibleLabel: qsTr("Application display name")
                placeholderText: qsTr("Application name")
                // TextField counts UTF-16 code units; the service enforces the
                // actual 120 Unicode-codepoint rule.
                maximumLength: 240
                onAccepted: root.controller.renameApplication(root.selectedId, text)
            }
        }
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            Shared.AppButton {
                text: qsTr("Cancel")
                quiet: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Shared.AppButton {
                id: renameButton
                text: qsTr("Save")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: root.controller.renameApplication(root.selectedId, nameField.text)
            }
        }
    }

    Dialogs.ConfirmationDialog {
        id: deleteDialog
        title: qsTr("Remove saved application?")
        confirmText: qsTr("Remove saved entry")
        destructive: true
        message: qsTr("The shortcut and its custom icon will be removed. The executable on disk will remain untouched.")
        onConfirmed: root.controller.removeApplication(root.selectedId)
    }

    Dialogs.ConfirmationDialog {
        id: resetIconDialog
        title: qsTr("Reset application icon?")
        confirmText: qsTr("Reset icon")
        message: qsTr("The imported icon will be removed and the application default will be shown.")
        onConfirmed: root.controller.resetIcon(root.selectedId)
    }

    Dialog {
        id: editDialog
        modal: true
        focus: true
        title: qsTr("Application actions")
        anchors.centerIn: Overlay.overlay
        width: Math.min(450, parent ? parent.width - 40 : 420)
        padding: 18
        background: Rectangle { color: ThemeModule.Theme.raisedSurface; radius: ThemeModule.Theme.panelRadius; border.color: ThemeModule.Theme.strongBorder }
        contentItem: ColumnLayout {
            spacing: 8
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Rename")
                onClicked: {
                    editDialog.close()
                    nameField.text = root.selectedName
                    renameDialog.open()
                }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Relocate executable")
                quiet: true
                enabled: root.controller.supported
                onClicked: { editDialog.close(); root.relocateId = root.selectedId; relocationPicker.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Import custom icon")
                quiet: true
                onClicked: { editDialog.close(); root.iconId = root.selectedId; iconPicker.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Reset custom icon")
                quiet: true
                visible: root.selectedHasCustomIcon
                onClicked: { editDialog.close(); resetIconDialog.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Remove saved entry")
                destructive: true
                onClicked: { editDialog.close(); deleteDialog.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Close")
                quiet: true
                onClicked: editDialog.close()
            }
        }
    }

    Connections {
        target: root.controller
        function onMutationFinished(success) {
            if (success && renameDialog.visible) renameDialog.close()
        }
    }

}
