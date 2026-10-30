import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root

    property var controller
    property bool active: false
    property var savedViewState: ({})
    property string pendingAddUrl: ""
    property string statusFilter: "all"
    property string sortOrder: "release_asc"
    property int countdownDay: new Date().getDate()
    property real calendarFocusedAppId: 0
    property bool calendarRevealInProgress: false

    implicitHeight: content.implicitHeight + 8
    Accessible.name: qsTr("Steam Release Radar")

    function captureViewState() {
        return {
            addUrl: storeUrlField.text,
            search: searchField.text,
            statusFilter: statusFilter,
            sortOrder: sortOrder,
            listPosition: releasesList.contentY
        }
    }

    function restoreViewState(state) {
        savedViewState = state || {}
        storeUrlField.text = String(savedViewState.addUrl || "")
        searchField.text = String(savedViewState.search || "")
        statusFilter = String(savedViewState.statusFilter || "all")
        sortOrder = String(savedViewState.sortOrder || "release_asc")
        if (controller) {
            controller.setSearchText(searchField.text)
            controller.setStatusFilter(statusFilter)
            controller.setSortOrder(sortOrder)
        }
        Qt.callLater(function() {
            releasesList.contentY = Math.max(0, Number(root.savedViewState.listPosition || 0))
        })
    }

    function addRelease() {
        if (!controller || storeUrlField.text.trim().length === 0)
            return
        pendingAddUrl = storeUrlField.text
        controller.addRelease(storeUrlField.text)
    }

    function revealPendingCalendarRelease() {
        if (!active || !controller || !controller.snapshotLoaded
                || calendarRevealInProgress) return
        const appId = Number(controller.pendingReleaseDetailsAppId)
        if (appId <= 0) return

        calendarRevealInProgress = true
        statusFilter = "all"
        searchField.text = ""
        controller.setSearchText("")
        controller.setStatusFilter("all")
        Qt.callLater(function() {
            const index = root.controller.releases.visibleRowForAppId(appId)
            if (index >= 0) {
                root.calendarFocusedAppId = appId
                releasesList.positionViewAtIndex(index, ListView.Contain)
            }
            root.calendarRevealInProgress = false
            root.controller.clearPendingReleaseDetailsRequest()
        })
    }

    function formattedTimestamp(value) {
        if (!value || String(value).length === 0)
            return qsTr("Not checked yet")
        const parsed = new Date(String(value))
        if (isNaN(parsed.getTime()))
            return String(value)
        return Qt.formatDateTime(parsed, "yyyy-MM-dd HH:mm")
    }

    function countdownDays(exactDate) {
        const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(exactDate || ""))
        if (!match)
            return -1
        const releaseDay = Date.UTC(Number(match[1]), Number(match[2]) - 1, Number(match[3]))
        const now = new Date()
        const today = Date.UTC(now.getFullYear(), now.getMonth(), root.countdownDay)
        return Math.floor((releaseDay - today) / 86400000)
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
                text: qsTr("Game Release Radar")
                color: ThemeModule.Theme.textPrimary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            Label {
                width: parent.width
                text: qsTr("Follow Steam games independently from price tracking. Release windows stay approximate unless Steam announces an exact day.")
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.Wrap
            }

            RowLayout {
                width: parent.width
                spacing: 10
                Shared.AppTextField {
                    id: storeUrlField
                    Layout.fillWidth: true
                    placeholderText: qsTr("https://store.steampowered.com/app/123456/")
                    accessibleLabel: qsTr("Steam Store URL to follow in Release Radar")
                    inputMethodHints: Qt.ImhUrlCharactersOnly
                    enabled: root.controller && root.controller.snapshotLoaded
                             && !root.controller.addBusy && !root.controller.refreshBusy
                             && !root.controller.deleteBusy
                             && !root.controller.dateChangeAcknowledgeBusy
                    onAccepted: root.addRelease()
                }
                Shared.AppButton {
                    text: root.controller && root.controller.addBusy
                          ? qsTr("Adding…") : qsTr("Follow release")
                    enabled: root.controller && root.controller.snapshotLoaded
                             && !root.controller.addBusy && !root.controller.refreshBusy
                             && !root.controller.deleteBusy
                             && !root.controller.dateChangeAcknowledgeBusy
                    onClicked: root.addRelease()
                }
            }
            Label {
                width: parent.width
                visible: root.controller && root.controller.addError.length > 0
                text: root.controller ? root.controller.addError : ""
                color: ThemeModule.Theme.error
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }

            RowLayout {
                width: parent.width
                spacing: 10
                Shared.AppTextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search followed games")
                    accessibleLabel: qsTr("Search Release Radar")
                    enabled: root.controller && root.controller.snapshotLoaded
                    onTextEdited: if (root.controller) root.controller.setSearchText(text)
                }
                Shared.AppComboBox {
                    id: statusBox
                    Layout.preferredWidth: 180
                    model: [
                        { label: qsTr("All releases"), value: "all" },
                        { label: qsTr("Upcoming"), value: "upcoming" },
                        { label: qsTr("Released"), value: "released" },
                        { label: qsTr("Unknown date"), value: "unknown_date" },
                        { label: qsTr("Unavailable"), value: "unavailable" }
                    ]
                    textRole: "label"
                    valueRole: "value"
                    accessibleLabel: qsTr("Filter releases by status")
                    enabled: root.controller && root.controller.snapshotLoaded
                    onActivated: function(index) {
                        root.statusFilter = currentValue
                        if (root.controller) root.controller.setStatusFilter(currentValue)
                    }
                }
                Shared.AppComboBox {
                    id: sortBox
                    Layout.preferredWidth: 170
                    model: [
                        { label: qsTr("Release date"), value: "release_asc" },
                        { label: qsTr("Latest date first"), value: "release_desc" },
                        { label: qsTr("Title"), value: "title" }
                    ]
                    textRole: "label"
                    valueRole: "value"
                    accessibleLabel: qsTr("Sort followed releases")
                    enabled: root.controller && root.controller.snapshotLoaded
                    onActivated: function(index) {
                        root.sortOrder = currentValue
                        if (root.controller) root.controller.setSortOrder(currentValue)
                    }
                }
                Shared.AppButton {
                    text: root.controller && root.controller.refreshBusy
                          ? (root.controller.refreshCancelable
                             ? qsTr("Refreshing…") : qsTr("Saving…"))
                          : qsTr("Refresh")
                    quiet: true
                    enabled: root.controller && root.controller.snapshotLoaded
                             && root.controller.releases.count > 0
                             && !root.controller.refreshBusy && !root.controller.addBusy
                     && !root.controller.deleteBusy
                     && !root.controller.dateChangeAcknowledgeBusy
                    onClicked: root.controller.refreshReleases()
                }
            }

            RowLayout {
                width: parent.width
                visible: root.controller && root.controller.refreshBusy
                Item { Layout.fillWidth: true }
                Shared.AppButton {
                    text: qsTr("Cancel refresh")
                    quiet: true
                    enabled: root.controller.refreshCancelable
                    onClicked: root.controller.cancelRefresh()
                }
            }

            Label {
                width: parent.width
                visible: root.controller && root.controller.refreshStatus.length > 0
                text: root.controller ? root.controller.refreshStatus : ""
                color: root.controller && root.controller.refreshBusy
                       ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.errorMessage.length > 0
            state: "error"
            title: qsTr("Release Radar needs attention")
            message: root.controller ? root.controller.errorMessage : ""
        }

        Shared.AppButton {
            Layout.alignment: Qt.AlignLeft
            visible: root.controller && !root.controller.snapshotLoaded
                     && !root.controller.loading && root.controller.errorMessage.length > 0
            text: qsTr("Retry loading followed releases")
            quiet: true
            onClicked: root.controller.retryLoad()
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.loading
                     && root.controller.releases.count === 0
            state: "loading"
            title: qsTr("Loading Release Radar")
            message: qsTr("Reading followed releases from this profile…")
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && !root.controller.loading
                     && root.controller.snapshotLoaded && root.controller.releases.count === 0
            state: "empty"
            title: qsTr("No followed releases yet")
            message: qsTr("Add a Steam Store game URL to start watching its release information.")
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.snapshotLoaded
                     && root.controller.releases.count > 0
                     && root.controller.releases.visibleCount === 0
            state: "empty"
            title: qsTr("No matching releases")
            message: qsTr("Try another search or status filter.")
        }

        ListView {
            id: releasesList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(720, Math.max(0, contentHeight))
            visible: root.controller && root.controller.snapshotLoaded
                     && root.controller.releases.visibleCount > 0
            model: root.controller ? root.controller.releases : null
            clip: true
            spacing: 10
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Followed Steam releases")

            delegate: Item {
                id: delegateRoot
                required property var appId
                required property string title
                required property string storeUrl
                required property string artworkUrl
                required property string artworkSource
                required property string releaseDateText
                required property string datePrecisionLabel
                required property bool hasExactDate
                required property string exactDate
                required property string status
                required property string statusLabel
                required property string lastSuccessfulUpdateAtUtc
                required property string lastAttemptedUpdateAtUtc
                required property string refreshState
                required property string refreshError
                required property bool dateChangeUnacknowledged
                property bool radarActive: root.active

                width: ListView.view.width
                height: releaseCard.implicitHeight

                function requestArtworkIfNeeded() {
                    if (radarActive && artworkSource.length === 0
                            && artworkUrl.length > 0 && root.controller)
                        root.controller.ensureArtwork(Number(appId))
                }

                onRadarActiveChanged: requestArtworkIfNeeded()
                onArtworkSourceChanged: requestArtworkIfNeeded()
                onArtworkUrlChanged: requestArtworkIfNeeded()
                Component.onCompleted: requestArtworkIfNeeded()

                Shared.AppCard {
                    id: releaseCard
                    width: parent.width
                    padding: 12
                    border.width: Number(delegateRoot.appId) === root.calendarFocusedAppId
                                  ? 2 : ThemeModule.Theme.panelBorderWidth
                    border.color: Number(delegateRoot.appId) === root.calendarFocusedAppId
                                  ? ThemeModule.Theme.accentViolet : ThemeModule.Theme.borderColor

                    RowLayout {
                        width: parent.width
                        spacing: 16

                        Item {
                            Layout.preferredWidth: 190
                            Layout.preferredHeight: 112
                            Layout.alignment: Qt.AlignTop
                            Rectangle {
                                anchors.fill: parent
                                radius: ThemeModule.Theme.controlRadius
                                color: ThemeModule.Theme.raisedSurface
                                border.color: ThemeModule.Theme.borderColor
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
                                Accessible.name: qsTr("Artwork for %1").arg(delegateRoot.title)
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
                                    color: ThemeModule.Theme.accentCyan
                                    font.pixelSize: 27
                                }
                                Label {
                                    text: qsTr("Artwork unavailable")
                                    color: ThemeModule.Theme.textSecondary
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
                                text: delegateRoot.title
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                Accessible.name: delegateRoot.title
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Shared.StatusBadge {
                                    text: delegateRoot.statusLabel
                                    badgeColor: delegateRoot.status === "released"
                                                ? ThemeModule.Theme.success
                                                : delegateRoot.status === "coming_soon"
                                                  || delegateRoot.status === "information_unavailable"
                                                  ? ThemeModule.Theme.warning
                                                  : ThemeModule.Theme.accentCyan
                                }
                                Shared.StatusBadge {
                                    visible: delegateRoot.hasExactDate
                                             && root.countdownDays(delegateRoot.exactDate) > 0
                                    text: visible
                                          ? qsTr("In %1 days").arg(root.countdownDays(delegateRoot.exactDate))
                                          : ""
                                    badgeColor: ThemeModule.Theme.success
                                }
                                Shared.StatusBadge {
                                    visible: delegateRoot.refreshState === "failed"
                                    text: qsTr("Update failed")
                                    badgeColor: ThemeModule.Theme.error
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                visible: delegateRoot.dateChangeUnacknowledged
                                Shared.StatusBadge {
                                    text: qsTr("Release date changed")
                                    badgeColor: ThemeModule.Theme.warning
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Steam’s latest announcement is shown below.")
                                    color: ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                }
                                Shared.AppButton {
                                    text: qsTr("Acknowledge")
                                    quiet: true
                                    enabled: root.controller
                                             && !root.controller.dateChangeAcknowledgeBusy
                                             && !root.controller.refreshBusy
                                             && !root.controller.deleteBusy
                                             && !root.controller.addBusy
                                    Accessible.name: qsTr("Acknowledge release date change for %1")
                                                     .arg(delegateRoot.title)
                                    onClicked: root.controller.acknowledgeDateChange(
                                                   Number(delegateRoot.appId))
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: delegateRoot.releaseDateText.length > 0
                                      ? delegateRoot.releaseDateText : delegateRoot.statusLabel
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 15
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: delegateRoot.releaseDateText.length > 0
                                         && delegateRoot.datePrecisionLabel !== "Exact date"
                                text: qsTr("Precision: %1. Steam has not announced an exact day.")
                                      .arg(delegateRoot.datePrecisionLabel)
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Last successful update: %1")
                                      .arg(root.formattedTimestamp(delegateRoot.lastSuccessfulUpdateAtUtc))
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: delegateRoot.refreshState === "failed"
                                         && delegateRoot.refreshError.length > 0
                                text: delegateRoot.lastAttemptedUpdateAtUtc.length > 0
                                      ? qsTr("Last attempt: %1 · %2")
                                        .arg(root.formattedTimestamp(delegateRoot.lastAttemptedUpdateAtUtc),
                                             delegateRoot.refreshError)
                                      : delegateRoot.refreshError
                                color: ThemeModule.Theme.error
                                font.family: ThemeModule.Theme.fontFamily
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
                                    Accessible.name: qsTr("Open Steam Store page for %1").arg(delegateRoot.title)
                                    onClicked: root.controller.openStore(Number(delegateRoot.appId))
                                }
                                Shared.AppButton {
                                    text: qsTr("Remove")
                                    destructive: true
                                    enabled: root.controller && !root.controller.deleteBusy
                                             && !root.controller.refreshBusy
                                             && !root.controller.addBusy
                                             && !root.controller.dateChangeAcknowledgeBusy
                                    Accessible.name: qsTr("Remove %1 from Release Radar").arg(delegateRoot.title)
                                    onClicked: {
                                        removeDialog.appId = Number(delegateRoot.appId)
                                        removeDialog.gameTitle = delegateRoot.title
                                        removeDialog.open()
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }
            ScrollBar.vertical: Shared.AppScrollBar { }
        }

        Shared.NotificationToast {
            Layout.alignment: Qt.AlignHCenter
            message: root.controller ? root.controller.notification : ""
            onDismissed: if (root.controller) root.controller.clearNotification()
        }
    }

    Dialogs.ConfirmationDialog {
        id: removeDialog
        property var appId: 0
        property string gameTitle: ""
        title: qsTr("Remove followed release?")
        message: qsTr("Remove “%1” from Release Radar? This does not affect Steam price tracking.")
                 .arg(gameTitle)
        confirmText: qsTr("Remove release")
        destructive: true
        onConfirmed: if (root.controller) root.controller.removeRelease(Number(appId))
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
            root.revealPendingCalendarRelease()
        }
    }

    Component.onCompleted: {
        if (controller) {
            controller.setSearchText(searchField.text)
            controller.setStatusFilter(statusFilter)
            controller.setSortOrder(sortOrder)
        }
    }

    Timer {
        interval: 60000
        repeat: true
        running: root.active
        onTriggered: root.countdownDay = new Date().getDate()
    }

    onStatusFilterChanged: {
        if (controller) controller.setStatusFilter(statusFilter)
        for (let i = 0; i < statusBox.count; ++i) {
            if (statusBox.valueAt(i) === statusFilter) {
                statusBox.currentIndex = i
                break
            }
        }
    }

    onSortOrderChanged: {
        if (controller) controller.setSortOrder(sortOrder)
        for (let i = 0; i < sortBox.count; ++i) {
            if (sortBox.valueAt(i) === sortOrder) {
                sortBox.currentIndex = i
                break
            }
        }
    }

    onActiveChanged: {
        if (active) countdownDay = new Date().getDate()
    }
}
