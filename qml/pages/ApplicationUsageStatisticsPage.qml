import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root
    required property var controller
    implicitHeight: content.implicitHeight
    property string customFirstDay: ""
    property string customLastDay: ""
    property string pendingDeleteId: ""

    function stateLabel(state) {
        if (state === "ForegroundFullscreen") return qsTr("Foreground fullscreen")
        if (state === "ForegroundWindowed") return qsTr("Foreground windowed")
        if (state === "BackgroundVisible") return qsTr("Background visible")
        if (state === "Minimized") return qsTr("Minimized")
        if (state === "Unknown") return qsTr("Unknown interval")
        return qsTr("Not running")
    }

    function rangeIndex(preset) {
        const values = ["today", "yesterday", "last7Days", "last30Days", "currentMonth", "custom"]
        return Math.max(0, values.indexOf(preset))
    }

    function sortIndex(value) {
        const values = ["total", "foreground", "fullscreen", "name", "lastActivity"]
        return Math.max(0, values.indexOf(value))
    }

    Component.onCompleted: {
        root.customFirstDay = root.controller.customFirstDay
        root.customLastDay = root.controller.customLastDay
    }

    Connections {
        target: root.controller
        function onStateChanged() {
            dailyChart.requestPaint()
            hourlyChart.requestPaint()
            if (root.controller.rangePreset === "custom" && !root.customFirstDay) {
                root.customFirstDay = root.controller.customFirstDay
                root.customLastDay = root.controller.customLastDay
            }
        }
    }

    Dialogs.ConfirmationDialog {
        id: deleteApplicationHistoryConfirmation
        title: qsTr("Delete this application's usage history?")
        message: qsTr("The selected application's saved sessions and state segments will be permanently deleted. This cannot be undone.")
        confirmText: qsTr("Delete history")
        destructive: true
        onConfirmed: {
            root.controller.deleteHistory(root.pendingDeleteId)
            root.pendingDeleteId = ""
            root.controller.clearSelection()
        }
    }

    ColumnLayout {
        id: content
        width: parent.width
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Application Usage Statistics")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.headingFontSize
                    font.weight: Font.Bold
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Local activity totals for eligible taskbar windows. Foreground time means a window owns focus; it does not measure keyboard or mouse activity.")
                    color: ThemeModule.Theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
            Shared.AppButton {
                visible: root.controller.selectedApplicationId.length > 0
                text: qsTr("All applications")
                quiet: true
                onClicked: root.controller.clearSelection()
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            Layout.preferredHeight: overviewRow.implicitHeight + padding * 2
            RowLayout {
                id: overviewRow
                width: parent.width
                spacing: 14

                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Foreground time"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 12 }
                    Label {
                        text: root.controller.formatDuration(Number(root.controller.overview.foregroundMs || 0))
                        color: ThemeModule.Theme.textPrimary
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                }
                Rectangle { width: 1; Layout.fillHeight: true; color: ThemeModule.Theme.borderColor }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Tracked runtime"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 12 }
                    Label {
                        text: root.controller.formatDuration(Number(root.controller.overview.totalMs || 0))
                        color: ThemeModule.Theme.textPrimary
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                }
                Rectangle { width: 1; Layout.fillHeight: true; color: ThemeModule.Theme.borderColor }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Most used"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 12 }
                    Label {
                        Layout.fillWidth: true
                        text: root.controller.overview.mostUsed
                              && root.controller.overview.mostUsed.name
                              ? root.controller.overview.mostUsed.name : qsTr("No activity yet")
                        color: ThemeModule.Theme.textPrimary
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                }
                Rectangle { width: 1; Layout.fillHeight: true; color: ThemeModule.Theme.borderColor }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Applications"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 12 }
                    Label {
                        text: String(root.controller.overview.applicationCount || 0)
                        color: ThemeModule.Theme.textPrimary
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Shared.AppComboBox {
                id: rangeChoice
                Layout.preferredWidth: 180
                model: [qsTr("Today"), qsTr("Yesterday"), qsTr("Last 7 days"),
                        qsTr("Last 30 days"), qsTr("Current month"), qsTr("Custom range")]
                currentIndex: root.rangeIndex(root.controller.rangePreset)
                accessibleLabel: qsTr("Usage date range")
                onActivated: function(index) {
                    const values = ["today", "yesterday", "last7Days", "last30Days", "currentMonth", "custom"]
                    root.controller.setRangePreset(values[index])
                }
            }
            Shared.AppTextField {
                Layout.fillWidth: true
                placeholderText: qsTr("Search applications")
                accessibleLabel: qsTr("Search usage statistics by application name")
                text: root.controller.searchText
                onTextChanged: root.controller.setSearchText(text)
            }
            Shared.AppComboBox {
                Layout.preferredWidth: 190
                model: [qsTr("Total runtime"), qsTr("Foreground time"), qsTr("Fullscreen time"),
                        qsTr("Application name"), qsTr("Last activity")]
                currentIndex: root.sortIndex(root.controller.sortKey)
                accessibleLabel: qsTr("Sort applications")
                onActivated: function(index) {
                    const values = ["total", "foreground", "fullscreen", "name", "lastActivity"]
                    root.controller.setSortKey(values[index])
                }
            }
            Shared.AppButton {
                text: root.controller.trackingEnabled ? qsTr("Pause") : qsTr("Resume")
                quiet: true
                enabled: root.controller.ready && root.controller.supported
                onClicked: root.controller.setTrackingEnabled(!root.controller.trackingEnabled)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.controller.rangePreset === "custom"
            Shared.AppTextField {
                id: customFirst
                Layout.preferredWidth: 170
                placeholderText: qsTr("From YYYY-MM-DD")
                accessibleLabel: qsTr("Custom range start date")
                text: root.customFirstDay
                onTextChanged: root.customFirstDay = text
            }
            Shared.AppTextField {
                id: customLast
                Layout.preferredWidth: 170
                placeholderText: qsTr("To YYYY-MM-DD")
                accessibleLabel: qsTr("Custom range end date")
                text: root.customLastDay
                onTextChanged: root.customLastDay = text
            }
            Shared.AppButton {
                text: qsTr("Apply range")
                onClicked: root.controller.setCustomRange(root.customFirstDay, root.customLastDay)
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            Layout.preferredHeight: statusRow.implicitHeight + padding * 2
            RowLayout {
                id: statusRow
                width: parent.width
                Shared.StatusBadge {
                    text: !root.controller.supported ? qsTr("Unavailable")
                          : root.controller.trackingActive ? qsTr("Tracking")
                          : root.controller.trackingEnabled ? qsTr("Starting") : qsTr("Paused")
                    badgeColor: root.controller.trackingActive
                                ? ThemeModule.Theme.success : ThemeModule.Theme.warning
                }
                Label {
                    Layout.fillWidth: true
                    text: root.controller.trackingStatus
                    color: ThemeModule.Theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Shared.AppButton {
                    text: root.controller.loading ? qsTr("Loading…") : qsTr("Refresh")
                    quiet: true
                    enabled: !root.controller.loading
                    onClicked: root.controller.refresh()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.errorMessage.length > 0
            text: root.controller.errorMessage
            color: ThemeModule.Theme.error
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }

        Shared.AppCard {
            Layout.fillWidth: true
            Layout.preferredHeight: applicationsColumn.implicitHeight + padding * 2
            visible: root.controller.selectedApplicationId.length === 0

            ColumnLayout {
                id: applicationsColumn
                width: parent.width
                spacing: 10
                Label {
                    text: qsTr("Applications")
                    color: ThemeModule.Theme.textPrimary
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                ListView {
                    id: applicationList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(520, Math.max(82, contentHeight))
                    clip: true
                    model: root.controller.applications
                    spacing: 7
                    delegate: Shared.AppCard {
                        required property var modelData
                        width: applicationList.width
                        height: 78
                        padding: 10
                        RowLayout {
                            anchors.fill: parent
                            spacing: 10
                            Rectangle {
                                Layout.preferredWidth: 42
                                Layout.preferredHeight: 42
                                radius: ThemeModule.Theme.controlRadius
                                color: ThemeModule.Theme.insetSurface
                                border.color: ThemeModule.Theme.borderColor
                                Image {
                                    id: applicationIcon
                                    anchors.fill: parent
                                    anchors.margins: 3
                                    source: modelData.executablePath
                                            ? "image://application-usage-icons/" + encodeURIComponent(modelData.executablePath)
                                            : ""
                                    sourceSize: Qt.size(64, 64)
                                    asynchronous: true
                                    fillMode: Image.PreserveAspectFit
                                    visible: status === Image.Ready
                                }
                                Label {
                                    anchors.centerIn: parent
                                    text: String(modelData.name || "?").trim().charAt(0).toUpperCase()
                                    color: ThemeModule.Theme.accentCyan
                                    font.pixelSize: 19
                                    font.weight: Font.Bold
                                    visible: applicationIcon.status !== Image.Ready
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.name || modelData.executableName || qsTr("Unknown application")
                                    color: ThemeModule.Theme.textPrimary
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.running ? root.stateLabel(modelData.currentState)
                                          : modelData.lastSeenUtc ? qsTr("Last activity %1").arg(root.controller.formatTimestamp(modelData.lastSeenUtc))
                                          : qsTr("No activity in this range")
                                    color: modelData.running ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.textSecondary
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                            ColumnLayout {
                                Layout.preferredWidth: 112
                                Label { text: qsTr("Runtime"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 10 }
                                Label {
                                    text: root.controller.formatDuration(Number(modelData.totalMs || 0))
                                    color: ThemeModule.Theme.textPrimary
                                    font.weight: Font.DemiBold
                                }
                            }
                            ColumnLayout {
                                Layout.preferredWidth: 118
                                Label { text: qsTr("Foreground"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 10 }
                                Label {
                                    text: root.controller.formatDuration(Number(modelData.foregroundMs || 0))
                                    color: ThemeModule.Theme.textPrimary
                                }
                            }
                            ColumnLayout {
                                Layout.preferredWidth: 104
                                Label { text: qsTr("Fullscreen"); color: ThemeModule.Theme.textSecondary; font.pixelSize: 10 }
                                Label {
                                    text: root.controller.formatDuration(Number(modelData.fullscreenMs || 0))
                                    color: ThemeModule.Theme.textPrimary
                                }
                            }
                        }
                        TapHandler {
                            onTapped: root.controller.selectApplication(modelData.applicationId)
                        }
                    }
                    ScrollBar.vertical: Shared.AppScrollBar { policy: ScrollBar.AsNeeded }
                }
                Shared.StatePanel {
                    Layout.fillWidth: true
                    visible: root.controller.applications.length === 0
                    height: 110
                    state: root.controller.loading ? "loading" : "empty"
                    title: root.controller.loading ? qsTr("Loading usage statistics") : qsTr("No tracked activity for this range")
                    message: root.controller.trackingActive
                             ? qsTr("Eligible taskbar applications will appear automatically as they are observed.")
                             : qsTr("Enable tracking to begin collecting local application usage time.")
                }
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            Layout.preferredHeight: detailsColumn.implicitHeight + padding * 2
            visible: root.controller.selectedApplicationId.length > 0

            ColumnLayout {
                id: detailsColumn
                width: parent.width
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: root.controller.detail.name || qsTr("Application details")
                            color: ThemeModule.Theme.textPrimary
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            text: root.controller.detail.running
                                  ? root.stateLabel(root.controller.detail.currentState) : qsTr("Not currently running")
                            color: root.controller.detail.running ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.textSecondary
                            font.pixelSize: 12
                        }
                    }
                    Shared.AppButton {
                        text: qsTr("Exclude application")
                        quiet: true
                        enabled: root.controller.detail.applicationId !== undefined
                        onClicked: root.controller.addApplicationExclusion(
                            root.controller.detail.applicationId,
                            root.controller.detail.executablePath,
                            root.controller.detail.executableName)
                    }
                    Shared.AppButton {
                        text: qsTr("Delete history")
                        quiet: true
                        destructive: true
                        enabled: root.controller.detail.applicationId !== undefined
                        onClicked: {
                            root.pendingDeleteId = root.controller.detail.applicationId
                            deleteApplicationHistoryConfirmation.open()
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Repeater {
                        model: [
                            { title: qsTr("Tracked runtime"), value: root.controller.detail.totalMs || 0 },
                            { title: qsTr("Foreground"), value: root.controller.detail.foregroundMs || 0 },
                            { title: qsTr("Sessions"), value: root.controller.detail.sessionCount || 0 },
                            { title: qsTr("Last observed"), value: root.controller.formatTimestamp(root.controller.detail.lastSeenUtc || "") }
                        ]
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label { text: modelData.title; color: ThemeModule.Theme.textSecondary; font.pixelSize: 11 }
                            Label {
                                text: typeof modelData.value === "string"
                                      ? modelData.value : root.controller.formatDuration(Number(modelData.value))
                                color: ThemeModule.Theme.textPrimary
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("First observed: %1").arg(root.controller.formatTimestamp(root.controller.detail.firstSeenUtc || ""))
                    color: ThemeModule.Theme.textSecondary
                    font.pixelSize: 11
                }

                Label {
                    text: qsTr("State breakdown")
                    color: ThemeModule.Theme.textPrimary
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: [
                        { key: "ForegroundFullscreen", color: ThemeModule.Theme.accentCyan },
                        { key: "ForegroundWindowed", color: ThemeModule.Theme.accentViolet },
                        { key: "BackgroundVisible", color: ThemeModule.Theme.success },
                        { key: "Minimized", color: ThemeModule.Theme.warning },
                        { key: "Unknown", color: ThemeModule.Theme.textSecondary }
                    ]
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.preferredWidth: 170
                            text: root.stateLabel(modelData.key)
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 11
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 10
                            radius: 5
                            color: ThemeModule.Theme.insetSurface
                            Rectangle {
                                width: root.controller.detail.totalMs > 0
                                       ? parent.width * Number((root.controller.detail.distribution || {})[modelData.key] || 0)
                                         / Number(root.controller.detail.totalMs) : 0
                                height: parent.height
                                radius: parent.radius
                                color: modelData.color
                            }
                        }
                        Label {
                            Layout.preferredWidth: 110
                            horizontalAlignment: Text.AlignRight
                            text: root.controller.formatDuration(Number((root.controller.detail.distribution || {})[modelData.key] || 0))
                            color: ThemeModule.Theme.textPrimary
                            font.pixelSize: 11
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Unknown intervals are shown separately. They include short periods when window state could not be read; long observation gaps are omitted.")
                    color: ThemeModule.Theme.textSecondary
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    Shared.AppCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 230
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: qsTr("Daily usage"); color: ThemeModule.Theme.textPrimary; font.weight: Font.DemiBold }
                            Canvas {
                                id: dailyChart
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                onPaint: {
                                    const ctx = getContext("2d")
                                    ctx.clearRect(0, 0, width, height)
                                    const points = root.controller.detail.daily || []
                                    if (!points.length) return
                                    const maxBars = Math.max(1, Math.floor(width / 42))
                                    const bucketSize = Math.max(1, Math.ceil(points.length / maxBars))
                                    const bars = []
                                    let max = 1
                                    for (let first = 0; first < points.length; first += bucketSize) {
                                        const last = Math.min(points.length, first + bucketSize)
                                        let total = 0
                                        for (let i = first; i < last; ++i) total += Number(points[i].totalMs)
                                        bars.push({ totalMs: total, label: String(points[last - 1].day).slice(5) })
                                        max = Math.max(max, total)
                                    }
                                    const gap = 5
                                    const barWidth = Math.max(4, (width - gap * (bars.length + 1)) / bars.length)
                                    ctx.font = "9px sans-serif"
                                    ctx.textAlign = "center"
                                    for (let j = 0; j < bars.length; ++j) {
                                        const barHeight = Math.max(2, (height - 25) * bars[j].totalMs / max)
                                        ctx.fillStyle = ThemeModule.Theme.accentCyan
                                        const x = gap + j * (barWidth + gap)
                                        ctx.fillRect(x, height - 17 - barHeight, barWidth, barHeight)
                                        ctx.fillStyle = ThemeModule.Theme.textSecondary
                                        ctx.fillText(bars[j].label, x + barWidth / 2, height - 3)
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.controller.detail.daily && root.controller.detail.daily.length
                                      ? qsTr("Bars show local-day totals; long ranges are grouped to fit the chart.")
                                      : qsTr("No daily usage in this range.")
                                color: ThemeModule.Theme.textSecondary
                                font.pixelSize: 10
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                    Shared.AppCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 230
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: qsTr("Usage by local hour"); color: ThemeModule.Theme.textPrimary; font.weight: Font.DemiBold }
                            Canvas {
                                id: hourlyChart
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                onPaint: {
                                    const ctx = getContext("2d")
                                    ctx.clearRect(0, 0, width, height)
                                    const points = root.controller.detail.hourly || []
                                    if (!points.length) return
                                    const bars = new Array(24).fill(0)
                                    let max = 1
                                    for (let i = 0; i < points.length; ++i) {
                                        const hour = Number(points[i].hour)
                                        bars[hour] = Number(points[i].totalMs)
                                        max = Math.max(max, bars[hour])
                                    }
                                    const gap = 2
                                    const barWidth = Math.max(1, (width - gap * 25) / 24)
                                    for (let h = 0; h < 24; ++h) {
                                        const barHeight = Math.max(bars[h] > 0 ? 2 : 0, (height - 18) * bars[h] / max)
                                        ctx.fillStyle = ThemeModule.Theme.accentViolet
                                        ctx.fillRect(gap + h * (barWidth + gap), height - 14 - barHeight, barWidth, barHeight)
                                    }
                                    ctx.font = "9px sans-serif"
                                    ctx.textAlign = "center"
                                    ctx.fillStyle = ThemeModule.Theme.textSecondary
                                    for (const hour of [0, 6, 12, 18, 23]) {
                                        const x = gap + hour * (barWidth + gap) + barWidth / 2
                                        ctx.fillText(String(hour).padStart(2, "0"), x, height - 1)
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Hours use the local time zone recorded for each segment.")
                                color: ThemeModule.Theme.textSecondary
                                font.pixelSize: 10
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }

                Label {
                    text: qsTr("Recent sessions")
                    color: ThemeModule.Theme.textPrimary
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: root.controller.detail.sessions || []
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: root.controller.formatTimestamp(modelData.startedUtc)
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 11
                        }
                        Label {
                            text: root.controller.formatDuration(Number(modelData.durationMs || 0))
                            color: ThemeModule.Theme.textPrimary
                            font.pixelSize: 11
                        }
                        Label {
                            text: modelData.endReason || qsTr("Running")
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 11
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: !root.controller.detail.sessions || root.controller.detail.sessions.length === 0
                    text: qsTr("No session history is available for this range.")
                    color: ThemeModule.Theme.textSecondary
                    font.pixelSize: 11
                }
            }
        }
    }
}
