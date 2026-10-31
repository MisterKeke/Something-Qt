import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root
    property var controller
    implicitHeight: 570

    Component.onCompleted: controller.setPageVisible(visible)
    onVisibleChanged: if (controller) controller.setPageVisible(visible)
    Component.onDestruction: controller.setPageVisible(false)

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Visible desktop windows")
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 13
            }
            BusyIndicator {
                running: root.visible && (root.controller.loading || root.controller.activating)
                visible: running
            }
            Shared.AppButton {
                text: qsTr("Refresh")
                quiet: true
                enabled: !root.controller.loading
                onClicked: root.controller.reload()
            }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: !root.controller.supported || root.controller.errorMessage.length > 0
                     || (root.controller.loading && root.controller.windows.count === 0)
            state: !root.controller.supported || root.controller.errorMessage.length > 0
                   ? "error" : "loading"
            title: !root.controller.supported ? qsTr("Running-window control unavailable")
                   : root.controller.errorMessage.length > 0 ? qsTr("Could not refresh windows")
                   : qsTr("Scanning visible windows")
            message: !root.controller.supported
                     ? qsTr("Window enumeration and activation are supported on Windows.")
                     : root.controller.errorMessage
        }

        Shared.AppCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.controller.supported && root.controller.windows.count === 0
                     && !root.controller.loading && root.controller.errorMessage.length === 0
            Shared.StatePanel {
                anchors.fill: parent
                state: "empty"
                title: qsTr("No eligible windows")
                message: qsTr("Visible application windows will appear here while this page is active.")
            }
        }

        ListView {
            id: windowList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.controller.windows.count > 0
            clip: true
            spacing: 9
            model: root.controller.windows
            boundsBehavior: Flickable.StopAtBounds

            delegate: Shared.AppCard {
                required property string windowId
                required property string title
                required property string processName
                required property string iconDataUrl
                required property bool minimized
                required property bool active

                width: windowList.width
                implicitHeight: row.implicitHeight + 26
                RowLayout {
                    id: row
                    anchors.fill: parent
                    spacing: 12
                    Rectangle {
                        Layout.preferredWidth: 46
                        Layout.preferredHeight: 46
                        radius: ThemeModule.Theme.panelRadius
                        color: ThemeModule.Theme.raisedSurface
                        Image {
                            anchors.fill: parent
                            anchors.margins: 3
                            source: iconDataUrl
                            sourceSize: Qt.size(80, 80)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: iconDataUrl.length === 0
                            text: processName.length ? processName[0].toUpperCase() : "▣"
                            color: ThemeModule.Theme.focusHighlight
                            font.pixelSize: 22
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            Layout.fillWidth: true
                            text: title.length ? title : qsTr("Untitled window")
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: processName
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 12
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                        RowLayout {
                            spacing: 6
                            Shared.StatusBadge {
                                visible: active
                                text: qsTr("Active")
                                badgeColor: ThemeModule.Theme.accentCyan
                            }
                            Shared.StatusBadge {
                                visible: minimized
                                text: qsTr("Minimized")
                                badgeColor: ThemeModule.Theme.warning
                            }
                        }
                    }
                    Shared.AppButton {
                        text: qsTr("Activate")
                        quiet: !active
                        enabled: !root.controller.activating
                        Accessible.name: qsTr("Activate window: %1").arg(title)
                        onClicked: root.controller.activateWindow(windowId)
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.notification.length > 0
            text: root.controller.notification
            color: ThemeModule.Theme.warning
            wrapMode: Text.Wrap
            Accessible.name: text
        }
    }
}
