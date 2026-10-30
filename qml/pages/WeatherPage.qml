import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root
    property var controller: weatherController
    property string cityQuery: ""
    property bool showComparisonTable: false
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
    function celsius(value, available) {
        return available ? Math.round(Number(value)) + "°C" : "—"
    }
    function windSpeed(value, available) {
        return available ? Math.round(Number(value)) + " km/h" : "—"
    }
    function selectedForecastDay(forecast, date) {
        const days = forecast && forecast.days ? forecast.days : []
        for (let index = 0; index < days.length; ++index) {
            if (days[index].date === date) return days[index]
        }
        return null
    }
    function comparisonMetricLabel(key) {
        if (key === "temperature") return qsTr("Current temperature")
        if (key === "feelsLike") return qsTr("Feels like")
        if (key === "maximum") return qsTr("Maximum temperature")
        if (key === "minimum") return qsTr("Minimum temperature")
        if (key === "wind") return qsTr("Wind speed")
        if (key === "precipitation") return qsTr("Precipitation probability")
        if (key === "condition") return qsTr("Current condition")
        return key
    }
    function comparisonCardDateText(hasSelectedDay, hasForecast, loading, date) {
        if (hasSelectedDay) return qsTr("Forecast for %1").arg(formatDate(date))
        if (!hasForecast && loading) {
            return date.length > 0
                ? qsTr("Loading forecast for %1").arg(formatDate(date))
                : qsTr("Loading forecast…")
        }
        return date.length > 0
            ? qsTr("Forecast unavailable for %1").arg(formatDate(date))
            : qsTr("Waiting for forecast date")
    }
    function comparisonTableDateText(dateAvailable, loading, hasForecast, date) {
        if (dateAvailable) return qsTr("Forecast · %1").arg(formatDate(date))
        if (date.length > 0 && loading && !hasForecast)
            return qsTr("Loading · %1").arg(formatDate(date))
        return date.length > 0
            ? qsTr("Selected date unavailable · %1").arg(formatDate(date))
            : qsTr("Forecast date unavailable")
    }
    Component.onCompleted: controller.updateSolarInfo()
    onVisibleChanged: if (visible) controller.updateSolarInfo()

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
                    Label {
                        Layout.fillWidth: true
                        visible: controller.cityCandidates.length > 1
                        text: qsTr("Choose the location that matches your search.")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: controller.cityCandidates
                        delegate: Shared.AppCard {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            visible: controller.cityCandidates.length > 1
                            padding: 10
                            RowLayout {
                                width: parent.width
                                spacing: 10
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        color: ThemeModule.Theme.textPrimary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: [modelData.administrativeArea, modelData.country]
                                            .filter(function(part) { return String(part).length > 0 })
                                            .join(", ")
                                        color: ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("%1, %2").arg(Number(modelData.latitude).toFixed(4))
                                            .arg(Number(modelData.longitude).toFixed(4))
                                        color: ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 10
                                    }
                                }
                                Shared.AppButton {
                                    text: qsTr("Choose")
                                    Accessible.name: qsTr("Choose %1 at %2, %3")
                                        .arg(modelData.name)
                                        .arg(Number(modelData.latitude).toFixed(4))
                                        .arg(Number(modelData.longitude).toFixed(4))
                                    onClicked: controller.selectCityCandidate(index)
                                }
                            }
                        }
                    }
                    Shared.StatePanel {
                        Layout.fillWidth: true
                        visible: controller.cityForecast.count === 0 && !controller.citySearching
                                 && controller.cityCandidates.length === 0
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
                    RowLayout {
                        Layout.fillWidth: true
                        visible: controller.cityForecast.count > 0
                        Shared.AppButton {
                            text: controller.comparisonMutationPending
                                  ? qsTr("Saving comparison…")
                                  : controller.comparisonStorageLoading
                                    ? qsTr("Loading saved cities…")
                                    : controller.searchedCityAlreadyAdded
                                      ? qsTr("Already in comparison")
                                      : controller.comparisonCityCount >= 4
                                        ? (controller.canReplaceSearchedCity
                                           ? qsTr("Choose a city below to replace")
                                           : qsTr("Comparison list full"))
                                        : qsTr("Add to comparison")
                            enabled: controller.canAddSearchedCity
                            Accessible.name: qsTr("Add %1 to city comparison").arg(controller.cityLocation)
                            onClicked: controller.addSearchedCityToComparison()
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: controller.cityUpdatedAt.length > 0
                            text: qsTr("Updated %1").arg(controller.cityUpdatedAt)
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
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

            Shared.AppCard {
                Layout.fillWidth: true
                visible: controller.comparisonCityCount > 0
                         || controller.comparisonStorageError.length > 0
                padding: 18
                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Comparison cities · %1 of 4").arg(controller.comparisonCityCount)
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                        Shared.AppButton {
                            text: qsTr("Refresh all")
                            quiet: true
                            enabled: controller.comparisonCityCount > 0
                            onClicked: controller.refreshAllComparisonCities()
                        }
                        Shared.AppButton {
                            text: root.showComparisonTable
                                  ? qsTr("Hide table") : qsTr("Detailed comparison")
                            quiet: true
                            enabled: controller.comparisonCityCount > 0
                            onClicked: root.showComparisonTable = !root.showComparisonTable
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: controller.comparisonCityCount < 2
                              ? qsTr("Add one more city to compare forecasts.")
                              : qsTr("Saved comparison cities stay separate from your primary location.")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: controller.comparisonStorageLoading
                        text: qsTr("Restoring saved cities and cached forecasts…")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: controller.comparisonMutationPending
                        text: qsTr("Saving comparison changes…")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: controller.comparisonStorageError.length > 0
                        text: controller.comparisonStorageError
                        color: ThemeModule.Theme.warning
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: controller.comparisonDateOptions.length > 0
                        spacing: 5
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Shared provider-local forecast date")
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("All cities use this calendar date. Cities without it show unavailable.")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 5
                            Repeater {
                                model: controller.comparisonDateOptions
                                delegate: Button {
                                    required property var modelData
                                    readonly property string dayDate: String(modelData.date || "")
                                    text: qsTr("%1 · %2/%3")
                                        .arg(root.formatDate(dayDate))
                                        .arg(modelData.availableCount)
                                        .arg(modelData.cityCount)
                                    checkable: true
                                    checked: dayDate === controller.comparisonSelectedDate
                                    implicitWidth: Math.max(112, dateLabel.implicitWidth + 18)
                                    implicitHeight: 34
                                    padding: 5
                                    Accessible.name: qsTr("Compare forecast date %1, available for %2 of %3 cities")
                                        .arg(root.formatDate(dayDate))
                                        .arg(modelData.availableCount)
                                        .arg(modelData.cityCount)
                                    onClicked: controller.selectComparisonForecastDate(dayDate)
                                    contentItem: Text {
                                        id: dateLabel
                                        text: parent.text
                                        color: parent.checked ? ThemeModule.Theme.textPrimary
                                                             : ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 10
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                    background: Rectangle {
                                        radius: ThemeModule.Theme.controlRadius
                                        color: parent.checked ? ThemeModule.Theme.selectionBlue
                                                              : ThemeModule.Theme.raisedSurface
                                        border.color: parent.checked ? ThemeModule.Theme.accentCyan
                                                                    : ThemeModule.Theme.borderColor
                                    }
                                }
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: controller.comparisonCityCount > 0
                                 && controller.comparisonDateOptions.length === 0
                        text: qsTr("Waiting for forecast dates from selected cities…")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 11
                    }
                    GridLayout {
                        id: comparisonGrid
                        Layout.fillWidth: true
                        columns: width >= 900 ? 2 : 1
                        rowSpacing: 12
                        columnSpacing: 12

                        Repeater {
                            model: controller.comparisonCities
                            delegate: Shared.AppCard {
                                required property string identity
                                required property string name
                                required property string administrativeArea
                                required property string country
                                required property string timezone
                                required property string localDateTime
                                required property string lastSuccessfulRefresh
                                required property bool loading
                                required property string error
                                required property bool hasForecast
                                required property string selectedForecastDate
                                required property var forecast
                                required property int index

                                readonly property var selectedDay:
                                    root.selectedForecastDay(forecast, selectedForecastDate)

                                Layout.fillWidth: true
                                Layout.minimumWidth: comparisonGrid.columns === 2 ? 360 : 0
                                padding: 14

                                ColumnLayout {
                                    width: parent.width
                                    spacing: 9

                                    Label {
                                        Layout.fillWidth: true
                                        text: [name, administrativeArea, country]
                                            .filter(function(part) { return String(part).length > 0 })
                                            .join(", ")
                                        color: ThemeModule.Theme.textPrimary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 16
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: localDateTime.length > 0
                                              ? qsTr("Local time · %1 (%2)").arg(localDateTime).arg(timezone)
                                              : timezone.length > 0
                                                ? qsTr("Local time unavailable · %1").arg(timezone)
                                                : qsTr("Local time unavailable")
                                        color: ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Current")
                                        color: ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 10
                                        Label {
                                            text: forecast.current && forecast.current.icon
                                                  ? forecast.current.icon : "🌡️"
                                            color: ThemeModule.Theme.textPrimary
                                            font.pixelSize: 34
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    text: root.celsius(
                                                        forecast.current ? forecast.current.temperature : 0,
                                                        forecast.current && forecast.current.hasTemperature === true)
                                                    color: ThemeModule.Theme.textPrimary
                                                    font.family: ThemeModule.Theme.fontFamily
                                                    font.pixelSize: 29
                                                    font.weight: Font.Bold
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: forecast.current && forecast.current.condition
                                                          ? forecast.current.condition
                                                          : qsTr("Condition unavailable")
                                                    color: ThemeModule.Theme.textSecondary
                                                    font.family: ThemeModule.Theme.fontFamily
                                                    font.pixelSize: 12
                                                    wrapMode: Text.Wrap
                                                }
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Feels like %1 · Wind %2")
                                                    .arg(root.celsius(
                                                        forecast.current ? forecast.current.feelsLike : 0,
                                                        forecast.current && forecast.current.hasFeelsLike === true))
                                                    .arg(root.windSpeed(
                                                        forecast.current ? forecast.current.wind : 0,
                                                        forecast.current && forecast.current.hasWind === true))
                                                color: ThemeModule.Theme.textSecondary
                                                font.family: ThemeModule.Theme.fontFamily
                                                font.pixelSize: 11
                                                wrapMode: Text.Wrap
                                            }
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.comparisonCardDateText(
                                            selectedDay !== null, hasForecast, loading,
                                            controller.comparisonSelectedDate)
                                        color: ThemeModule.Theme.textPrimary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                    }
                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: 3
                                        columnSpacing: 8
                                        visible: controller.comparisonSelectedDate.length > 0
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label {
                                                text: qsTr("High")
                                                color: ThemeModule.Theme.textSecondary
                                                font.pixelSize: 10
                                            }
                                            Label {
                                                text: selectedDay
                                                      ? root.celsius(selectedDay.maximum,
                                                          selectedDay.maximum !== undefined
                                                          && selectedDay.maximum !== null)
                                                      : "—"
                                                color: ThemeModule.Theme.textPrimary
                                                font.family: ThemeModule.Theme.fontFamily
                                                font.pixelSize: 14
                                                font.weight: Font.DemiBold
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label {
                                                text: qsTr("Low")
                                                color: ThemeModule.Theme.textSecondary
                                                font.pixelSize: 10
                                            }
                                            Label {
                                                text: selectedDay
                                                      ? root.celsius(selectedDay.minimum,
                                                          selectedDay.minimum !== undefined
                                                          && selectedDay.minimum !== null)
                                                      : "—"
                                                color: ThemeModule.Theme.textPrimary
                                                font.family: ThemeModule.Theme.fontFamily
                                                font.pixelSize: 14
                                                font.weight: Font.DemiBold
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label {
                                                text: qsTr("Precipitation")
                                                color: ThemeModule.Theme.textSecondary
                                                font.pixelSize: 10
                                            }
                                            Label {
                                                text: selectedDay
                                                      && selectedDay.precipitationProbability !== undefined
                                                      && selectedDay.precipitationProbability !== null
                                                      ? qsTr("%1%").arg(Math.round(selectedDay.precipitationProbability))
                                                      : "—"
                                                color: ThemeModule.Theme.textPrimary
                                                font.family: ThemeModule.Theme.fontFamily
                                                font.pixelSize: 14
                                                font.weight: Font.DemiBold
                                            }
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: loading || error.length > 0
                                        text: error.length > 0 ? error
                                              : hasForecast ? qsTr("Refreshing · showing saved forecast")
                                              : qsTr("Loading forecast…")
                                        color: error.length > 0 ? ThemeModule.Theme.warning
                                              : ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: lastSuccessfulRefresh.length > 0
                                        text: qsTr("Last successful update %1")
                                            .arg(new Date(lastSuccessfulRefresh).toLocaleString())
                                        color: ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 10
                                        wrapMode: Text.Wrap
                                    }

                                    Shared.AppButton {
                                        Layout.fillWidth: true
                                        visible: controller.canReplaceSearchedCity
                                        quiet: true
                                        text: qsTr("Replace with %1").arg(controller.cityLocation)
                                        Accessible.name: qsTr("Replace %1 with %2")
                                            .arg(name).arg(controller.cityLocation)
                                        enabled: !controller.comparisonMutationPending
                                        onClicked: controller.replaceComparisonCity(identity)
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Shared.AppButton {
                                            Layout.fillWidth: true
                                            quiet: true
                                            text: loading ? qsTr("Cancel refresh") : qsTr("Refresh")
                                            Accessible.name: loading
                                                ? qsTr("Cancel refresh for %1").arg(name)
                                                : qsTr("Refresh forecast for %1").arg(name)
                                            onClicked: loading
                                                ? controller.cancelComparisonCityRefresh(identity)
                                                : controller.refreshComparisonCity(identity)
                                        }
                                        Shared.AppButton {
                                            text: qsTr("↑")
                                            quiet: true
                                            implicitWidth: 40
                                            Layout.preferredWidth: 40
                                            enabled: index > 0 && !controller.comparisonMutationPending
                                            Accessible.name: qsTr("Move %1 earlier").arg(name)
                                            onClicked: controller.moveComparisonCity(identity, -1)
                                        }
                                        Shared.AppButton {
                                            text: qsTr("↓")
                                            quiet: true
                                            implicitWidth: 40
                                            Layout.preferredWidth: 40
                                            enabled: index < controller.comparisonCityCount - 1
                                                     && !controller.comparisonMutationPending
                                            Accessible.name: qsTr("Move %1 later").arg(name)
                                            onClicked: controller.moveComparisonCity(identity, 1)
                                        }
                                        Shared.AppButton {
                                            text: qsTr("×")
                                            quiet: true
                                            implicitWidth: 40
                                            Layout.preferredWidth: 40
                                            Accessible.name: qsTr("Remove %1 from comparison").arg(name)
                                            enabled: !controller.comparisonMutationPending
                                            onClicked: controller.removeComparisonCity(identity)
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.showComparisonTable && controller.comparisonCityCount > 0
                        spacing: 7
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Daily rows use the shared provider-local calendar date; unavailable cities are marked.")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Flickable {
                            id: comparisonTableFlick
                            property int metricColumnWidth: 154
                            property int minimumCityColumnWidth: 176
                            Layout.fillWidth: true
                            Layout.preferredHeight: comparisonTableColumn.implicitHeight + 12
                            implicitHeight: comparisonTableColumn.implicitHeight + 12
                            clip: true
                            contentWidth: Math.max(width,
                                metricColumnWidth + controller.comparisonCityCount * minimumCityColumnWidth)
                            contentHeight: comparisonTableColumn.implicitHeight
                            flickableDirection: Flickable.HorizontalFlick
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.horizontal: ThemeModule.AppScrollBar {
                                policy: ScrollBar.AsNeeded
                            }

                            ColumnLayout {
                                id: comparisonTableColumn
                                x: 0
                                y: 0
                                width: comparisonTableFlick.contentWidth
                                spacing: 0

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Rectangle {
                                        Layout.preferredWidth: comparisonTableFlick.metricColumnWidth
                                        Layout.minimumWidth: comparisonTableFlick.metricColumnWidth
                                        Layout.preferredHeight: 88
                                        color: ThemeModule.Theme.selectionBlue
                                        border.color: ThemeModule.Theme.borderColor
                                        Label {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            text: qsTr("Metric")
                                            color: ThemeModule.Theme.textPrimary
                                            font.family: ThemeModule.Theme.fontFamily
                                            font.pixelSize: 12
                                            font.weight: Font.DemiBold
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                    Repeater {
                                        model: controller.comparisonTableHeaders
                                        delegate: Rectangle {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: comparisonTableFlick.minimumCityColumnWidth
                                            Layout.preferredWidth: (comparisonTableFlick.contentWidth
                                                - comparisonTableFlick.metricColumnWidth)
                                                / Math.max(1, controller.comparisonCityCount)
                                            Layout.preferredHeight: 88
                                            color: ThemeModule.Theme.selectionBlue
                                            border.color: ThemeModule.Theme.borderColor
                                            Accessible.name: modelData.name
                                            ColumnLayout {
                                                anchors.fill: parent
                                                anchors.margins: 7
                                                spacing: 3
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: modelData.name
                                                    color: ThemeModule.Theme.textPrimary
                                                    font.family: ThemeModule.Theme.fontFamily
                                                    font.pixelSize: 11
                                                    font.weight: Font.DemiBold
                                                    wrapMode: Text.Wrap
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: root.comparisonTableDateText(
                                                        modelData.dateAvailable, modelData.loading,
                                                        modelData.hasForecast, modelData.date)
                                                    color: ThemeModule.Theme.textSecondary
                                                    font.family: ThemeModule.Theme.fontFamily
                                                    font.pixelSize: 10
                                                    wrapMode: Text.Wrap
                                                }
                                            }
                                        }
                                    }
                                }

                                Repeater {
                                    model: controller.comparisonTableRows
                                    delegate: RowLayout {
                                        id: metricRow
                                        required property var modelData
                                        required property int index
                                        readonly property string metricKey: modelData.key
                                        Layout.fillWidth: true
                                        spacing: 0

                                        Rectangle {
                                            Layout.preferredWidth: comparisonTableFlick.metricColumnWidth
                                            Layout.minimumWidth: comparisonTableFlick.metricColumnWidth
                                            Layout.preferredHeight: 48
                                            color: ThemeModule.Theme.insetSurface
                                            border.color: ThemeModule.Theme.borderColor
                                            Label {
                                                anchors.fill: parent
                                                anchors.margins: 8
                                                text: root.comparisonMetricLabel(metricRow.metricKey)
                                                color: ThemeModule.Theme.textSecondary
                                                font.family: ThemeModule.Theme.fontFamily
                                                font.pixelSize: 11
                                                wrapMode: Text.Wrap
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                        }
                                        Repeater {
                                            model: modelData.values
                                            delegate: Rectangle {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: comparisonTableFlick.minimumCityColumnWidth
                                                Layout.preferredWidth: (comparisonTableFlick.contentWidth
                                                    - comparisonTableFlick.metricColumnWidth)
                                                    / Math.max(1, controller.comparisonCityCount)
                                                Layout.preferredHeight: 48
                                                color: metricRow.index % 2 === 0
                                                       ? ThemeModule.Theme.raisedSurface
                                                       : ThemeModule.Theme.insetSurface
                                                border.color: ThemeModule.Theme.borderColor
                                                Label {
                                                    anchors.fill: parent
                                                    anchors.margins: 8
                                                    text: modelData.text
                                                    color: modelData.text === "—"
                                                          ? ThemeModule.Theme.textDisabled
                                                          : ThemeModule.Theme.textPrimary
                                                    font.family: ThemeModule.Theme.fontFamily
                                                    font.pixelSize: 11
                                                    wrapMode: Text.Wrap
                                                    verticalAlignment: Text.AlignVCenter
                                                    Accessible.name: qsTr("%1 for %2")
                                                        .arg(root.comparisonMetricLabel(metricRow.metricKey))
                                                        .arg(modelData.city)
                                                }
                                            }
                                        }
                                    }
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
                 && (controller.primaryForecast.count > 0 || controller.cityForecast.count > 0
                     || controller.comparisonCityCount > 0)
        onTriggered: controller.updateSolarInfo()
    }
}
