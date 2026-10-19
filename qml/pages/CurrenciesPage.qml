import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root
    property string activeMode: "single"
    property double amountValue: 100
    implicitHeight: pageColumn.implicitHeight

    function captureViewState() {
        return {
            scrollY: pageScroll.contentY,
            activeMode: activeMode,
            singleBase: singleBase.text,
            singleQuote: singleQuote.text,
            allBase: allBase.text,
            allFilter: allFilter.text,
            conversionAmount: conversionAmount.text,
            conversionBase: conversionBase.text,
            conversionQuote: conversionQuote.text
        }
    }
    function restoreViewState(state) {
        if (!state) return
        if (state.activeMode !== undefined) activeMode = String(state.activeMode)
        if (state.singleBase !== undefined) singleBase.text = String(state.singleBase)
        if (state.singleQuote !== undefined) singleQuote.text = String(state.singleQuote)
        if (state.allBase !== undefined) allBase.text = String(state.allBase)
        if (state.allFilter !== undefined) allFilter.text = String(state.allFilter)
        if (state.conversionAmount !== undefined) conversionAmount.text = String(state.conversionAmount)
        if (state.conversionBase !== undefined) conversionBase.text = String(state.conversionBase)
        if (state.conversionQuote !== undefined) conversionQuote.text = String(state.conversionQuote)
        if (state.scrollY !== undefined) pageScroll.contentY = Number(state.scrollY)
    }

    Connections {
        target: currencyController
        function onFavoriteSelected(baseCode, quoteCode) {
            singleBase.text = baseCode
            singleQuote.text = quoteCode
            allBase.text = baseCode
            allFilter.text = ""
            activeMode = "single"
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
            spacing: 14

            Shared.AppCard {
                Layout.fillWidth: true
                padding: 18
                ColumnLayout {
                    width: parent.width
                    spacing: 13
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Currency exchange rates")
                            color: ThemeModule.Theme.text
                            font.family: ThemeModule.Theme.displayFont
                            font.pixelSize: 20
                            font.weight: Font.DemiBold
                        }
                        Shared.StatusBadge { text: qsTr("ECB data"); badgeColor: ThemeModule.Theme.cyan }
                    }
                    Label {
                        text: qsTr("Live rates from Frankfurter. Saved pairs are local; exchange rates are refreshed and are not stored.")
                        color: ThemeModule.Theme.textDim
                        font.family: ThemeModule.Theme.bodyFont
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: [
                                { id: "single", label: qsTr("Single rate") },
                                { id: "all", label: qsTr("All rates") },
                                { id: "conversion", label: qsTr("Conversion") }
                            ]
                            delegate: Shared.AppButton {
                                required property var modelData
                                text: modelData.label
                                quiet: root.activeMode !== modelData.id
                                Accessible.name: modelData.label
                                onClicked: root.activeMode = modelData.id
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.activeMode === "single"
                        spacing: 10
                        RowLayout {
                            Layout.fillWidth: true
                            Shared.AppTextField {
                                id: singleBase
                                Layout.fillWidth: true
                                placeholderText: "USD"
                                text: "USD"
                                maximumLength: 3
                                accessibleLabel: qsTr("From currency code")
                            }
                            Shared.AppButton {
                                text: "⇄"
                                Accessible.name: qsTr("Swap currencies")
                                onClicked: {
                                    const previous = singleBase.text
                                    singleBase.text = singleQuote.text
                                    singleQuote.text = previous
                                }
                            }
                            Shared.AppTextField {
                                id: singleQuote
                                Layout.fillWidth: true
                                placeholderText: "EUR"
                                text: "EUR"
                                maximumLength: 3
                                accessibleLabel: qsTr("To currency code")
                            }
                        }
                        RowLayout {
                            Shared.AppButton {
                                text: currencyController.singleLoading ? qsTr("Loading…") : qsTr("Get rate")
                                enabled: !currencyController.singleLoading
                                onClicked: currencyController.lookupSingle(singleBase.text, singleQuote.text)
                            }
                            Shared.AppButton {
                                text: qsTr("Add to favorites")
                                enabled: !currencyController.favoritesBusy
                                onClicked: currencyController.addFavorite(singleBase.text, singleQuote.text)
                            }
                        }
                        BusyIndicator {
                            visible: currencyController.singleLoading
                            running: visible
                            width: 28
                            height: 28
                        }
                        Shared.StatePanel {
                            Layout.fillWidth: true
                            visible: currencyController.singleError.length > 0
                            height: visible ? implicitHeight : 0
                            state: "error"
                            title: qsTr("Rate unavailable")
                            message: currencyController.singleError
                        }
                        Shared.AppCard {
                            Layout.fillWidth: true
                            visible: Object.keys(currencyController.singleResult).length > 0
                            height: visible ? 95 : 0
                            Label {
                                visible: currencyController.singleResult.found === true
                                text: qsTr("1 %1 = %2 %3")
                                    .arg(currencyController.singleResult.baseCode || "")
                                    .arg(Number(currencyController.singleResult.rate).toFixed(4))
                                    .arg(currencyController.singleResult.quoteCode || "")
                                color: ThemeModule.Theme.text
                                font.family: ThemeModule.Theme.displayFont
                                font.pixelSize: 19
                                font.weight: Font.DemiBold
                            }
                            Label {
                                visible: currencyController.singleResult.found !== true
                                text: qsTr("No rate found for %1 to %2.")
                                    .arg(currencyController.singleResult.baseCode || "")
                                    .arg(currencyController.singleResult.quoteCode || "")
                                color: ThemeModule.Theme.warning
                                font.family: ThemeModule.Theme.displayFont
                                font.pixelSize: 15
                            }
                            Label {
                                text: currencyController.singleResult.identity === true
                                      ? qsTr("Same currency")
                                      : currencyController.singleResult.date
                                        ? qsTr("Rate date: %1").arg(currencyController.singleResult.date) : ""
                                visible: text.length > 0
                                color: ThemeModule.Theme.textDim
                                font.family: ThemeModule.Theme.bodyFont
                                font.pixelSize: 11
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.activeMode === "all"
                        spacing: 10
                        RowLayout {
                            Layout.fillWidth: true
                            Shared.AppTextField {
                                id: allBase
                                Layout.fillWidth: true
                                placeholderText: "USD"
                                text: "USD"
                                maximumLength: 3
                                accessibleLabel: qsTr("Base currency code")
                            }
                            Shared.AppButton {
                                text: currencyController.allLoading ? qsTr("Loading…") : qsTr("Get all rates")
                                enabled: !currencyController.allLoading
                                onClicked: currencyController.loadAllRates(allBase.text)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: currencyController.allBase.length > 0 && currencyController.allError.length === 0
                            text: qsTr("%1 rates for %2%3")
                                .arg(currencyController.allRates.totalCount)
                                .arg(currencyController.allBase)
                                .arg(currencyController.allDate.length ? qsTr(" on %1").arg(currencyController.allDate) : "")
                            color: ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
                            font.pixelSize: 11
                        }
                        Shared.AppTextField {
                            id: allFilter
                            Layout.fillWidth: true
                            visible: currencyController.allBase.length > 0
                            placeholderText: qsTr("Filter currency code…")
                            accessibleLabel: qsTr("Filter all currency codes")
                            onTextChanged: currencyController.setAllRatesFilter(text)
                        }
                        BusyIndicator {
                            visible: currencyController.allLoading
                            running: visible
                            width: 28
                            height: 28
                        }
                        Shared.StatePanel {
                            Layout.fillWidth: true
                            visible: currencyController.allError.length > 0
                            height: visible ? implicitHeight : 0
                            state: "error"
                            title: qsTr("Rates unavailable")
                            message: currencyController.allError
                        }
                        Shared.StatePanel {
                            Layout.fillWidth: true
                            visible: currencyController.allBase.length > 0
                                     && !currencyController.allLoading
                                     && currencyController.allError.length === 0
                                     && currencyController.allRates.count === 0
                            height: visible ? implicitHeight : 0
                            state: "empty"
                            title: qsTr("No matching rates")
                            message: qsTr("No currency codes match this filter.")
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            visible: currencyController.allRates.count > 0
                            columns: width >= 760 ? 3 : width >= 500 ? 2 : 1
                            rowSpacing: 7
                            columnSpacing: 7
                            Repeater {
                                model: currencyController.allRates
                                delegate: Rectangle {
                                    required property string code
                                    required property double rate
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 44
                                    radius: 10
                                    color: ThemeModule.Theme.panelAlt
                                    border.color: ThemeModule.Theme.border
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 12
                                        Label { text: code; color: ThemeModule.Theme.text; font.bold: true; Layout.fillWidth: true }
                                        Label { text: Number(rate).toFixed(4); color: ThemeModule.Theme.cyan; font.family: ThemeModule.Theme.bodyFont }
                                    }
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.activeMode === "conversion"
                        spacing: 10
                        RowLayout {
                            Layout.fillWidth: true
                            Shared.AppTextField {
                                id: conversionAmount
                                Layout.fillWidth: true
                                placeholderText: "100"
                                text: "100"
                                accessibleLabel: qsTr("Amount to convert")
                            }
                            Shared.AppTextField {
                                id: conversionBase
                                Layout.fillWidth: true
                                placeholderText: "EUR"
                                text: "EUR"
                                maximumLength: 3
                                accessibleLabel: qsTr("Conversion source currency code")
                            }
                            Shared.AppButton {
                                text: "⇄"
                                Accessible.name: qsTr("Swap conversion currencies")
                                onClicked: {
                                    const previous = conversionBase.text
                                    conversionBase.text = conversionQuote.text
                                    conversionQuote.text = previous
                                }
                            }
                            Shared.AppTextField {
                                id: conversionQuote
                                Layout.fillWidth: true
                                placeholderText: "TRY"
                                text: "TRY"
                                maximumLength: 3
                                accessibleLabel: qsTr("Conversion target currency code")
                            }
                        }
                        Shared.AppButton {
                            text: currencyController.conversionLoading ? qsTr("Converting…") : qsTr("Convert")
                            enabled: !currencyController.conversionLoading
                            onClicked: currencyController.convert(Number(conversionAmount.text), conversionBase.text, conversionQuote.text)
                        }
                        BusyIndicator {
                            visible: currencyController.conversionLoading
                            running: visible
                            width: 28
                            height: 28
                        }
                        Shared.StatePanel {
                            Layout.fillWidth: true
                            visible: currencyController.conversionError.length > 0
                            height: visible ? implicitHeight : 0
                            state: "error"
                            title: qsTr("Conversion unavailable")
                            message: currencyController.conversionError
                        }
                        Shared.AppCard {
                            Layout.fillWidth: true
                            visible: Object.keys(currencyController.conversionResult).length > 0
                            height: visible ? 96 : 0
                            Label {
                                visible: currencyController.conversionResult.found === true
                                text: qsTr("%1 %2 = %3 %4")
                                    .arg(Number(currencyController.conversionResult.amount || 0).toString())
                                    .arg(currencyController.conversionResult.baseCode || "")
                                    .arg(Number(currencyController.conversionResult.converted).toFixed(4))
                                    .arg(currencyController.conversionResult.quoteCode || "")
                                color: ThemeModule.Theme.text
                                font.family: ThemeModule.Theme.displayFont
                                font.pixelSize: 18
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }
                            Label {
                                visible: currencyController.conversionResult.found !== true
                                text: qsTr("No rate found for %1 to %2.")
                                    .arg(currencyController.conversionResult.baseCode || "")
                                    .arg(currencyController.conversionResult.quoteCode || "")
                                color: ThemeModule.Theme.warning
                                font.pixelSize: 14
                            }
                            Label {
                                text: currencyController.conversionResult.identity === true
                                      ? qsTr("Same currency")
                                      : currencyController.conversionResult.date
                                        ? qsTr("Rate date: %1").arg(currencyController.conversionResult.date) : ""
                                visible: text.length > 0
                                color: ThemeModule.Theme.textDim
                                font.pixelSize: 11
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
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Favorite pairs")
                            color: ThemeModule.Theme.text
                            font.family: ThemeModule.Theme.displayFont
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                        }
                        Shared.AppButton {
                            text: currencyController.favoritesBusy ? qsTr("Refreshing…") : qsTr("Refresh rates")
                            enabled: !currencyController.favoritesBusy
                            onClicked: currencyController.refreshFavorites()
                        }
                    }
                    Label {
                        visible: currencyController.favoritesError.length > 0
                        text: currencyController.favoritesError
                        color: ThemeModule.Theme.error
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Shared.StatePanel {
                        Layout.fillWidth: true
                        visible: currencyController.favoritesLoading
                        height: visible ? implicitHeight : 0
                        state: "loading"
                        title: qsTr("Loading saved pairs")
                        message: qsTr("Reading locally saved favorite definitions.")
                    }
                    Shared.StatePanel {
                        Layout.fillWidth: true
                        visible: !currencyController.favoritesLoading
                                 && currencyController.favorites.count === 0
                                 && currencyController.favoritesError.length === 0
                        height: visible ? implicitHeight : 0
                        state: "empty"
                        title: qsTr("No favorite pairs")
                        message: qsTr("Save a pair from Single rate. Definitions remain available offline.")
                    }
                    Repeater {
                        model: currencyController.favorites
                        delegate: Rectangle {
                            required property string baseCode
                            required property string quoteCode
                            required property string rateStatus
                            required property bool found
                            required property double rate
                            required property string date
                            required property string rateError
                            Layout.fillWidth: true
                            Layout.preferredHeight: 62
                            radius: 10
                            color: ThemeModule.Theme.panelAlt
                            border.color: ThemeModule.Theme.border
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                spacing: 8
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Shared.AppButton {
                                        text: qsTr("%1 to %2").arg(baseCode).arg(quoteCode)
                                        Accessible.name: qsTr("Select %1 to %2").arg(baseCode).arg(quoteCode)
                                        quiet: true
                                        onClicked: currencyController.selectFavorite(baseCode, quoteCode)
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: found ? qsTr("1 %1 = %2 %3 · %4")
                                                .arg(baseCode).arg(Number(rate).toFixed(4)).arg(quoteCode)
                                                .arg(date.length ? date : qsTr("date unavailable"))
                                              : rateStatus === "loading" ? qsTr("Fetching latest rate…")
                                              : rateStatus === "missing" ? qsTr("No rate found")
                                              : rateStatus === "error" ? rateError
                                              : qsTr("Rate not loaded")
                                        color: rateStatus === "error" ? ThemeModule.Theme.warning : ThemeModule.Theme.textDim
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                                Shared.RemoveFavoriteButton {
                                    pairName: qsTr("%1 to %2").arg(baseCode).arg(quoteCode)
                                    enabled: !currencyController.favoritesBusy
                                    onClicked: currencyController.removeFavorite(baseCode, quoteCode)
                                }
                            }
                        }
                    }
                    Label {
                        visible: currencyController.notification.length > 0
                        text: currencyController.notification
                        color: ThemeModule.Theme.success
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}
