import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

ColumnLayout {
    id: root
    property var runningController: runningApplicationsController
    property var rates: currencyController
    signal navigateRequested(string routeId)
    spacing: 14

    Shared.AppCard {
        Layout.fillWidth: true
        padding: 16
        ColumnLayout {
            width: parent.width
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Favorites")
                    color: ThemeModule.Theme.text
                    font.family: ThemeModule.Theme.displayFont
                    font.pixelSize: 18
                    font.weight: Font.Bold
                }
                Shared.AppButton {
                    text: qsTr("Refresh")
                    quiet: true
                    implicitWidth: 68
                    implicitHeight: 30
                    font.pixelSize: 11
                    leftPadding: 8
                    rightPadding: 8
                    enabled: root.rates.favorites.count > 0 && !root.rates.favoritesLoading
                    onClicked: root.rates.refreshFavorites()
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.rates.favoritesError.length > 0
                text: root.rates.favoritesError
                color: ThemeModule.Theme.warning
                wrapMode: Text.Wrap
                font.pixelSize: 12
            }
            Label {
                Layout.fillWidth: true
                visible: root.rates.favorites.count === 0
                text: root.rates.favoritesLoading ? qsTr("Loading currency pairs…")
                                                : qsTr("Keep your favorite currency pairs here.")
                color: ThemeModule.Theme.textDim
                wrapMode: Text.Wrap
                font.pixelSize: 12
            }
            Repeater {
                model: root.rates.favorites
                delegate: Rectangle {
                    required property string baseCode
                    required property string quoteCode
                    required property bool found
                    required property real rate
                    required property string rateStatus
                    Layout.fillWidth: true
                    implicitHeight: 60
                    radius: 9
                    color: pairMouse.containsMouse ? "#252a40" : "#1b2031"
                    border.color: ThemeModule.Theme.border
                    MouseArea {
                        id: pairMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            root.rates.selectFavorite(baseCode, quoteCode)
                            root.navigateRequested("currencies")
                        }
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 9
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 to %2").arg(baseCode).arg(quoteCode)
                                color: ThemeModule.Theme.text
                                font.bold: true
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: found ? qsTr("1 %1 = %2 %3").arg(baseCode).arg(Number(rate).toFixed(4)).arg(quoteCode)
                                            : rateStatus === "loading" ? qsTr("Updating…") : qsTr("Rate unavailable")
                                color: ThemeModule.Theme.textDim
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                        Shared.RemoveFavoriteButton {
                            pairName: qsTr("%1 to %2").arg(baseCode).arg(quoteCode)
                            enabled: !root.rates.favoritesBusy
                            onClicked: root.rates.removeFavorite(baseCode, quoteCode)
                        }
                    }
                }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: root.rates.favorites.count === 0 ? qsTr("Add a currency pair") : qsTr("All currencies  ›")
                quiet: true
                onClicked: root.navigateRequested("currencies")
            }
        }
    }
    Shared.WeatherSummaryCard {
        Layout.fillWidth: true
        onWeatherRequested: root.navigateRequested("weather")
    }
    Shared.AppCard {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        padding: 15

        ColumnLayout {
            width: parent.width
            spacing: 9
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Running apps")
                    color: ThemeModule.Theme.text
                    font.family: ThemeModule.Theme.displayFont
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                BusyIndicator {
                    visible: root.runningController.loading
                    running: visible
                    implicitWidth: 24
                    implicitHeight: 24
                    Accessible.name: qsTr("Loading running applications")
                }
                Shared.AppButton {
                    text: qsTr("Refresh")
                    quiet: true
                    implicitWidth: 68
                    implicitHeight: 30
                    font.pixelSize: 11
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: root.runningController.reload()
                }
            }
            Shared.StatePanel {
                Layout.fillWidth: true
                visible: !root.runningController.supported || !root.runningController.ready
                         || root.runningController.loading
                         || root.runningController.errorMessage.length > 0
                         || (root.runningController.windows.count === 0
                             && root.runningController.hasSnapshot)
                state: !root.runningController.supported
                       || root.runningController.errorMessage.length > 0 ? "error"
                       : root.runningController.loading || !root.runningController.ready
                         ? "loading" : "empty"
                title: !root.runningController.supported
                       ? qsTr("Running-window control unavailable")
                       : root.runningController.errorMessage.length > 0
                         ? qsTr("Running-app data is unavailable")
                       : root.runningController.loading || !root.runningController.ready
                         ? qsTr("Checking available windows") : qsTr("No running applications")
                message: !root.runningController.supported
                         ? qsTr("Window enumeration and activation are supported on Windows.")
                         : root.runningController.errorMessage.length > 0
                           ? root.runningController.errorMessage
                         : root.runningController.loading || !root.runningController.ready
                           ? qsTr("Checking visible desktop windows.")
                           : qsTr("Visible application windows will appear here while Something is active.")
            }
            Label {
                Layout.fillWidth: true
                visible: root.runningController.windows.count > 0
                         && root.runningController.errorMessage.length > 0
                text: root.runningController.errorMessage
                color: ThemeModule.Theme.warning
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            ListView {
                id: dashboardWindows
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 340)
                visible: root.runningController.windows.count > 0
                clip: true
                spacing: 5
                model: root.runningController.windows
                delegate: RowLayout {
                    required property string windowId
                    required property string title
                    required property string processName
                    required property string iconDataUrl
                    required property bool active
                    required property bool minimized
                    width: dashboardWindows.width
                    height: 44
                    spacing: 9
                    Image {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        source: iconDataUrl
                        sourceSize: Qt.size(48, 48)
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        Accessible.name: qsTr("Application icon for %1").arg(processName)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            Layout.fillWidth: true
                            text: title.length ? title : qsTr("Untitled window")
                            font.pixelSize: 12
                            color: ThemeModule.Theme.text
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            Accessible.name: qsTr("Window title: %1").arg(title)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: processName + (active ? " · " + qsTr("Active")
                                                      : minimized ? " · " + qsTr("Minimized") : "")
                            color: ThemeModule.Theme.textDim
                            font.pixelSize: 11
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                    }
                    Shared.AppButton {
                        text: "↗"
                        implicitWidth: 32
                        implicitHeight: 32
                        leftPadding: 0
                        rightPadding: 0
                        quiet: !active
                        enabled: !root.runningController.activating
                        Accessible.name: qsTr("Activate window: %1").arg(title)
                        onClicked: root.runningController.activateWindow(windowId)
                    }
                }
            }
        }
    }
}
