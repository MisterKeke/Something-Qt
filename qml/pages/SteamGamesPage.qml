import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root

    property var controller
    property var savedViewState: ({})
    property string pendingAddUrl: ""
    implicitHeight: content.implicitHeight + 8
    Accessible.name: qsTr("Steam game tracking")

    function captureViewState() {
        return { storeUrl: storeUrlField.text, listPosition: gamesList.contentY }
    }

    function restoreViewState(state) {
        savedViewState = state || {}
        storeUrlField.text = String(savedViewState.storeUrl || "")
        Qt.callLater(function() {
            gamesList.contentY = Math.max(0, Number(root.savedViewState.listPosition || 0))
        })
    }

    function addGame() {
        if (!controller || storeUrlField.text.trim().length === 0)
            return
        pendingAddUrl = storeUrlField.text
        controller.addGame(storeUrlField.text)
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 14

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 18

            Label {
                width: parent.width
                text: qsTr("Track Steam Store prices")
                color: ThemeModule.Theme.text
                font.family: ThemeModule.Theme.displayFont
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            Label {
                width: parent.width
                text: qsTr("Prices are stored locally in the selected country’s currency. Cached results stay visible if Steam is unavailable.")
                color: ThemeModule.Theme.textDim
                font.family: ThemeModule.Theme.bodyFont
                font.pixelSize: 13
                wrapMode: Text.Wrap
            }

            RowLayout {
                width: parent.width
                spacing: 10

                Label {
                    text: qsTr("Store country")
                    color: ThemeModule.Theme.textDim
                    Accessible.name: qsTr("Steam Store country")
                }
                Shared.AppComboBox {
                    id: countryBox
                    Layout.preferredWidth: 220
                    model: root.controller ? root.controller.countries : []
                    textRole: "label"
                    valueRole: "code"
                    accessibleLabel: qsTr("Country used for Steam price checks")
                    enabled: root.controller && root.controller.snapshotLoaded
                    onActivated: function(index) {
                        if (root.controller)
                            root.controller.setSelectedCountry(currentValue)
                    }
                    Component.onCompleted: {
                        if (!root.controller)
                            return
                        for (let i = 0; i < count; ++i) {
                            if (valueAt(i) === root.controller.selectedCountry) {
                                currentIndex = i
                                break
                            }
                        }
                    }
                    Connections {
                        target: root.controller
                        function onSettingsChanged() {
                            for (let i = 0; i < countryBox.count; ++i) {
                                if (countryBox.valueAt(i) === root.controller.selectedCountry) {
                                    countryBox.currentIndex = i
                                    break
                                }
                            }
                        }
                    }
                }
                Shared.AppButton {
                    text: qsTr("Save country")
                    quiet: true
                    enabled: root.controller && !root.controller.countryBusy
                             && !root.controller.refreshBusy
                             && root.controller.selectedCountry !== root.controller.countryCode
                    onClicked: root.controller.saveCountry()
                }
                Item { Layout.fillWidth: true }
            }

            Label {
                width: parent.width
                visible: root.controller && root.controller.snapshotLoaded
                text: !root.controller ? ""
                      : root.controller.selectedCountry !== root.controller.countryCode
                        ? qsTr("Save to use %1 for future checks. Saving does not refresh prices.").arg(countryBox.currentText)
                        : root.controller.stalePriceCount > 0
                          ? qsTr("%1 cached %2 from another country. Refresh prices to update them.")
                                .arg(root.controller.stalePriceCount)
                                .arg(root.controller.stalePriceCount === 1 ? qsTr("price is") : qsTr("prices are"))
                          : qsTr("New checks use %1. Changing this setting never refreshes automatically.").arg(countryBox.currentText)
                color: root.controller && root.controller.selectedCountry !== root.controller.countryCode
                       ? ThemeModule.Theme.warning
                       : root.controller && root.controller.stalePriceCount > 0
                         ? ThemeModule.Theme.warning : ThemeModule.Theme.textDim
                font.family: ThemeModule.Theme.bodyFont
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            RowLayout {
                width: parent.width
                spacing: 10
                Shared.AppTextField {
                    id: storeUrlField
                    Layout.fillWidth: true
                    placeholderText: qsTr("https://store.steampowered.com/app/123456/")
                    accessibleLabel: qsTr("Steam Store game URL")
                    inputMethodHints: Qt.ImhUrlCharactersOnly
                    onAccepted: root.addGame()
                }
                Shared.AppButton {
                    text: root.controller && root.controller.addBusy ? qsTr("Adding…") : qsTr("Add game")
                    enabled: root.controller && root.controller.snapshotLoaded
                             && !root.controller.addBusy && !root.controller.loading
                    onClicked: root.addGame()
                }
            }
            Label {
                width: parent.width
                visible: root.controller && root.controller.addError.length > 0
                text: root.controller ? root.controller.addError : ""
                color: ThemeModule.Theme.error
                font.family: ThemeModule.Theme.bodyFont
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Shared.AppButton {
                text: root.controller && root.controller.refreshBusy
                      ? qsTr("Refreshing…") : qsTr("Refresh prices")
                enabled: root.controller && root.controller.snapshotLoaded
                         && !root.controller.refreshBusy && !root.controller.countryBusy
                onClicked: root.controller.refreshPrices()
            }
            Shared.StatusBadge {
                visible: root.controller && root.controller.refreshBusy
                text: root.controller ? root.controller.refreshStatus : ""
                badgeColor: ThemeModule.Theme.cyan
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller && root.controller.refreshStatus.length > 0
                         && !root.controller.refreshBusy
                text: root.controller ? root.controller.refreshStatus : ""
                color: ThemeModule.Theme.textDim
                font.family: ThemeModule.Theme.bodyFont
                font.pixelSize: 13
                wrapMode: Text.Wrap
            }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.errorMessage.length > 0
            state: "error"
            title: qsTr("Steam tracking needs attention")
            message: root.controller ? root.controller.errorMessage : ""
        }

        Shared.AppButton {
            Layout.alignment: Qt.AlignLeft
            visible: root.controller && !root.controller.snapshotLoaded
                     && !root.controller.loading && root.controller.errorMessage.length > 0
            text: qsTr("Retry loading saved games")
            quiet: true
            onClicked: root.controller.retryLoad()
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.loading && root.controller.games.count === 0
            state: "loading"
            title: qsTr("Loading saved games")
            message: qsTr("Reading the local Steam price cache…")
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && !root.controller.loading
                     && root.controller.snapshotLoaded && root.controller.games.count === 0
            state: "empty"
            title: qsTr("No tracked games yet")
            message: qsTr("Add a Steam Store game URL to begin tracking its price.")
        }

        ListView {
            id: gamesList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(680, Math.max(0, contentHeight))
            visible: root.controller && root.controller.games.count > 0
            model: root.controller ? root.controller.games : null
            clip: true
            spacing: 10
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Tracked Steam games")

            delegate: Item {
                id: delegateRoot
                required property var gameId
                required property var steamAppId
                required property string name
                required property string artworkSource
                required property string priceStatus
                required property string priceStateLabel
                required property string currency
                required property string priceDisplay
                required property string regularPriceDisplay
                required property int discountPercent
                required property string priceCountryCode
                required property string lastCheckedAt
                required property string lastAttemptedAt
                required property string lastRefreshError
                width: ListView.view.width
                height: gameCard.implicitHeight

                Shared.AppCard {
                    id: gameCard
                    width: parent.width
                    padding: 12

                    RowLayout {
                        width: parent.width
                        spacing: 16

                        Item {
                            Layout.preferredWidth: 190
                            Layout.preferredHeight: 112
                            Layout.alignment: Qt.AlignTop
                            Rectangle {
                                anchors.fill: parent
                                radius: 9
                                color: ThemeModule.Theme.panelAlt
                                border.color: ThemeModule.Theme.border
                            }
                            Image {
                                id: artwork
                                anchors.fill: parent
                                anchors.margins: 1
                                source: delegateRoot.artworkSource
                                sourceSize: Qt.size(384, 216)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: false
                                visible: status === Image.Ready
                                Accessible.name: qsTr("Artwork for %1").arg(delegateRoot.name)
                            }
                            Column {
                                anchors.centerIn: parent
                                spacing: 4
                                visible: delegateRoot.artworkSource.length === 0
                                         || artwork.status === Image.Error
                                         || artwork.status === Image.Null
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "◉"
                                    color: ThemeModule.Theme.accent
                                    font.pixelSize: 27
                                }
                                Label {
                                    text: qsTr("Artwork unavailable")
                                    color: ThemeModule.Theme.textDim
                                    font.pixelSize: 11
                                }
                            }
                            Accessible.ignored: true
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 7

                            Label {
                                Layout.fillWidth: true
                                text: delegateRoot.name
                                color: ThemeModule.Theme.text
                                font.family: ThemeModule.Theme.displayFont
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                Accessible.name: delegateRoot.name
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Shared.StatusBadge {
                                    text: delegateRoot.priceStateLabel
                                    badgeColor: delegateRoot.priceStatus === "priced"
                                                ? ThemeModule.Theme.success
                                                : delegateRoot.priceStatus === "free"
                                                  ? ThemeModule.Theme.cyan
                                                  : ThemeModule.Theme.warning
                                }
                                Label {
                                    text: delegateRoot.priceDisplay
                                    visible: delegateRoot.priceDisplay.length > 0
                                    color: ThemeModule.Theme.text
                                    font.family: ThemeModule.Theme.displayFont
                                    font.pixelSize: 16
                                    font.weight: Font.Bold
                                }
                                Label {
                                    text: delegateRoot.regularPriceDisplay
                                    visible: delegateRoot.discountPercent > 0
                                    color: ThemeModule.Theme.textDim
                                    font.family: ThemeModule.Theme.bodyFont
                                    font.pixelSize: 12
                                    font.strikeout: true
                                }
                                Shared.StatusBadge {
                                    visible: delegateRoot.discountPercent > 0
                                    text: qsTr("−%1%").arg(delegateRoot.discountPercent)
                                    badgeColor: ThemeModule.Theme.pink
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: delegateRoot.priceCountryCode.length > 0
                                      ? qsTr("Price country: %1").arg(delegateRoot.priceCountryCode)
                                      : qsTr("No successful price check yet")
                                color: delegateRoot.priceCountryCode.length > 0
                                       && root.controller
                                       && delegateRoot.priceCountryCode !== root.controller.countryCode
                                       ? ThemeModule.Theme.warning : ThemeModule.Theme.textDim
                                font.family: ThemeModule.Theme.bodyFont
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: delegateRoot.priceCountryCode.length > 0
                                         && root.controller
                                         && delegateRoot.priceCountryCode !== root.controller.countryCode
                                text: qsTr("Cached price is from another country; refresh to update it.")
                                color: ThemeModule.Theme.warning
                                font.family: ThemeModule.Theme.bodyFont
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: delegateRoot.lastCheckedAt.length > 0
                                text: qsTr("Last successful check: %1").arg(delegateRoot.lastCheckedAt)
                                color: ThemeModule.Theme.textDim
                                font.family: ThemeModule.Theme.bodyFont
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: delegateRoot.lastRefreshError.length > 0
                                text: delegateRoot.lastAttemptedAt.length > 0
                                      ? qsTr("Last attempt: %1 · %2").arg(delegateRoot.lastAttemptedAt, delegateRoot.lastRefreshError)
                                      : delegateRoot.lastRefreshError
                                color: ThemeModule.Theme.error
                                font.family: ThemeModule.Theme.bodyFont
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                Accessible.role: Accessible.AlertMessage
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Shared.AppButton {
                                    text: qsTr("Open Store")
                                    quiet: true
                                    Accessible.name: qsTr("Open %1 in Steam Store").arg(delegateRoot.name)
                                    onClicked: root.controller.openStore(delegateRoot.gameId)
                                }
                                Shared.AppButton {
                                    text: qsTr("Remove")
                                    destructive: true
                                    enabled: root.controller && !root.controller.deleteBusy
                                             && !root.controller.refreshBusy
                                    Accessible.name: qsTr("Remove %1 from tracked games").arg(delegateRoot.name)
                                    onClicked: {
                                        removeDialog.gameId = delegateRoot.gameId
                                        removeDialog.gameName = delegateRoot.name
                                        removeDialog.open()
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }
        }

        Shared.NotificationToast {
            Layout.alignment: Qt.AlignHCenter
            message: root.controller ? root.controller.notification : ""
            onDismissed: if (root.controller) root.controller.clearNotification()
        }
    }

    Dialogs.ConfirmationDialog {
        id: removeDialog
        property var gameId: 0
        property string gameName: ""
        title: qsTr("Remove tracked game?")
        message: qsTr("Remove “%1” from Steam tracking? Its local artwork will also be removed.").arg(gameName)
        confirmText: qsTr("Remove game")
        destructive: true
        onConfirmed: if (root.controller) root.controller.deleteGame(gameId)
    }

    Connections {
        target: root.controller
        function onStateChanged() {
            if (root.pendingAddUrl.length > 0
                    && root.controller.notification.startsWith(qsTr("Added "))) {
                if (storeUrlField.text === root.pendingAddUrl)
                    storeUrlField.clear()
                root.pendingAddUrl = ""
            }
            if (root.controller.addError.length > 0)
                root.pendingAddUrl = ""
        }
    }
}
