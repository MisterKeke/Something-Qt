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
    property real viewportHeight: 760
    property string pendingDeletePath: ""
    property string pendingDeleteName: ""
    property string lastLocationKey: ""
    readonly property bool showPlaces: width >= 960
    readonly property bool showType: width >= 680
    readonly property bool showModified: width >= 900
    implicitHeight: Math.max(660, viewportHeight)
    Component.onCompleted: controller.setPageVisible(true)
    Component.onDestruction: controller.setPageVisible(false)
    function captureViewState() { return { scrollY: entryList.contentY } }
    function restoreViewState(state) { Qt.callLater(function() { entryList.contentY = Number(state.scrollY || 0) }) }
    function confirmDelete(path, name) {
        pendingDeletePath = path
        pendingDeleteName = name
        recycleConfirmation.open()
    }
    function formatSize(bytes, directory) {
        if (directory) return "—"
        const value = Number(bytes)
        if (value < 1024) return value + " B"
        if (value < 1048576) return (value / 1024).toFixed(1) + " KB"
        if (value < 1073741824) return (value / 1048576).toFixed(1) + " MB"
        return (value / 1073741824).toFixed(1) + " GB"
    }
    function formatModified(value) {
        if (!value) return "—"
        const date = new Date(value)
        return Number.isNaN(date.getTime()) ? value : Qt.formatDateTime(date, "dd MMM yyyy, HH:mm")
    }
    Connections {
        target: root.controller
        function onStateChanged() {
            if (root.controller.loading) return
            const key = root.controller.rootId + "|" + root.controller.relativePath
            if (key !== root.lastLocationKey) {
                root.lastLocationKey = key
                entryList.currentIndex = -1
                entryList.contentY = 0
            }
        }
    }
    FolderDialog {
        id: folderPicker
        title: qsTr("Choose a folder")
        onAccepted: root.controller.addSessionRootFromUrl(selectedFolder)
    }
    Dialogs.ConfirmationDialog {
        id: recycleConfirmation
        title: qsTr("Move to Recycle Bin?")
        message: qsTr("Move “%1” to the Windows Recycle Bin? You can restore it from there.").arg(root.pendingDeleteName)
        confirmText: qsTr("Move to Recycle Bin")
        destructive: true
        onConfirmed: root.controller.recycleFile(root.pendingDeletePath)
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: toolbar.implicitHeight + 28
            radius: 14
            color: "#e611131d"
            border.color: ThemeModule.Theme.border
            ColumnLayout {
                id: toolbar
                anchors.fill: parent
                anchors.margins: 14
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Shared.AppButton {
                        text: "←"; quiet: true; implicitWidth: 38
                        enabled: root.controller.canGoBack && !root.controller.loading
                        Accessible.name: qsTr("Back")
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Back")
                        onClicked: root.controller.goBack()
                    }
                    Shared.AppButton {
                        text: "→"; quiet: true; implicitWidth: 38
                        enabled: root.controller.canGoForward && !root.controller.loading
                        Accessible.name: qsTr("Forward")
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Forward")
                        onClicked: root.controller.goForward()
                    }
                    Shared.AppButton {
                        text: "↑"; quiet: true; implicitWidth: 38
                        enabled: root.controller.hasParent && !root.controller.loading
                        Accessible.name: qsTr("Parent folder")
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Parent folder")
                        onClicked: root.controller.goParent()
                    }
                    Shared.AppButton {
                        text: "↻"; quiet: true; implicitWidth: 38
                        enabled: root.controller.ready && !root.controller.loading
                        Accessible.name: qsTr("Refresh folder")
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Refresh")
                        onClicked: root.controller.refresh()
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        Layout.leftMargin: 6
                        Layout.rightMargin: 6
                        color: "#121725"
                        border.color: ThemeModule.Theme.border
                        radius: 8
                        Flickable {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            clip: true
                            contentWidth: crumbRow.implicitWidth
                            contentHeight: height
                            flickableDirection: Flickable.HorizontalFlick
                            Row {
                                id: crumbRow
                                height: parent.height
                                spacing: 3
                                Repeater {
                                    model: root.controller.breadcrumbs
                                    delegate: Row {
                                        required property var modelData
                                        required property int index
                                        height: crumbRow.height
                                        spacing: 3
                                        Label { height: parent.height; verticalAlignment: Text.AlignVCenter; text: index > 0 ? "›" : ""; color: ThemeModule.Theme.textDim }
                                        ToolButton {
                                            height: parent.height
                                            text: modelData.label
                                            enabled: !root.controller.loading
                                            Accessible.name: qsTr("Open folder %1").arg(modelData.label)
                                            onClicked: root.controller.navigateToPath(modelData.relativePath)
                                            contentItem: Text { text: parent.text; color: ThemeModule.Theme.text; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Shared.AppButton {
                        text: root.controller.addingRoot ? qsTr("Adding…") : qsTr("Choose folder")
                        enabled: root.controller.ready && !root.controller.addingRoot
                        onClicked: folderPicker.open()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Shared.AppTextField {
                        id: searchField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Search in this folder…")
                        accessibleLabel: qsTr("Filter files and folders by name")
                        maximumLength: 512
                        text: root.controller.query
                        onTextEdited: root.controller.setQuery(text)
                        Keys.onReturnPressed: root.controller.setQuery(text)
                    }
                    Shared.AppButton {
                        visible: searchField.text.length > 0
                        text: qsTr("Clear"); quiet: true
                        onClicked: { searchField.text = ""; root.controller.setQuery("") }
                    }
                    BusyIndicator { visible: root.controller.loading; running: visible; implicitWidth: 28; implicitHeight: 28 }
                    Label { text: qsTr("%1 items").arg(root.controller.entries.count); color: ThemeModule.Theme.textDim; font.pixelSize: 12 }
                }
                Flow {
                    Layout.fillWidth: true
                    visible: !root.showPlaces
                    spacing: 6
                    Repeater {
                        model: root.controller.places
                        delegate: Shared.AppButton {
                            required property var modelData
                            text: modelData.label
                            quiet: modelData.rootId !== root.controller.rootId
                            enabled: !root.controller.loading
                            onClicked: root.controller.openPlace(modelData.rootId)
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12
            Rectangle {
                visible: root.showPlaces
                Layout.preferredWidth: 210
                Layout.fillHeight: true
                radius: 14
                color: "#e611131d"
                border.color: ThemeModule.Theme.border
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 12
                    Label { text: qsTr("Places"); color: ThemeModule.Theme.text; font.pixelSize: 16; font.bold: true }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 5
                        model: root.controller.places
                        delegate: Button {
                            required property var modelData
                            width: ListView.view.width
                            height: 42
                            enabled: !root.controller.loading && !root.controller.addingRoot
                            onClicked: root.controller.openPlace(modelData.rootId)
                            contentItem: RowLayout {
                                spacing: 9
                                Label { text: "▣"; color: ThemeModule.Theme.accentHover; font.pixelSize: 20 }
                                Label { Layout.fillWidth: true; text: modelData.label; color: ThemeModule.Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                            }
                            background: Rectangle {
                                radius: 8
                                color: modelData.rootId === root.controller.rootId ? "#283452" : parent.hovered ? "#202638" : "transparent"
                                border.color: parent.activeFocus ? ThemeModule.Theme.cyan : "transparent"
                                Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
                            }
                        }
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Added folders stay available for this session.")
                        color: ThemeModule.Theme.textDim
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                radius: 14
                color: "#e611131d"
                border.color: ThemeModule.Theme.border
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        Layout.leftMargin: 12
                        Layout.rightMargin: 24
                        spacing: 12
                        Label { Layout.fillWidth: true; text: qsTr("Name"); color: ThemeModule.Theme.textDim; font.pixelSize: 11; font.bold: true }
                        Label { visible: root.showType; Layout.preferredWidth: 100; text: qsTr("Type"); color: ThemeModule.Theme.textDim; font.pixelSize: 11; font.bold: true }
                        Label { visible: root.showModified; Layout.preferredWidth: 160; text: qsTr("Date modified"); color: ThemeModule.Theme.textDim; font.pixelSize: 11; font.bold: true }
                        Label { Layout.preferredWidth: 88; text: qsTr("Size"); color: ThemeModule.Theme.textDim; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignRight }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: ThemeModule.Theme.border }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Shared.StatePanel {
                            anchors.centerIn: parent
                            width: Math.min(460, parent.width)
                            visible: root.controller.entries.count === 0
                            state: root.controller.errorMessage.length > 0 ? "error" : root.controller.loading || !root.controller.ready ? "loading" : "empty"
                            title: root.controller.errorMessage.length > 0 ? qsTr("Folder unavailable")
                                : root.controller.loading || !root.controller.ready ? qsTr("Reading folder…")
                                : root.controller.query.length > 0 ? qsTr("No matching items") : qsTr("This folder is empty")
                            message: root.controller.errorMessage.length > 0 ? root.controller.errorMessage
                                : root.controller.query.length > 0 ? qsTr("Try another search.") : qsTr("Choose another place or add a folder.")
                        }
                        ListView {
                            id: entryList
                            anchors.fill: parent
                            anchors.topMargin: 6
                            visible: root.controller.entries.count > 0
                            enabled: !root.controller.loading
                            clip: true
                            model: root.controller.entries
                            boundsBehavior: Flickable.StopAtBounds
                            spacing: 3
                            activeFocusOnTab: true
                            currentIndex: -1
                            Accessible.name: qsTr("Folder contents")
                            Keys.onReturnPressed: root.controller.activateSelected()
                            Keys.onEnterPressed: root.controller.activateSelected()
                            Keys.onDeletePressed: {
                                if (root.controller.entries.selectedCanRecycle)
                                    root.confirmDelete(root.controller.entries.selectedPath, root.controller.entries.selectedName)
                            }
                            onCurrentIndexChanged: {
                                const current = itemAtIndex(currentIndex)
                                if (current) root.controller.selectEntry(current.relativePath)
                            }
                            delegate: Rectangle {
                                id: entryRow
                                required property int index
                                required property string name
                                required property string relativePath
                                required property string entryType
                                required property string modified
                                required property var size
                                required property bool directory
                                required property bool canNavigate
                                required property bool canOpen
                                required property bool canRecycle
                                required property bool selected
                                width: entryList.width - 12
                                height: 45
                                radius: 7
                                color: selected ? "#2b3858" : rowMouse.containsMouse ? "#22293c" : index % 2 ? "#70171c2b" : "transparent"
                                border.color: selected ? "#647ba9" : "transparent"
                                Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
                                Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
                                Accessible.role: Accessible.ListItem
                                Accessible.name: name + ", " + entryType
                                MouseArea {
                                    id: rowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: { entryList.currentIndex = index; root.controller.selectEntry(relativePath); entryList.forceActiveFocus() }
                                    onDoubleClicked: root.controller.activateEntry(relativePath)
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    spacing: 12
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 10
                                        Image {
                                            Layout.preferredWidth: 22
                                            Layout.preferredHeight: 22
                                            source: entryType === "link" ? "qrc:/something/assets/icons/link.svg" : directory ? "qrc:/something/assets/icons/folder.svg" : "qrc:/something/assets/icons/file.svg"
                                            sourceSize: Qt.size(28, 28)
                                        }
                                        Label { Layout.fillWidth: true; text: name; color: canNavigate || canOpen ? ThemeModule.Theme.text : ThemeModule.Theme.textDim; elide: Text.ElideMiddle; font.pixelSize: 13 }
                                    }
                                    Label { visible: root.showType; Layout.preferredWidth: 100; text: entryType === "link" ? qsTr("Link") : directory ? qsTr("Folder") : qsTr("File"); color: ThemeModule.Theme.textDim; font.pixelSize: 12; elide: Text.ElideRight }
                                    Label { visible: root.showModified; Layout.preferredWidth: 160; text: root.formatModified(modified); color: ThemeModule.Theme.textDim; font.pixelSize: 11; elide: Text.ElideRight }
                                    Label { Layout.preferredWidth: 88; text: root.formatSize(size, directory); color: ThemeModule.Theme.textDim; font.pixelSize: 12; horizontalAlignment: Text.AlignRight }
                                }
                            }
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: ThemeModule.Theme.border }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 9
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            text: root.controller.entries.selectedName.length > 0 ? root.controller.entries.selectedName : qsTr("Double-click a folder or file to open it")
                            color: ThemeModule.Theme.textDim
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                        }
                        Label { visible: root.controller.truncated; text: qsTr("First 2,000 matches"); color: ThemeModule.Theme.warning; font.pixelSize: 11 }
                        Shared.AppButton { visible: root.controller.hasMore; text: qsTr("Load more"); quiet: true; enabled: !root.controller.loading; onClicked: root.controller.loadMore() }
                        Shared.AppButton { text: qsTr("Open"); quiet: true; enabled: root.controller.entries.selectedPath.length > 0 && !root.controller.actionBusy && !root.controller.loading; onClicked: root.controller.activateSelected() }
                        Shared.AppButton {
                            text: qsTr("Recycle")
                            destructive: true
                            visible: root.controller.nativeActionsSupported
                            enabled: root.controller.entries.selectedCanRecycle && !root.controller.actionBusy && !root.controller.loading
                            onClicked: root.confirmDelete(root.controller.entries.selectedPath, root.controller.entries.selectedName)
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.controller.errorMessage.length > 0 || root.controller.notification.length > 0
            text: root.controller.errorMessage.length > 0 ? root.controller.errorMessage : root.controller.notification
            color: root.controller.errorMessage.length > 0 ? ThemeModule.Theme.error : ThemeModule.Theme.success
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }
    }
}
