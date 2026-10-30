import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root
    property var controller: weatherController
    property string cityQuery: ""
    implicitHeight: pageColumn.implicitHeight

    function captureViewState() {
        return { scrollY: pageScroll.contentY, cityQuery: cityField.text }
    }
    function restoreViewState(state) {
        if (!state) return
        if (state.cityQuery !== undefined) cityField.text = String(state.cityQuery)
        if (state.scrollY !== undefined) pageScroll.contentY = Number(state.scrollY)
    }
    function formatDate(value) {
        // Use the forecast's calendar date, including cached days. Row zero
        // can belong to yesterday or to a city in another time zone.
        const parsed = new Date(String(value) + "T12:00:00")
        return Number.isNaN(parsed.getTime()) ? value
              : parsed.toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" })
    }
    function temperature(value, available) {
        return available ? Math.round(Number(value)) + "°" : "—"
    }
    Component.onCompleted: controller.updateSolarInfo()

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
            spacing: 14

            Shared.AppCard {
                Layout.fillWidth: true
                padding: 18
                ColumnLayout {
                    width: parent.width
                    spacing: 12
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Label {
                                text: qsTr("Your location")
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 19
                                font.weight: Font.DemiBold
                            }
                            Label {
                                text: controller.primaryLocation
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                            }
                            Label {
                                text: controller.primaryStatus
                                visible: text.length > 0
                                color: controller.primaryError.length ? ThemeModule.Theme.warning : ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }
                        Shared.AppButton {
                            text: controller.primaryRefreshing ? qsTr("Refreshing…") : qsTr("Refresh")
                            enabled: !controller.primaryRefreshing && !controller.primaryLoading
                            Accessible.name: qsTr("Refresh saved location forecast")
                            onClicked: controller.refreshPrimary()
                        }
                    }

                    Shared.AppButton {
                        visible: controller.primaryForecast.count === 0 && controller.locationAvailable
                        enabled: !controller.primaryLoading && !controller.primaryRefreshing
                        text: qsTr("Use device location")
                        Accessible.name: qsTr("Use native device location for the saved forecast")
                        onClicked: controller.requestDeviceLocation()
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: controller.primaryError.length > 0
                        text: controller.primaryError
                        color: ThemeModule.Theme.warning
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }

                    Shared.StatePanel {
                        Layout.fillWidth: true
                        visible: controller.primaryForecast.count === 0
                        height: visible ? implicitHeight : 0
                        state: controller.primaryLoading || controller.primaryRefreshing ? "loading"
                             : controller.primaryError.length > 0 ? "error" : "empty"
                        title: controller.primaryLoading ? qsTr("Loading saved forecast")
                             : controller.primaryError.length ? qsTr("Primary forecast unavailable")
                             : qsTr("No saved primary location")
                        message: controller.primaryError.length ? controller.primaryError
                               : qsTr("Device location can save your primary forecast. City search below remains available.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: controller.primaryForecast.count > 0
                        spacing: 18
                        ColumnLayout {
                            Layout.preferredWidth: 165
                            spacing: 4
                            Label {
                                text: controller.primaryCurrent.icon || "🌡️"
                                color: ThemeModule.Theme.textPrimary
                                font.pixelSize: 36
                            }
                            Label {
                                text: root.temperature(controller.primaryCurrent.temperature,
                                    controller.primaryCurrent.hasTemperature === true)
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 38
                                font.weight: Font.Bold
                            }
                            Label {
                                text: controller.primaryCurrent.condition || qsTr("Weather unavailable")
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Feels like %1").arg(root.temperature(
                                    controller.primaryCurrent.feelsLike,
                                    controller.primaryCurrent.hasFeelsLike === true))
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                            }
                            Label {
                                Layout.fillWidth: true
                                text: controller.primaryCurrent.hasWind === true
                                      ? qsTr("Wind %1 km/h").arg(Math.round(controller.primaryCurrent.wind))
                                      : qsTr("Wind —")
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                            }
                            Label {
                                Layout.fillWidth: true
                                text: controller.primaryUpdatedAt.length
                                      ? qsTr("Updated %1").arg(controller.primaryUpdatedAt) : ""
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 11
                            }
                        }
                    }

                    Shared.WeatherSolarSection {
                        Layout.fillWidth: true
                        visible: controller.primaryForecast.count > 0
                        solarInfo: controller.primarySolar
                        dayLabel: controller.primarySolar.date
                                   ? root.formatDate(controller.primarySolar.date) : ""
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        visible: controller.primaryForecast.count > 0
                        columns: width >= 900 ? 6 : width >= 580 ? 3 : 2
                        rowSpacing: 8
                        columnSpacing: 8
                        Repeater {
                            model: controller.primaryForecast
                            delegate: Button {
                                required property string date
                                required property string icon
                                required property string condition
                                required property double maximum
                                required property double minimum
                                required property double precipitationProbability
                                required property int index
                                Layout.fillWidth: true
                                Layout.preferredHeight: 146
                                padding: 0
                                Accessible.role: Accessible.Button
                                Accessible.name: qsTr("Select forecast for %1").arg(root.formatDate(date))
                                onClicked: root.controller.selectPrimaryForecastDate(date)
                                background: Rectangle {
                                    radius: ThemeModule.Theme.panelRadius
                                    color: date === root.controller.primarySelectedDate
                                           ? ThemeModule.Theme.selectionBlue : ThemeModule.Theme.raisedSurface
                                    border.color: date === root.controller.primarySelectedDate || parent.activeFocus
                                                  ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.borderColor
                                }
                                contentItem: ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 4
                                    Label { text: root.formatDate(date); color: ThemeModule.Theme.textPrimary; font.bold: true; font.pixelSize: 12 }
                                    Label { text: icon; color: ThemeModule.Theme.textPrimary; font.pixelSize: 22 }
                                    Label { text: condition; color: ThemeModule.Theme.textSecondary; font.pixelSize: ThemeModule.Theme.metadataFontSize; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                    Label { text: qsTr("%1° / %2°").arg(Math.round(maximum)).arg(Math.round(minimum)); color: ThemeModule.Theme.textPrimary; font.pixelSize: 12 }
                                    Label { text: qsTr("%1% rain").arg(Math.round(precipitationProbability)); color: ThemeModule.Theme.textSecondary; font.pixelSize: ThemeModule.Theme.metadataFontSize }
                                }
                            }
                        }
                    }
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                padding: 18
                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    Label {
                        text: qsTr("Search another city")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: qsTr("This forecast is temporary and does not replace your saved location.")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Shared.AppTextField {
                            id: cityField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Enter a city name")
                            accessibleLabel: qsTr("City name")
                            Keys.onReturnPressed: controller.searchCity(text)
                        }
                        Shared.AppButton {
                            text: controller.citySearching ? qsTr("Searching…") : qsTr("Search")
                            enabled: !controller.citySearching
                            Accessible.name: qsTr("Search city weather")
                            onClicked: controller.searchCity(cityField.text)
                        }
                    }
                    BusyIndicator {
                        visible: controller.citySearching
                        running: visible
                        width: 28
                        height: 28
                    }
                    Shared.StatePanel {
                        Layout.fillWidth: true
                        visible: controller.cityForecast.count === 0 && !controller.citySearching
                        height: visible ? implicitHeight : 0
                        state: controller.cityError.length ? "error" : "empty"
                        title: controller.cityError.length ? qsTr("City forecast unavailable") : qsTr("Search for a city")
                        message: controller.cityError.length ? controller.cityError : qsTr("City results appear here and remain separate from your saved location.")
                    }
                    Label {
                        text: controller.cityLocation
                        visible: controller.cityForecast.count > 0
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    RowLayout {
                        visible: controller.cityForecast.count > 0
                        Label { text: controller.cityCurrent.icon || "🌡️"; color: ThemeModule.Theme.textPrimary; font.pixelSize: 28 }
                        Label {
                            text: root.temperature(controller.cityCurrent.temperature,
                                controller.cityCurrent.hasTemperature === true)
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 27
                            font.weight: Font.Bold
                        }
                        Label { text: controller.cityCurrent.condition || qsTr("Weather unavailable"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 12 }
                    }
                    Shared.WeatherSolarSection {
                        Layout.fillWidth: true
                        visible: controller.cityForecast.count > 0
                        solarInfo: controller.citySolar
                        dayLabel: controller.citySolar.date
                                   ? root.formatDate(controller.citySolar.date) : ""
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        visible: controller.cityForecast.count > 0
                        columns: width >= 760 ? 6 : 3
                        rowSpacing: 6
                        columnSpacing: 6
                        Repeater {
                            model: controller.cityForecast
                            delegate: Button {
                                required property string date
                                required property string icon
                                required property string condition
                                required property double maximum
                                required property double minimum
                                required property int index
                                Layout.fillWidth: true
                                Layout.preferredHeight: 92
                                padding: 0
                                Accessible.role: Accessible.Button
                                Accessible.name: qsTr("Select forecast for %1").arg(root.formatDate(date))
                                onClicked: root.controller.selectCityForecastDate(date)
                                background: Rectangle {
                                    radius: ThemeModule.Theme.panelRadius
                                    color: date === root.controller.citySelectedDate
                                           ? ThemeModule.Theme.selectionBlue : ThemeModule.Theme.raisedSurface
                                    border.color: date === root.controller.citySelectedDate || parent.activeFocus
                                                  ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.borderColor
                                }
                                contentItem: Column {
                                    anchors.centerIn: parent
                                    spacing: 3
                                    Label { anchors.horizontalCenter: parent.horizontalCenter; text: root.formatDate(date); color: ThemeModule.Theme.textPrimary; font.pixelSize: 11 }
                                    Label { anchors.horizontalCenter: parent.horizontalCenter; text: icon; color: ThemeModule.Theme.textPrimary; font.pixelSize: 17 }
                                    Label { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("%1° / %2°").arg(Math.round(maximum)).arg(Math.round(minimum)); color: ThemeModule.Theme.textSecondary; font.pixelSize: ThemeModule.Theme.metadataFontSize }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Timer {
        interval: 60 * 1000
        repeat: true
        running: root.visible
                 && (controller.primaryForecast.count > 0 || controller.cityForecast.count > 0)
        onTriggered: controller.updateSolarInfo()
    }
}
