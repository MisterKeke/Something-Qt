import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Dialog {
    id: root

    property var historyController
    property int selectedPointIndex: -1
    readonly property var game: historyController ? historyController.historyGame : ({})
    readonly property var statistics: historyController ? historyController.historyStatistics : ({})
    readonly property var historyPoints: historyController ? historyController.historyPoints : []
    readonly property var historyMarketChoices: {
        const markets = historyController ? historyController.historyMarkets : []
        const choices = []
        for (let i = 0; i < markets.length; ++i) {
            const market = markets[i]
            const currency = market.currencyCode || qsTr("currency unavailable")
            choices.push({
                countryCode: market.countryCode,
                currencyCode: market.currencyCode,
                label: qsTr("%1 (%2) · %3")
                       .arg(countryName(market.countryCode))
                       .arg(market.countryCode)
                       .arg(currency)
            })
        }
        return choices
    }
    readonly property int rangePriceCount: {
        let count = 0
        const points = historyPoints || []
        for (let i = 0; i < points.length; ++i) {
            if (points[i].hasPrice) ++count
        }
        return count
    }

    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(920, parent ? Math.max(320, parent.width - 28) : 880)
    height: Math.min(840, parent ? Math.max(320, parent.height - 28) : 760)
    padding: 18
    title: qsTr("Price History")
    Accessible.name: title
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function countryName(code) {
        const choices = historyController ? historyController.countries : []
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].code === code)
                return choices[i].name
        }
        return code || qsTr("Unknown country")
    }

    function selectedMarketIndex() {
        if (!historyController) return -1
        const markets = historyController.historyMarkets
        for (let i = 0; i < markets.length; ++i) {
            if (markets[i].countryCode === historyController.historyCountryCode
                    && markets[i].currencyCode === historyController.historyCurrencyCode)
                return i
        }
        return -1
    }

    function syncMarketSelection() {
        if (!historyController) {
            historyMarketCombo.currentIndex = -1
            return
        }
        historyMarketCombo.currentIndex = selectedMarketIndex()
    }

    function selectedMarketMatchesCurrentGame() {
        if (!historyController) return false
        const gameCountry = String(game.priceCountryCode || historyController.countryCode || "")
        const gameCurrency = String(game.currency || "")
        return historyController.historyCountryCode === gameCountry
                && historyController.historyCurrencyCode === gameCurrency
    }

    function formatTimestamp(value) {
        const date = new Date(String(value || ""))
        return isNaN(date.getTime()) ? (value || qsTr("Not recorded"))
                                     : date.toLocaleString()
    }

    function pointDescription(point) {
        if (!point) return ""
        if (point.hasPrice) return point.priceDisplay || point.priceState
        if (point.priceState === "unavailable") return qsTr("Temporarily unavailable")
        if (point.priceState === "unsupported") return qsTr("No supported pricing")
        return point.priceState || qsTr("No price recorded")
    }

    function currentPriceText() {
        if (Number(statistics.observationCount || 0) > 0) {
            if (statistics.latestObservationState === "free") return qsTr("Free")
            if (statistics.latestObservationState === "priced")
                return statistics.latestPriceDisplay || qsTr("Price unavailable")
            if (statistics.latestObservationState === "unavailable")
                return qsTr("Temporarily unavailable")
            if (statistics.latestObservationState === "unsupported")
                return qsTr("No supported pricing")
            return qsTr("No price recorded")
        }
        if (!selectedMarketMatchesCurrentGame())
            return qsTr("No price recorded for this market")
        if (game.priceStatus === "priced" || game.priceStatus === "free")
            return game.priceDisplay || (game.priceStatus === "free" ? qsTr("Free") : qsTr("Price unavailable"))
        if (game.priceStatus === "unavailable")
            return qsTr("Temporarily unavailable")
        if (game.priceStatus === "unsupported")
            return qsTr("No supported pricing")
        return qsTr("No price recorded")
    }

    function pointAt(index) {
        if (index < 0 || index >= historyPoints.length)
            return null
        return historyPoints[index]
    }

    function selectNewestPricePoint() {
        for (let i = historyPoints.length - 1; i >= 0; --i) {
            if (historyPoints[i].hasPrice) {
                selectedPointIndex = i
                return
            }
        }
        selectedPointIndex = -1
    }

    onHistoryPointsChanged: Qt.callLater(selectNewestPricePoint)
    onSelectedPointIndexChanged: chart.requestPaint()
    onClosed: if (historyController) historyController.closePriceHistory()

    Connections {
        target: root.historyController
        function onStateChanged() { root.syncMarketSelection() }
    }

    background: Rectangle {
        color: ThemeModule.Theme.raisedSurface
        border.color: ThemeModule.Theme.accentCyan
        border.width: 1
        radius: ThemeModule.Theme.panelRadius
    }

    contentItem: ScrollView {
        id: historyScroll
        clip: true

        ColumnLayout {
            width: historyScroll.availableWidth
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                Item {
                    Layout.preferredWidth: 132
                    Layout.preferredHeight: 74
                    Rectangle {
                        anchors.fill: parent
                        radius: ThemeModule.Theme.controlRadius
                        color: ThemeModule.Theme.surface
                        border.color: ThemeModule.Theme.borderColor
                    }
                    Image {
                        id: historyArtwork
                        anchors.fill: parent
                        anchors.margins: 1
                        source: root.game.artworkSource || ""
                        sourceSize: Qt.size(384, 216)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: false
                        visible: status === Image.Ready
                        Accessible.name: qsTr("Artwork for %1").arg(root.game.name || "")
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: !root.game.artworkSource || historyArtwork.status !== Image.Ready
                        text: "◉"
                        color: ThemeModule.Theme.accentCyan
                        font.pixelSize: 24
                    }
                    Accessible.ignored: true
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        text: root.game.name || qsTr("Steam game")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 19
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Chart market: %1 (%2) · %3")
                              .arg(root.countryName(root.historyController
                                  ? root.historyController.historyCountryCode : root.game.priceCountryCode))
                              .arg(root.historyController
                                  ? (root.historyController.historyCountryCode || "—")
                                  : (root.game.priceCountryCode || "—"))
                              .arg(root.historyController
                                  ? (root.historyController.historyCurrencyCode || qsTr("currency unavailable"))
                                  : (root.game.currency || qsTr("currency unavailable")))
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Historical country and currency")
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 12
                }
                Shared.AppComboBox {
                    id: historyMarketCombo
                    Layout.fillWidth: true
                    model: root.historyMarketChoices
                    textRole: "label"
                    displayText: currentIndex < 0
                                 ? qsTr("No recorded history for the selected market") : currentText
                    accessibleLabel: qsTr("Select a country and currency with recorded price history")
                    enabled: root.historyController && root.historyMarketChoices.length > 0
                    Component.onCompleted: root.syncMarketSelection()
                    onActivated: function(index) {
                        if (!root.historyController || index < 0
                                || index >= root.historyController.historyMarkets.length)
                            return
                        const market = root.historyController.historyMarkets[index]
                        root.historyController.setPriceHistoryMarket(
                            market.countryCode, market.currencyCode)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.historyController
                             && root.historyController.historyMarkets.length > 0
                             && root.selectedMarketIndex() < 0
                    text: qsTr("This market has no saved observations. Choose a market with history from the list.")
                    color: ThemeModule.Theme.warning
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: 7
                Shared.FilterChip {
                    text: qsTr("7 days")
                    selected: root.historyController && root.historyController.historyRange === "7d"
                    onClicked: if (root.historyController) root.historyController.setPriceHistoryRange("7d")
                }
                Shared.FilterChip {
                    text: qsTr("30 days")
                    selected: root.historyController && root.historyController.historyRange === "30d"
                    onClicked: if (root.historyController) root.historyController.setPriceHistoryRange("30d")
                }
                Shared.FilterChip {
                    text: qsTr("90 days")
                    selected: root.historyController && root.historyController.historyRange === "90d"
                    onClicked: if (root.historyController) root.historyController.setPriceHistoryRange("90d")
                }
                Shared.FilterChip {
                    text: qsTr("1 year")
                    selected: root.historyController && root.historyController.historyRange === "1y"
                    onClicked: if (root.historyController) root.historyController.setPriceHistoryRange("1y")
                }
                Shared.FilterChip {
                    text: qsTr("All history")
                    selected: root.historyController && root.historyController.historyRange === "all"
                    onClicked: if (root.historyController) root.historyController.setPriceHistoryRange("all")
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                BusyIndicator {
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    visible: root.historyController && root.historyController.historyLoading
                    running: visible
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.historyController && root.historyController.historyError.length > 0
                    text: root.historyController ? root.historyController.historyError : ""
                    color: ThemeModule.Theme.error
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
                Shared.AppButton {
                    text: qsTr("Retry")
                    quiet: true
                    visible: root.historyController && root.historyController.historyError.length > 0
                    enabled: !root.historyController.historyLoading
                    onClicked: root.historyController.reloadPriceHistory()
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: root.width < 620 ? 1 : 3
                columnSpacing: 8
                rowSpacing: 8

                Repeater {
                    model: [
                        { label: qsTr("Current recorded price"), value: root.currentPriceText() },
                        { label: qsTr("Lowest recorded by Something-Qt"), value: root.statistics.lowestPriceDisplay || "—" },
                        { label: qsTr("Highest observed"), value: root.statistics.highestPriceDisplay || "—" },
                        { label: qsTr("Recorded data range"), value: root.statistics.observationCount > 0
                                  ? (root.formatTimestamp(root.statistics.firstObservationAtUtc) + " → "
                                     + root.formatTimestamp(root.statistics.mostRecentObservationAtUtc))
                                  : qsTr("No observations") },
                        { label: root.selectedMarketMatchesCurrentGame()
                                  ? qsTr("Last successful refresh")
                                  : qsTr("Most recent recorded observation"),
                          value: root.selectedMarketMatchesCurrentGame()
                                  ? root.formatTimestamp(root.game.lastCheckedAt)
                                  : root.formatTimestamp(root.statistics.mostRecentObservationAtUtc) },
                        { label: qsTr("Recorded observations"), value: String(root.statistics.observationCount || 0) }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 78
                        radius: ThemeModule.Theme.controlRadius
                        color: ThemeModule.Theme.surface
                        border.color: ThemeModule.Theme.borderColor

                        Column {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 5
                            Label {
                                width: parent.width
                                text: modelData.label
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                            Label {
                                width: parent.width
                                text: modelData.value
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }

            Shared.StatePanel {
                Layout.fillWidth: true
                visible: root.historyController && !root.historyController.historyLoading
                         && root.historyController.historyError.length === 0
                         && Number(root.statistics.observationCount || 0) === 0
                state: "empty"
                title: root.historyMarketChoices.length > 0
                       ? qsTr("No history for the selected market")
                       : qsTr("No price history recorded yet")
                message: root.historyMarketChoices.length > 0
                         ? qsTr("No observations are recorded for %1 (%2) · %3. Choose a market with saved observations above.")
                               .arg(root.countryName(root.historyController.historyCountryCode))
                               .arg(root.historyController.historyCountryCode || "—")
                               .arg(root.historyController.historyCurrencyCode
                                    || qsTr("currency unavailable"))
                         : qsTr("Earlier prices are not reconstructed. New observations appear after successful Steam checks.")
            }

            Shared.StatePanel {
                Layout.fillWidth: true
                visible: root.historyController && !root.historyController.historyLoading
                         && Number(root.statistics.observationCount || 0) > 0
                         && root.historyPoints.length === 0
                state: "empty"
                title: qsTr("No observations in this range")
                message: qsTr("Choose a longer range to see the recorded Steam checks.")
            }

            Shared.StatePanel {
                Layout.fillWidth: true
                visible: root.historyController && !root.historyController.historyLoading
                         && Number(root.statistics.observationCount || 0) > 0
                         && Number(root.statistics.comparablePriceCount || 0) > 0
                         && !root.statistics.hasSufficientHistory
                state: "empty"
                title: qsTr("More history is needed")
                message: Number(root.statistics.comparablePriceCount || 0) === 1
                         ? qsTr("There is one recorded price. A second comparable observation is needed to calculate a price change.")
                         : qsTr("There are not yet two comparable recorded prices for this market.")
            }

            Shared.StatePanel {
                Layout.fillWidth: true
                visible: root.historyController && !root.historyController.historyLoading
                         && Number(root.statistics.observationCount || 0) > 0
                         && Number(root.statistics.comparablePriceCount || 0) === 0
                state: "empty"
                title: qsTr("No numeric prices in this market")
                message: qsTr("Recorded checks show a price state, but Steam has not supplied a comparable price value.")
            }

            Shared.StatePanel {
                Layout.fillWidth: true
                visible: root.historyController && !root.historyController.historyLoading
                         && root.historyPoints.length > 0
                         && Number(root.statistics.comparablePriceCount || 0) > 0
                         && root.rangePriceCount === 0
                state: "empty"
                title: qsTr("No numeric prices in this range")
                message: qsTr("This range contains price states without a numeric value. The chart marks those checks and leaves gaps in the price line.")
            }

            Label {
                Layout.fillWidth: true
                visible: root.statistics.hasComparableInterval
                text: qsTr("Change in selected range: %1 · %2")
                      .arg(root.statistics.intervalAbsoluteChangeDisplay || "—")
                      .arg(root.statistics.intervalPercentageChangeDisplay || qsTr("percentage unavailable"))
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Canvas {
                id: chart
                Layout.fillWidth: true
                Layout.preferredHeight: 285
                visible: root.historyPoints.length > 0
                property double minTime: 0
                property double maxTime: 1
                property double minValue: 0
                property double maxValue: 1
                readonly property real plotLeft: Math.min(132, Math.max(100, width * 0.34))
                readonly property real plotRight: Math.max(plotLeft + 20, width - 12)
                readonly property real plotTop: 14
                readonly property real plotBottom: height - 38
                readonly property double maxLineGapMilliseconds: 36 * 60 * 60 * 1000
                Accessible.name: qsTr("Recorded Steam prices by date")
                Accessible.description: qsTr("Price values are plotted by UTC date. State-only checks appear at the bottom of the chart and break the price line.")

                function updateDomain() {
                    const points = root.historyPoints || []
                    let firstTime = Infinity
                    let lastTime = -Infinity
                    let lowest = Infinity
                    let highest = -Infinity
                    for (let i = 0; i < points.length; ++i) {
                        const point = points[i]
                        const time = Number(point.observedAtUtcMs)
                        if (time < firstTime) firstTime = time
                        if (time > lastTime) lastTime = time
                        if (!point.hasPrice) continue
                        const amount = Number(point.priceMinor)
                        if (amount < lowest) lowest = amount
                        if (amount > highest) highest = amount
                    }
                    if (!isFinite(firstTime) || !isFinite(lastTime)) {
                        minTime = 0
                        maxTime = 1
                    } else if (firstTime === lastTime) {
                        minTime = firstTime - 12 * 60 * 60 * 1000
                        maxTime = lastTime + 12 * 60 * 60 * 1000
                    } else {
                        minTime = firstTime
                        maxTime = lastTime
                    }
                    if (!isFinite(lowest) || !isFinite(highest)) {
                        minValue = 0
                        maxValue = 1
                    } else if (lowest === highest) {
                        const padding = Math.max(1, Math.abs(lowest) * 0.05)
                        minValue = lowest - padding
                        maxValue = highest + padding
                    } else {
                        const padding = (highest - lowest) * 0.08
                        minValue = lowest - padding
                        maxValue = highest + padding
                    }
                }

                function pointPosition(point) {
                    const xRatio = (Number(point.observedAtUtcMs) - minTime) / (maxTime - minTime)
                    const yRatio = point.hasPrice
                                  ? (Number(point.priceMinor) - minValue) / (maxValue - minValue)
                                  : 0
                    return {
                        x: plotLeft + xRatio * (plotRight - plotLeft),
                        y: point.hasPrice
                           ? plotBottom - yRatio * (plotBottom - plotTop)
                           : plotBottom
                    }
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onVisibleChanged: if (visible) { updateDomain(); requestPaint() }
                Connections {
                    target: root.historyController
                    function onStateChanged() {
                        chart.updateDomain()
                        chart.requestPaint()
                    }
                }

                onPaint: {
                    const context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    const left = plotLeft
                    const right = plotRight
                    const top = plotTop
                    const bottom = plotBottom
                    const currency = String(root.statistics.currencyCode || "")
                    context.font = "11px " + ThemeModule.Theme.fontFamily
                    context.lineWidth = 1
                    context.strokeStyle = ThemeModule.Theme.borderColor
                    context.fillStyle = ThemeModule.Theme.textSecondary
                    context.textAlign = "right"
                    context.textBaseline = "middle"
                    for (let tick = 0; tick <= 4; ++tick) {
                        const ratio = tick / 4
                        const y = bottom - ratio * (bottom - top)
                        const value = minValue + ratio * (maxValue - minValue)
                        context.beginPath()
                        context.moveTo(left, y)
                        context.lineTo(right, y)
                        context.stroke()
                        const label = currency.length > 0 && root.historyController
                                      ? root.historyController.formatPrice(Math.round(value), currency)
                                      : String(Math.round(value))
                        context.fillText(label, left - 8, y)
                    }

                    context.textBaseline = "top"
                    context.textAlign = "left"
                    context.fillText(new Date(minTime).toISOString().slice(0, 10), left, bottom + 8)
                    context.textAlign = "right"
                    context.fillText(new Date(maxTime).toISOString().slice(0, 10), right, bottom + 8)
                    if (width > 560) {
                        context.textAlign = "center"
                        context.fillText(new Date((minTime + maxTime) / 2).toISOString().slice(0, 10),
                                         (left + right) / 2, bottom + 8)
                    }
                    context.textAlign = "left"
                    context.fillText(qsTr("Date (UTC)"), left, bottom + 24)

                    context.strokeStyle = ThemeModule.Theme.accentCyan
                    context.fillStyle = ThemeModule.Theme.accentCyan
                    context.lineWidth = 2
                    context.lineCap = "round"
                    context.lineJoin = "round"
                    let previous = null
                    const points = root.historyPoints || []
                    for (let i = 0; i < points.length; ++i) {
                        const point = points[i]
                        if (!point.hasPrice) {
                            previous = null
                            const statePosition = chart.pointPosition(point)
                            context.fillStyle = ThemeModule.Theme.warning
                            context.beginPath()
                            context.arc(statePosition.x, statePosition.y, 3, 0, Math.PI * 2)
                            context.fill()
                            if (i === root.selectedPointIndex) {
                                context.strokeStyle = ThemeModule.Theme.warning
                                context.beginPath()
                                context.arc(statePosition.x, statePosition.y, 7, 0, Math.PI * 2)
                                context.stroke()
                            }
                            context.strokeStyle = ThemeModule.Theme.accentCyan
                            context.fillStyle = ThemeModule.Theme.accentCyan
                            continue
                        }
                        const position = chart.pointPosition(point)
                        if (previous
                                && Number(point.observedAtUtcMs) - Number(previous.observedAtUtcMs)
                                   <= chart.maxLineGapMilliseconds) {
                            const previousPosition = chart.pointPosition(previous)
                            context.beginPath()
                            context.moveTo(previousPosition.x, previousPosition.y)
                            context.lineTo(position.x, position.y)
                            context.stroke()
                        }
                        context.beginPath()
                        context.arc(position.x, position.y, 3.5, 0, Math.PI * 2)
                        context.fill()
                        if (i === root.selectedPointIndex) {
                            context.beginPath()
                            context.arc(position.x, position.y, 7, 0, Math.PI * 2)
                            context.stroke()
                        }
                        previous = point
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    function selectNearest(x, y) {
                        let bestIndex = -1
                        let bestDistance = 22
                        const points = root.historyPoints || []
                        for (let i = 0; i < points.length; ++i) {
                            const position = chart.pointPosition(points[i])
                            const dx = position.x - x
                            const dy = position.y - y
                            const distance = Math.sqrt(dx * dx + dy * dy)
                            if (distance < bestDistance) {
                                bestDistance = distance
                                bestIndex = i
                            }
                        }
                        if (bestIndex >= 0) root.selectedPointIndex = bestIndex
                    }
                    onClicked: function(mouse) { selectNearest(mouse.x, mouse.y) }
                    onPositionChanged: function(mouse) { if (containsMouse) selectNearest(mouse.x, mouse.y) }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                radius: ThemeModule.Theme.controlRadius
                color: ThemeModule.Theme.surface
                border.color: ThemeModule.Theme.borderColor
                visible: chart.visible && root.pointAt(root.selectedPointIndex) !== null
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 12
                    Label {
                        text: {
                            const point = root.pointAt(root.selectedPointIndex)
                            return point ? root.formatTimestamp(point.observedAtUtc) : ""
                        }
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Label {
                        Layout.fillWidth: true
                        text: {
                            const point = root.pointAt(root.selectedPointIndex)
                            return root.pointDescription(point)
                        }
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                }
            }

        }
    }

    footer: DialogButtonBox {
        alignment: Qt.AlignRight
        Shared.AppButton {
            text: qsTr("Close")
            quiet: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
