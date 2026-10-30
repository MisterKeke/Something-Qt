import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root

    property var controller
    property string pendingAddUrl: ""

    implicitHeight: content.implicitHeight + 8
    Accessible.name: qsTr("Game Updates")

    function addGame() {
        if (!controller || addUrlField.text.trim().length === 0)
            return
        pendingAddUrl = addUrlField.text
        controller.addGame(addUrlField.text)
    }

    function formattedTimestamp(value) {
        if (!value || String(value).length === 0)
            return qsTr("Not checked yet")
        const parsed = new Date(String(value))
        if (isNaN(parsed.getTime()))
            return String(value)
        return Qt.formatDateTime(parsed, "yyyy-MM-dd HH:mm")
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 14

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 18

            ColumnLayout {
                width: parent.width
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Follow game updates independently")
                    textFormat: Text.PlainText
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 19
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Add any Steam game to keep its announcements and news here. This list is separate from price tracking. Owned is a local label; purchases are not checked.")
                    textFormat: Text.PlainText
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Shared.AppTextField {
                        id: addUrlField
                        Layout.fillWidth: true
                        placeholderText: qsTr("https://store.steampowered.com/app/123456/")
                        accessibleLabel: qsTr("Steam Store URL for a game to follow")
                        inputMethodHints: Qt.ImhUrlCharactersOnly
                        enabled: root.controller && root.controller.snapshotLoaded
                                 && !root.controller.addBusy && !root.controller.refreshBusy
                                 && !root.controller.removeBusy
                        onAccepted: root.addGame()
                    }
                    Shared.AppButton {
                        text: root.controller && root.controller.addBusy
                              ? qsTr("Adding…") : qsTr("Follow game")
                        enabled: root.controller && root.controller.snapshotLoaded
                                 && !root.controller.addBusy && !root.controller.refreshBusy
                                 && !root.controller.removeBusy
                        onClicked: root.addGame()
                    }
                    Shared.AppButton {
                        text: root.controller && root.controller.refreshBusy
                              ? qsTr("Refreshing…") : qsTr("Refresh all")
                        quiet: true
                        enabled: root.controller && root.controller.snapshotLoaded
                                 && root.controller.games.length > 0
                                 && !root.controller.refreshBusy && !root.controller.addBusy
                                 && !root.controller.removeBusy
                        onClicked: root.controller.refreshAll()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.controller && root.controller.addError.length > 0
                    text: root.controller ? root.controller.addError : ""
                    textFormat: Text.PlainText
                    color: ThemeModule.Theme.error
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.controller && root.controller.refreshBusy
                    Label {
                        Layout.fillWidth: true
                        text: root.controller ? root.controller.refreshStatus : ""
                        textFormat: Text.PlainText
                        color: ThemeModule.Theme.accentCyan
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Shared.AppButton {
                        text: qsTr("Cancel")
                        quiet: true
                        enabled: root.controller && root.controller.refreshBusy
                        onClicked: root.controller.cancelRefresh()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 690
            spacing: 14

            Shared.AppCard {
                Layout.preferredWidth: 315
                Layout.fillHeight: true
                padding: 14

                ColumnLayout {
                    width: parent.width
                    height: parent.height
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Followed games")
                            textFormat: Text.PlainText
                            color: ThemeModule.Theme.textPrimary
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                        Shared.StatusBadge {
                            text: String(root.controller ? root.controller.games.length : 0)
                            badgeColor: ThemeModule.Theme.accentCyan
                        }
                    }
                    Shared.FilterChip {
                        Layout.fillWidth: true
                        text: qsTr("All games · %1 unread")
                            .arg(root.controller ? root.controller.totalUnreadCount : 0)
                        selected: root.controller && root.controller.selectedAppId === 0
                        onClicked: if (root.controller) root.controller.setSelectedAppId(0)
                    }

                    ListView {
                        id: gamesList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 8
                        boundsBehavior: Flickable.StopAtBounds
                        model: root.controller ? root.controller.games : []
                        Accessible.name: qsTr("Games followed for news")

                        delegate: Rectangle {
                            id: gameDelegate
                            required property var modelData
                            readonly property var game: modelData
                            width: ListView.view.width
                            height: gameCard.implicitHeight
                            radius: ThemeModule.Theme.panelRadius
                            color: Number(game.appId) === root.controller.selectedAppId
                                   ? Qt.alpha(ThemeModule.Theme.accentCyan, 0.13)
                                   : ThemeModule.Theme.raisedSurface
                            border.width: Number(game.appId) === root.controller.selectedAppId ? 2 : 1
                            border.color: Number(game.appId) === root.controller.selectedAppId
                                          ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.borderColor

                            Shared.AppCard {
                                id: gameCard
                                width: parent.width
                                padding: 10
                                fillColor: "transparent"
                                border.width: 0

                                RowLayout {
                                    width: parent.width
                                    spacing: 10

                                    Item {
                                        Layout.preferredWidth: 64
                                        Layout.preferredHeight: 64
                                        Rectangle {
                                            anchors.fill: parent
                                            radius: ThemeModule.Theme.controlRadius
                                            color: ThemeModule.Theme.insetSurface
                                            border.color: ThemeModule.Theme.borderColor
                                        }
                                        Image {
                                            anchors.fill: parent
                                            anchors.margins: 1
                                            source: game.artworkSource || ""
                                            sourceSize: Qt.size(128, 128)
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                            cache: false
                                            visible: status === Image.Ready
                                            Accessible.name: qsTr("Artwork for %1").arg(game.name)
                                        }
                                        Label {
                                            anchors.centerIn: parent
                                            visible: !game.artworkSource
                                            text: "◉"
                                            color: ThemeModule.Theme.accentCyan
                                            font.pixelSize: 25
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        Label {
                                            Layout.fillWidth: true
                                            text: String(game.name || "")
                                            textFormat: Text.PlainText
                                            color: ThemeModule.Theme.textPrimary
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 2
                                            elide: Text.ElideRight
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Shared.StatusBadge {
                                                text: String(game.ownershipLabel || qsTr("Following"))
                                                badgeColor: game.ownershipStatus === "owned"
                                                            ? ThemeModule.Theme.success
                                                            : ThemeModule.Theme.accentViolet
                                            }
                                            Item { Layout.fillWidth: true }
                                            Shared.StatusBadge {
                                                text: String(game.unreadNewsCount)
                                                badgeColor: ThemeModule.Theme.accentCyan
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Updated %1")
                                                .arg(root.formattedTimestamp(game.lastSuccessfulNewsCheckAtUtc))
                                            textFormat: Text.PlainText
                                            color: ThemeModule.Theme.textSecondary
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            visible: game.refreshState === "failed"
                                                     && String(game.lastRefreshError || "").length > 0
                                            text: String(game.lastRefreshError || "")
                                            textFormat: Text.PlainText
                                            color: ThemeModule.Theme.error
                                            font.pixelSize: 10
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }

                                RowLayout {
                                    width: parent.width
                                    spacing: 6
                                    Shared.AppButton {
                                        Layout.fillWidth: true
                                        text: qsTr("News")
                                        quiet: true
                                        onClicked: root.controller.setSelectedAppId(Number(game.appId))
                                    }
                                    Shared.AppButton {
                                        text: qsTr("Refresh")
                                        quiet: true
                                        enabled: root.controller && !root.controller.refreshBusy
                                                 && !root.controller.removeBusy
                                        onClicked: root.controller.refreshGame(Number(game.appId))
                                    }
                                    Shared.AppButton {
                                        text: game.ownershipStatus === "owned"
                                              ? qsTr("Set following") : qsTr("Set owned")
                                        quiet: true
                                        onClicked: root.controller.setOwnershipStatus(
                                            Number(game.appId), game.ownershipStatus !== "owned")
                                    }
                                }
                                RowLayout {
                                    width: parent.width
                                    Shared.AppButton {
                                        text: qsTr("Open Store")
                                        quiet: true
                                        onClicked: root.controller.openStore(Number(game.appId))
                                    }
                                    Item { Layout.fillWidth: true }
                                    Shared.AppButton {
                                        text: qsTr("Remove")
                                        quiet: true
                                        destructive: true
                                        enabled: root.controller && !root.controller.removeBusy
                                                 && !root.controller.refreshBusy
                                        onClicked: root.controller.removeGame(Number(game.appId))
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: root.controller && root.controller.snapshotLoaded
                                     && root.controller.games.length === 0
                            text: qsTr("No games followed yet. Add a Steam Store URL above to start a separate news feed.")
                            textFormat: Text.PlainText
                            color: ThemeModule.Theme.textSecondary
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 16

                ColumnLayout {
                    width: parent.width
                    height: parent.height
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: root.controller && root.controller.selectedAppId > 0
                                      ? qsTr("%1 news").arg(root.selectedGameName())
                                      : qsTr("Latest game news")
                                textFormat: Text.PlainText
                                color: ThemeModule.Theme.textPrimary
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.controller
                                      ? qsTr("%1 unread across followed games")
                                        .arg(root.controller.totalUnreadCount)
                                      : ""
                                textFormat: Text.PlainText
                                color: ThemeModule.Theme.textSecondary
                                font.pixelSize: 11
                            }
                        }
                        Shared.AppButton {
                            visible: root.controller && root.controller.selectedAppId > 0
                            text: qsTr("Mark game read")
                            quiet: true
                            enabled: root.controller && !root.controller.newsMutationBusy
                            onClicked: root.controller.markAllRead(root.controller.selectedAppId)
                        }
                        Shared.AppButton {
                            visible: root.controller && root.controller.selectedAppId > 0
                            text: qsTr("Load older from Steam")
                            quiet: true
                            enabled: root.controller && !root.controller.refreshBusy
                                     && !root.controller.removeBusy
                            onClicked: root.controller.loadOlderFromSteam(
                                root.controller.selectedAppId)
                        }
                        Shared.AppButton {
                            visible: root.controller && root.controller.selectedAppId > 0
                            text: qsTr("Refresh game")
                            quiet: true
                            enabled: root.controller && !root.controller.refreshBusy
                                     && !root.controller.removeBusy
                            onClicked: root.controller.refreshGame(root.controller.selectedAppId)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Shared.AppTextField {
                            id: titleSearch
                            Layout.fillWidth: true
                            placeholderText: qsTr("Search cached article titles")
                            accessibleLabel: qsTr("Search cached Game Updates article titles")
                            enabled: root.controller && root.controller.snapshotLoaded
                            onTextEdited: searchDebounce.restart()
                        }
                        Shared.AppComboBox {
                            Layout.preferredWidth: 150
                            model: [
                                { label: qsTr("All articles"), value: false },
                                { label: qsTr("Unread only"), value: true }
                            ]
                            textRole: "label"
                            valueRole: "value"
                            accessibleLabel: qsTr("Filter cached articles by read state")
                            enabled: root.controller && root.controller.snapshotLoaded
                            currentIndex: root.controller && root.controller.unreadOnly ? 1 : 0
                            onActivated: if (root.controller) root.controller.setUnreadOnly(currentValue)
                        }
                    }

                    Timer {
                        id: searchDebounce
                        interval: 300
                        repeat: false
                        onTriggered: if (root.controller) root.controller.setSearchText(titleSearch.text)
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.controller && root.controller.newsError.length > 0
                        text: root.controller ? root.controller.newsError : ""
                        textFormat: Text.PlainText
                        color: ThemeModule.Theme.error
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }

                    ListView {
                        id: newsList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 10
                        boundsBehavior: Flickable.StopAtBounds
                        model: root.controller ? root.controller.newsItems : []
                        Accessible.name: qsTr("Chronological Steam news feed")

                        delegate: Item {
                            required property var modelData
                            readonly property var article: modelData
                            width: ListView.view.width
                            height: articleCard.implicitHeight

                            Shared.AppCard {
                                id: articleCard
                                width: parent.width
                                padding: 13
                                fillColor: article.isRead
                                           ? ThemeModule.Theme.panelSurface
                                           : Qt.alpha(ThemeModule.Theme.accentCyan, 0.055)

                                ColumnLayout {
                                    width: parent.width
                                    spacing: 7

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            Layout.fillWidth: true
                                            text: String(article.gameName || qsTr("Steam game"))
                                                + " · " + String(article.feedLabel || article.feedName || qsTr("Steam News"))
                                            textFormat: Text.PlainText
                                            color: ThemeModule.Theme.accentCyan
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                        Shared.StatusBadge {
                                            text: article.isRead ? qsTr("Read") : qsTr("Unread")
                                            badgeColor: article.isRead
                                                        ? ThemeModule.Theme.textSecondary
                                                        : ThemeModule.Theme.accentCyan
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: String(article.title || "")
                                        textFormat: Text.PlainText
                                        color: ThemeModule.Theme.textPrimary
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.Wrap
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.formattedTimestamp(article.publishedAtUnixSeconds > 0
                                              ? new Date(Number(article.publishedAtUnixSeconds) * 1000).toISOString()
                                              : "")
                                        textFormat: Text.PlainText
                                        color: ThemeModule.Theme.textSecondary
                                        font.pixelSize: 11
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: String(article.contentText || "").length > 0
                                        text: String(article.contentText || "")
                                        textFormat: Text.PlainText
                                        color: ThemeModule.Theme.textSecondary
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 4
                                        elide: Text.ElideRight
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Shared.AppButton {
                                            text: qsTr("Open original")
                                            quiet: true
                                            enabled: article.originalLinkAllowed === true
                                            Accessible.description: article.originalLinkAllowed === true
                                                ? qsTr("Opens the selected article in the native browser.")
                                                : qsTr("This source link is unavailable for safety.")
                                            onClicked: root.controller.openArticle(
                                                Number(article.appId), String(article.newsId))
                                        }
                                        Shared.AppButton {
                                            text: article.isRead ? qsTr("Mark unread") : qsTr("Mark read")
                                            quiet: true
                                            enabled: root.controller && !root.controller.newsMutationBusy
                                            onClicked: root.controller.setNewsRead(
                                                Number(article.appId), String(article.newsId), !article.isRead)
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 36
                            visible: root.controller && root.controller.snapshotLoaded
                                     && !root.controller.newsLoading
                                     && root.controller.newsItems.length === 0
                            text: root.controller && root.controller.games.length === 0
                                  ? qsTr("Follow a game to see its news here.")
                                  : qsTr("No cached articles match these filters. Refresh a game to check for news.")
                            textFormat: Text.PlainText
                            color: ThemeModule.Theme.textSecondary
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: (root.controller && root.controller.newsLoading)
                                 || (root.controller && root.controller.newsHasMore)
                        Label {
                            Layout.fillWidth: true
                            visible: root.controller && root.controller.newsLoading
                            text: qsTr("Loading cached articles…")
                            textFormat: Text.PlainText
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 11
                        }
                        Shared.AppButton {
                            visible: root.controller && root.controller.newsHasMore
                                     && !root.controller.newsLoading
                            text: qsTr("Load older cached news")
                            quiet: true
                            onClicked: root.controller.loadMoreNews()
                        }
                    }
                }
            }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.errorMessage.length > 0
            state: "error"
            title: qsTr("Game Updates needs attention")
            message: root.controller ? root.controller.errorMessage : ""
        }

        Shared.AppButton {
            Layout.alignment: Qt.AlignLeft
            visible: root.controller && !root.controller.snapshotLoaded
                     && !root.controller.loading && root.controller.errorMessage.length > 0
            text: qsTr("Retry loading followed games")
            quiet: true
            onClicked: root.controller.retryLoad()
        }
    }

    function selectedGameName() {
        if (!controller) return qsTr("Game")
        for (let i = 0; i < controller.games.length; ++i) {
            if (Number(controller.games[i].appId) === Number(controller.selectedAppId))
                return String(controller.games[i].name || qsTr("Game"))
        }
        return qsTr("Game")
    }

    Connections {
        target: root.controller
        function onStateChanged() {
            if (root.pendingAddUrl.length > 0 && !root.controller.addBusy
                    && root.controller.addError.length === 0) {
                addUrlField.text = ""
                root.pendingAddUrl = ""
            }
        }
    }
}
