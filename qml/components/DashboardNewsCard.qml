import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Shared.AppCard {
    id: root
    property var controller: dashboardController
    function formatTimestamp(value) {
        if (!value) return ""
        const parsed = new Date(value)
        return Number.isNaN(parsed.getTime()) ? value : parsed.toLocaleString()
    }
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
                text: qsTr("New From Favorites")
                color: ThemeModule.Theme.text
                font.family: ThemeModule.Theme.displayFont
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            BusyIndicator {
                visible: root.controller.favoriteNewsLoading || favoritesController.newsScanning
                running: visible
                implicitWidth: 24
                implicitHeight: 24
                Accessible.name: qsTr("Loading saved favorite updates")
            }
            Shared.AppButton {
                text: qsTr("Refresh updates")
                quiet: true
                enabled: root.controller.ready && !favoritesController.newsScanning
                onClicked: favoritesController.refreshNews()
            }
        }
        Label {
            visible: root.controller.favoriteNewsError.length > 0
                     || favoritesController.newsScanError.length > 0
                     || favoritesController.externalOpenError.length > 0
            Layout.fillWidth: true
            text: root.controller.favoriteNewsError.length > 0
                  ? root.controller.favoriteNewsError
                  : favoritesController.newsScanError.length > 0
                    ? favoritesController.newsScanError : favoritesController.externalOpenError
            color: ThemeModule.Theme.error
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller.favoriteNews.count === 0
            state: (root.controller.favoriteNewsError.length > 0
                    || favoritesController.newsScanError.length > 0) ? "error"
                   : root.controller.favoriteNewsLoading || !root.controller.ready
                     ? "loading" : "empty"
            title: (root.controller.favoriteNewsError.length > 0
                    || favoritesController.newsScanError.length > 0)
                   ? qsTr("Saved updates are unavailable")
                   : qsTr("No saved favorite updates")
            message: root.controller.favoriteNewsError.length > 0
                     ? qsTr("Task cards remain available if this local cache cannot be read.")
                     : favoritesController.newsScanError.length > 0
                       ? favoritesController.newsScanError
                       : qsTr("Favorite-channel posts and videos discovered during app opens and refreshes appear here.")
        }
        Repeater {
            model: root.controller.favoriteNews
            delegate: Rectangle {
                required property string source
                required property string title
                required property string preview
                required property string publishedAt
                required property string sourceId
                required property string itemId
                required property string externalUrl
                required property var imageSources
                required property string views
                Layout.fillWidth: true
                implicitHeight: newsColumn.implicitHeight + 20
                radius: 10
                color: ThemeModule.Theme.panelAlt
                border.color: ThemeModule.Theme.border
                ColumnLayout {
                    id: newsColumn
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 5
                    RowLayout {
                        Layout.fillWidth: true
                        Shared.StatusBadge {
                            text: source === "youtube" ? qsTr("YouTube") : qsTr("Telegram")
                            badgeColor: source === "youtube" ? ThemeModule.Theme.error
                                                                 : ThemeModule.Theme.cyan
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.formatTimestamp(publishedAt)
                            textFormat: Text.PlainText
                            color: ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
                            font.pixelSize: 10
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: title
                        textFormat: Text.PlainText
                        color: ThemeModule.Theme.text
                        font.family: ThemeModule.Theme.displayFont
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }
                    Label {
                        visible: preview.length > 0
                        Layout.fillWidth: true
                        text: preview
                        textFormat: Text.PlainText
                        color: ThemeModule.Theme.textDim
                        font.family: ThemeModule.Theme.bodyFont
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: imageSources
                            delegate: Image {
                                required property string modelData
                                visible: modelData.length > 0
                                source: modelData
                                width: visible ? 152 : 0
                                height: visible ? 96 : 0
                                sourceSize: Qt.size(304, 192)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: true
                                smooth: true
                                Accessible.name: qsTr("Favorite update thumbnail")
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            visible: views.length > 0
                            text: views
                            textFormat: Text.PlainText
                            color: ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
                            font.pixelSize: 10
                        }
                        Shared.AppButton {
                            visible: externalUrl.length > 0
                            text: source === "youtube" ? qsTr("Open video") : qsTr("Open post")
                            quiet: true
                            onClicked: favoritesController.openExternal(source, externalUrl)
                        }
                    }
                }
            }
        }
    }
}
