import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root
    property var controller: setupsController
    signal navigateRequested(string routeId)
    property double pendingDeleteId: 0
    property double editingId: 0
    property url selectedIconFile: ""
    property bool removeExistingIcon: false
    property var selectedApplicationIds: []
    implicitHeight: setupsColumn.implicitHeight + 24

    function appForId(id) {
        const rows = controller.applications
        for (let i = 0; i < rows.length; ++i)
            if (Number(rows[i].id) === Number(id)) return rows[i]
        return null
    }

    function captureViewState() {
        return { setupListY: setupList.contentY }
    }

    function restoreViewState(state) {
        Qt.callLater(function() {
            setupList.contentY = Number(state.setupListY || 0)
        })
    }

    function openNew() {
        editingId = 0
        nameField.text = ""
        descriptionField.text = ""
        selectedApplicationIds = []
        selectedIconFile = ""
        removeExistingIcon = false
        editorDialog.existingIconFilename = ""
        controller.clearMessages()
        editorDialog.title = qsTr("Create setup")
        editorDialog.open()
        Qt.callLater(function() { nameField.forceActiveFocus() })
    }

    function openEdit(setupId, name, description, applicationIds, iconFilename) {
        editingId = Number(setupId)
        nameField.text = name
        descriptionField.text = description
        selectedApplicationIds = applicationIds.slice(0)
        selectedIconFile = ""
        removeExistingIcon = false
        controller.clearMessages()
        editorDialog.title = qsTr("Edit setup")
        editorDialog.existingIconFilename = iconFilename
        editorDialog.open()
    }

    function toggleApplication(id, checked) {
        const next = selectedApplicationIds.slice(0)
        const current = next.indexOf(Number(id))
        if (checked && current < 0) next.push(Number(id))
        if (!checked && current >= 0) next.splice(current, 1)
        selectedApplicationIds = next
    }

    function moveApplication(index, offset) {
        const target = index + offset
        if (target < 0 || target >= selectedApplicationIds.length) return
        const next = selectedApplicationIds.slice(0)
        const value = next[index]
        next[index] = next[target]
        next[target] = value
        selectedApplicationIds = next
    }

    Component.onCompleted: controller.setPageVisible(visible)
    onVisibleChanged: if (controller) controller.setPageVisible(visible)
    Component.onDestruction: controller.setPageVisible(false)

    Connections {
        target: root.controller
        function onMutationFinished(success) {
            if (success && editorDialog.visible) editorDialog.close()
        }
    }

    ColumnLayout {
        id: setupsColumn
        width: root.width
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Setups")
                color: ThemeModule.Theme.textPrimary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: ThemeModule.Theme.headingFontSize
                font.weight: Font.DemiBold
            }
            BusyIndicator { visible: root.controller.loading; running: visible; implicitWidth: 24; implicitHeight: 24 }
            Shared.AppButton {
                text: qsTr("Create setup")
                enabled: !root.controller.busy && !root.controller.loading
                onClicked: root.openNew()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.errorMessage.length > 0
            text: root.controller.errorMessage
            color: ThemeModule.Theme.error
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
        Label {
            Layout.fillWidth: true
            visible: root.controller.notification.length > 0
            text: root.controller.notification
            color: ThemeModule.Theme.success
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: root.controller.launching
            text: qsTr("Starting applications in the saved order. Applications already opened will keep running.")
            color: ThemeModule.Theme.textSecondary
            wrapMode: Text.Wrap
        }
        Shared.AppButton {
            visible: root.controller.launching
            enabled: root.controller.launching
            text: qsTr("Stop remaining launches")
            quiet: true
            onClicked: root.controller.cancelLaunch()
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller.loading && root.controller.setups.count === 0
            state: "loading"
            title: qsTr("Loading setups")
            message: qsTr("Reading saved setup details and application membership.")
        }
        Shared.StatePanel {
            Layout.fillWidth: true
            visible: !root.controller.loading && root.controller.setups.count === 0
            state: root.controller.errorMessage.length > 0 ? "error" : "empty"
            title: root.controller.errorMessage.length > 0 ? qsTr("Setups are unavailable") : qsTr("No setups yet")
            message: root.controller.errorMessage.length > 0
                     ? qsTr("Retry to load your saved setups.")
                     : qsTr("Create a setup to launch a chosen group of saved applications in order.")
        }
        Shared.AppButton {
            Layout.alignment: Qt.AlignHCenter
            visible: !root.controller.loading && root.controller.setups.count === 0
            text: root.controller.errorMessage.length > 0 ? qsTr("Retry") : qsTr("Create setup")
            quiet: root.controller.errorMessage.length > 0
            onClicked: root.controller.errorMessage.length > 0 ? root.controller.reload() : root.openNew()
        }

        ListView {
            id: setupList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(680, Math.max(0, contentHeight))
            visible: root.controller.setups.count > 0
            clip: true
            spacing: 9
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Saved setups")
            model: root.controller.setups
            delegate: Shared.AppCard {
                width: ListView.view.width
                height: cardContent.implicitHeight + 32
                implicitHeight: cardContent.implicitHeight + 32
                padding: 16
                ColumnLayout {
                    id: cardContent
                    anchors.fill: parent
                    spacing: 11
                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle {
                            Layout.preferredWidth: 50
                            Layout.preferredHeight: 50
                            radius: ThemeModule.Theme.panelRadius
                            color: ThemeModule.Theme.raisedSurface
                            border.color: ThemeModule.Theme.borderColor
                            Image {
                                id: setupIconImage
                                anchors.fill: parent
                                anchors.margins: 4
                                source: model.iconSource
                                visible: source.length > 0
                                         && status !== Image.Error
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                Accessible.name: qsTr("Setup icon")
                            }
                            Label {
                                anchors.centerIn: parent
                                visible: model.iconSource.length === 0
                                         || setupIconImage.status === Image.Error
                                text: model.name.length > 0 ? model.name.slice(0, 1).toUpperCase() : "S"
                                color: ThemeModule.Theme.accentCyan
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 20
                                font.weight: Font.Bold
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: model.name
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: model.description.length > 0 ? model.description : model.applicationSummary
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }
                        Shared.StatusBadge {
                            text: qsTr("%1 apps").arg(model.applicationIds.length)
                            badgeColor: ThemeModule.Theme.accentCyan
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Launch order: %1").arg(model.applicationSummary || qsTr("No saved applications"))
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Shared.AppButton {
                            text: qsTr("Start")
                            enabled: !root.controller.busy && model.applicationIds.length > 0
                            onClicked: root.controller.startSetup(model.setupId)
                        }
                        Shared.AppButton {
                            text: qsTr("Edit")
                            quiet: true
                            enabled: !root.controller.busy
                            onClicked: root.openEdit(model.setupId, model.name, model.description,
                                                     model.applicationIds, model.iconFilename)
                        }
                        Shared.AppButton {
                            text: qsTr("Delete")
                            destructive: true
                            quiet: true
                            enabled: !root.controller.busy
                            onClicked: {
                                root.pendingDeleteId = model.setupId
                                deleteDialog.message = qsTr("Delete ‘%1’? Its saved applications will remain on your computer.").arg(model.name)
                                deleteDialog.open()
                            }
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: iconPicker
        title: qsTr("Choose a setup icon")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Images (*.ico *.jpg *.jpeg *.png *.webp)")]
        onAccepted: {
            root.selectedIconFile = selectedFile
            root.removeExistingIcon = false
        }
    }

    Dialog {
        id: editorDialog
        property string existingIconFilename: ""
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(680, parent ? parent.width - 32 : 640)
        height: Math.min(760, parent ? parent.height - 36 : 700)
        padding: 20
        closePolicy: root.controller.busy ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        title: qsTr("Create setup")
        Accessible.name: title
        background: Rectangle { color: ThemeModule.Theme.raisedSurface; border.color: ThemeModule.Theme.strongBorder; radius: ThemeModule.Theme.panelRadius }
        contentItem: Flickable {
            clip: true
            contentWidth: width
            contentHeight: editorContent.implicitHeight
            ScrollBar.vertical: ThemeModule.AppScrollBar {}
            ColumnLayout {
                id: editorContent
                width: parent.width
                spacing: 10
                Shared.AppTextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Setup name")
                    accessibleLabel: qsTr("Setup name, required, up to 120 characters")
                }
                TextArea {
                    id: descriptionField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 82
                    placeholderText: qsTr("Description (optional, up to 300 characters)")
                    Accessible.name: qsTr("Setup description")
                    hoverEnabled: true
                    color: ThemeModule.Theme.textPrimary
                    placeholderTextColor: ThemeModule.Theme.textSecondary
                    selectionColor: ThemeModule.Theme.accentCyan
                    selectedTextColor: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.bodyFontSize
                    wrapMode: TextEdit.Wrap
                    onTextChanged: {
                        const characters = Array.from(text)
                        if (characters.length > 300) text = characters.slice(0, 300).join("")
                    }
                    background: Rectangle { color: descriptionField.hovered && !descriptionField.activeFocus ? ThemeModule.Theme.raisedSurface : ThemeModule.Theme.insetSurface; border.color: descriptionField.activeFocus ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.borderColor; border.width: descriptionField.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth; radius: ThemeModule.Theme.controlRadius }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Shared.AppButton {
                        text: qsTr("Choose icon")
                        quiet: true
                        onClicked: iconPicker.open()
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.selectedIconFile.toString().length > 0 ? qsTr("New icon selected")
                              : root.removeExistingIcon ? qsTr("Existing icon will be removed")
                              : editorDialog.existingIconFilename.length > 0 ? qsTr("Current icon retained")
                              : qsTr("No custom icon")
                        color: ThemeModule.Theme.textSecondary
                        elide: Text.ElideMiddle
                    }
                    Shared.AppButton {
                        text: qsTr("Remove icon")
                        quiet: true
                        visible: editorDialog.existingIconFilename.length > 0 || root.selectedIconFile.toString().length > 0
                        onClicked: { root.selectedIconFile = ""; root.removeExistingIcon = true }
                    }
                }
                Label {
                    text: qsTr("Choose one or more saved applications. Their order below controls launch order.")
                    color: ThemeModule.Theme.textSecondary
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Repeater {
                    model: root.controller.applications
                    delegate: ThemeModule.AppCheckBox {
                        Layout.fillWidth: true
                        text: modelData.displayName
                        checked: root.selectedApplicationIds.indexOf(Number(modelData.id)) >= 0
                        Accessible.name: qsTr("Include %1 in setup").arg(modelData.displayName)
                        onToggled: root.toggleApplication(modelData.id, checked)
                        contentItem: Text {
                            text: parent.text
                            color: parent.enabled ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textDisabled
                            leftPadding: 8
                            verticalAlignment: Text.AlignVCenter
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                            elide: Text.ElideRight
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.controller.applications.length === 0
                    text: qsTr("Save an application first, then add it to a setup.")
                    color: ThemeModule.Theme.textSecondary
                    wrapMode: Text.Wrap
                }
                Shared.AppButton {
                    visible: root.controller.applications.length === 0
                    text: qsTr("Open saved applications")
                    quiet: true
                    onClicked: {
                        editorDialog.close()
                        root.navigateRequested("applications")
                    }
                }
                Label {
                    text: qsTr("Launch order")
                    color: ThemeModule.Theme.textPrimary
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: root.selectedApplicationIds
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        property var app: root.appForId(modelData)
                        Label {
                            Layout.fillWidth: true
                            text: parent.app ? parent.app.displayName : qsTr("Removed application")
                            color: ThemeModule.Theme.textSecondary
                            elide: Text.ElideRight
                        }
                        Shared.AppButton {
                            text: qsTr("Up")
                            quiet: true
                            enabled: index > 0
                            Accessible.name: qsTr("Move %1 earlier").arg(parent.app ? parent.app.displayName : qsTr("application"))
                            onClicked: root.moveApplication(index, -1)
                        }
                        Shared.AppButton {
                            text: qsTr("Down")
                            quiet: true
                            enabled: index < root.selectedApplicationIds.length - 1
                            Accessible.name: qsTr("Move %1 later").arg(parent.app ? parent.app.displayName : qsTr("application"))
                            onClicked: root.moveApplication(index, 1)
                        }
                    }
                }
                Label {
                    id: editorError
                    Layout.fillWidth: true
                    visible: text.length > 0 || root.controller.errorMessage.length > 0
                    text: root.controller.errorMessage
                    color: ThemeModule.Theme.error
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
            }
        }
        footer: RowLayout {
            spacing: 8
            Item { Layout.fillWidth: true }
            Shared.AppButton { text: qsTr("Cancel"); quiet: true; enabled: !root.controller.busy; onClicked: editorDialog.close() }
            Shared.AppButton {
                text: root.controller.busy ? qsTr("Saving…") : qsTr("Save setup")
                enabled: !root.controller.busy
                onClicked: root.controller.saveSetupFromFile(root.editingId, nameField.text,
                    descriptionField.text, root.selectedApplicationIds, root.selectedIconFile,
                    root.removeExistingIcon)
            }
        }
    }

    Dialogs.ConfirmationDialog {
        id: deleteDialog
        title: qsTr("Delete setup?")
        confirmText: qsTr("Delete setup")
        destructive: true
        onConfirmed: root.controller.removeSetup(root.pendingDeleteId)
    }

}
