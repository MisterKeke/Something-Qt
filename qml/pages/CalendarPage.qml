import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root

    property var controller: calendarController
    property var taskActions: taskController
    property bool embedded: false
    signal releaseRequested(var appId)
    implicitHeight: calendarColumn.implicitHeight

    function dateAccessibleName(day) {
        var parts = []
        if (Number(day.deadlineCount) > 0)
            parts.push(qsTr("%1 deadlines").arg(Number(day.deadlineCount)))
        if (Number(day.plannedWorkCount) > 0)
            parts.push(qsTr("%1 planned work items").arg(Number(day.plannedWorkCount)))
        if (Number(day.completedTaskCount) > 0)
            parts.push(qsTr("%1 completed tasks").arg(Number(day.completedTaskCount)))
        if (root.controller.showGameReleases && Number(day.gameReleaseCount) > 0)
            parts.push(qsTr("%1 game releases").arg(Number(day.gameReleaseCount)))
        var activity = parts.length > 0 ? qsTr(". ") + parts.join(", ") : ""
        return (Boolean(day.selected) ? qsTr("Selected date, ") : "")
               + String(day.date) + activity
    }

    Component.onCompleted: controller.setPageVisible(visible)
    onVisibleChanged: if (controller) controller.setPageVisible(visible)
    Component.onDestruction: controller.setPageVisible(false)

    ColumnLayout {
        id: calendarColumn
        width: root.width
        spacing: 14

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 15
            ColumnLayout {
                width: parent.width
                spacing: 13
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("%1 Calendar").arg(root.controller.visibleYear)
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            horizontalAlignment: root.embedded ? Text.AlignLeft : Text.AlignHCenter
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.controller.rangeLabel
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                            horizontalAlignment: root.embedded ? Text.AlignLeft : Text.AlignHCenter
                        }
                    }
                    Shared.AppButton {
                        text: root.embedded ? "‹" : qsTr("Previous Year")
                        implicitWidth: root.embedded ? 38 : 130
                        implicitHeight: root.embedded ? 34 : 40
                        Accessible.name: qsTr("Previous year")
                        quiet: true
                        onClicked: root.controller.previousYear()
                    }
                    Shared.AppButton {
                        text: qsTr("Today")
                        implicitWidth: root.embedded ? 68 : 88
                        implicitHeight: root.embedded ? 34 : 40
                        onClicked: root.controller.goToday()
                    }
                    Shared.AppButton {
                        text: root.embedded ? "›" : qsTr("Next Year")
                        implicitWidth: root.embedded ? 38 : 110
                        implicitHeight: root.embedded ? 34 : 40
                        Accessible.name: qsTr("Next year")
                        quiet: true
                        onClicked: root.controller.nextYear()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Shared.AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Show exact-date game releases")
                        accessibleLabel: qsTr("Show exact-date game releases in the calendar")
                        checked: root.controller.showGameReleases
                        enabled: root.controller.ready
                                 && root.controller.releaseVisibilityLoaded
                                 && !root.controller.releaseVisibilitySaving
                        onToggled: root.controller.setShowGameReleases(checked)
                    }
                    Label {
                        visible: root.controller.releaseVisibilityError.length > 0
                        Layout.fillWidth: true
                        text: root.controller.releaseVisibilityError
                        color: ThemeModule.Theme.error
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    BusyIndicator {
                        visible: root.controller.markersLoading
                        running: visible
                        implicitWidth: 20
                        implicitHeight: 20
                        Accessible.name: qsTr("Loading calendar activity")
                    }
                    Label {
                        visible: root.controller.markerErrorMessage.length > 0
                        Layout.fillWidth: true
                        text: root.controller.markerErrorMessage
                        color: ThemeModule.Theme.error
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    Label {
                        visible: root.controller.showGameReleases
                                 && root.controller.releaseEventsErrorMessage.length > 0
                        Layout.fillWidth: true
                        text: root.controller.releaseEventsErrorMessage
                        color: ThemeModule.Theme.error
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                }

                GridLayout {
                    id: monthsGrid
                    Layout.fillWidth: true
                    columns: width >= 780 ? 3 : width >= 500 ? 2 : 1
                    columnSpacing: 12
                    rowSpacing: 12

                    Repeater {
                        model: root.controller.months
                        delegate: Shared.AppCard {
                            required property string monthName
                            required property int year
                            required property var days
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: (monthsGrid.width - (monthsGrid.columns - 1) * monthsGrid.columnSpacing) / monthsGrid.columns
                            Layout.alignment: Qt.AlignTop
                            implicitWidth: 0
                            padding: 12
                            ColumnLayout {
                                width: parent.width
                                spacing: 8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        Layout.fillWidth: true
                                        text: monthName
                                        color: ThemeModule.Theme.textPrimary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                    }
                                    Label {
                                        text: String(year)
                                        color: ThemeModule.Theme.textSecondary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.pixelSize: 11
                                    }
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 7
                                    columnSpacing: 2
                                    rowSpacing: 3
                                    Repeater {
                                        model: [qsTr("Mon"), qsTr("Tue"), qsTr("Wed"),
                                                qsTr("Thu"), qsTr("Fri"), qsTr("Sat"), qsTr("Sun")]
                                        delegate: Label {
                                            required property string modelData
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            Layout.preferredWidth: 1
                                            text: modelData
                                            color: ThemeModule.Theme.textSecondary
                                            font.family: ThemeModule.Theme.fontFamily
                                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                                            horizontalAlignment: Text.AlignHCenter
                                        }
                                    }
                                    Repeater {
                                        model: days
                                        delegate: Item {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            Layout.preferredWidth: 1
                                            Layout.preferredHeight: root.embedded ? 36 : 42
                                            Button {
                                                anchors.fill: parent
                                                visible: Number(modelData.day) > 0
                                                enabled: visible
                                                text: visible ? String(modelData.day) : ""
                                                Accessible.name: visible ? root.dateAccessibleName(modelData) : ""
                                                onClicked: root.controller.selectDate(String(modelData.date))
                                                contentItem: Column {
                                                    spacing: 1
                                                    anchors.centerIn: parent
                                                    Text {
                                                        text: parent.parent.text
                                                        color: modelData.selected ? ThemeModule.Theme.textPrimary
                                                               : modelData.today ? ThemeModule.Theme.accentCyan
                                                                                 : ThemeModule.Theme.textSecondary
                                                        font.family: ThemeModule.Theme.fontFamily
                                                        font.pixelSize: 12
                                                        font.weight: modelData.today || modelData.selected
                                                                      ? Font.Bold : Font.Normal
                                                        horizontalAlignment: Text.AlignHCenter
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                    }
                                                    Row {
                                                        spacing: 2
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        Rectangle {
                                                            visible: Number(modelData.deadlineCount) > 0
                                                            width: 5; height: 5; radius: 3
                                                            color: ThemeModule.Theme.warning
                                                        }
                                                        Rectangle {
                                                            visible: Number(modelData.plannedWorkCount) > 0
                                                            width: 5; height: 5; radius: 3
                                                            color: ThemeModule.Theme.accentCyan
                                                        }
                                                        Rectangle {
                                                            visible: Number(modelData.completedTaskCount) > 0
                                                            width: 5; height: 5; radius: 3
                                                            color: ThemeModule.Theme.success
                                                        }
                                                        Rectangle {
                                                            visible: root.controller.showGameReleases
                                                                     && Number(modelData.gameReleaseCount) > 0
                                                            width: 5; height: 5; radius: 3
                                                            color: ThemeModule.Theme.accentViolet
                                                        }
                                                    }
                                                }
                                                background: Rectangle {
                                                    radius: ThemeModule.Theme.controlRadius
                                                    color: parent.down ? ThemeModule.Theme.primaryButtonPressed
                                                           : modelData.selected ? ThemeModule.Theme.selectionBlue
                                                           : parent.hovered ? ThemeModule.Theme.hoverSurface
                                                           : modelData.weekend ? ThemeModule.Theme.raisedSurface : "transparent"
                                                    border.width: modelData.today || parent.activeFocus ? 2 : 1
                                                    border.color: parent.activeFocus ? ThemeModule.Theme.focusHighlight
                                                        : modelData.today ? ThemeModule.Theme.accentCyan
                                                        : modelData.selected ? ThemeModule.Theme.focusHighlight
                                                                             : "transparent"
                                                    Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
                                                    Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14
                    Repeater {
                        model: [
                            { label: qsTr("Deadline"), color: ThemeModule.Theme.warning },
                            { label: qsTr("Planned work"), color: ThemeModule.Theme.accentCyan },
                            { label: qsTr("Completed"), color: ThemeModule.Theme.success },
                            { label: qsTr("Game release"), color: ThemeModule.Theme.accentViolet,
                              gameRelease: true }
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            visible: !Boolean(modelData.gameRelease)
                                     || root.controller.showGameReleases
                            spacing: 5
                            Rectangle {
                                width: 7; height: 7; radius: 4
                                color: modelData.color
                            }
                            Label {
                                text: modelData.label
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            visible: root.controller.selectedDate.length > 0
            padding: 15
            ColumnLayout {
                width: parent.width
                spacing: 9
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Activity for %1").arg(root.controller.selectedDateLabel)
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            text: root.controller.summary
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                        }
                    }
                    BusyIndicator {
                        visible: root.controller.loading
                        running: visible
                        implicitWidth: 26
                        implicitHeight: 26
                        Accessible.name: qsTr("Loading tasks for the selected date")
                    }
                    Shared.AppButton {
                        text: qsTr("Refresh")
                        quiet: true
                        enabled: root.controller.ready && !root.controller.loading
                        onClicked: root.controller.refreshSelectedDate()
                    }
                }
                Label {
                    visible: root.controller.errorMessage.length > 0
                    Layout.fillWidth: true
                    text: root.controller.errorMessage
                    color: ThemeModule.Theme.error
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
                Shared.StatePanel {
                    Layout.fillWidth: true
                    visible: root.controller.tasks.count === 0
                             && (!root.controller.showGameReleases
                                 || root.controller.selectedReleaseEvents.length === 0)
                    state: root.controller.errorMessage.length > 0 ? "error"
                           : root.controller.loading || !root.controller.ready
                             ? "loading" : "empty"
                    title: root.controller.errorMessage.length > 0
                           ? qsTr("Tasks could not be loaded") : qsTr("No deadlines or planned work on this date")
                    message: root.controller.errorMessage.length > 0
                             ? qsTr("The selected date is unchanged. Retry this card when storage is available.")
                             : qsTr("Choose another date above to inspect its deadlines and planned work.")
                }
                Repeater {
                    model: root.controller.showGameReleases
                           ? root.controller.selectedReleaseEvents : []
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: releaseEventContent.implicitHeight + 18
                        radius: ThemeModule.Theme.controlRadius
                        color: ThemeModule.Theme.raisedSurface
                        border.width: 1
                        border.color: ThemeModule.Theme.accentViolet
                        Accessible.name: qsTr("Game release: %1, exact date %2")
                                         .arg(String(modelData.title), String(modelData.exactDate))

                        RowLayout {
                            id: releaseEventContent
                            anchors.fill: parent
                            anchors.margins: 9
                            spacing: 10

                            Rectangle {
                                Layout.fillHeight: true
                                Layout.preferredWidth: 4
                                radius: 2
                                color: ThemeModule.Theme.accentViolet
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: String(modelData.title)
                                    color: ThemeModule.Theme.textPrimary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Game release · exact Steam date: %1")
                                          .arg(String(modelData.exactDate))
                                    color: ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                }
                            }
                            Shared.AppButton {
                                text: qsTr("Open Release Radar")
                                quiet: true
                                Accessible.name: qsTr("Open Release Radar details for %1")
                                                 .arg(String(modelData.title))
                                onClicked: root.releaseRequested(modelData.appId)
                            }
                        }
                    }
                }

                Repeater {
                    model: root.controller.tasks
                    delegate: Shared.TaskSummaryCard {
                        property var calendarSchedule: root.controller.selectedSchedules[String(model.taskId)] || ({})
                        Layout.fillWidth: true
                        taskController: root.taskActions
                        taskId: model.taskId
                        title: model.title
                        description: model.description
                        completed: model.completed
                        dueDate: model.dueDate
                        todayDate: root.controller.todayDate
                        priority: model.priority
                        difficulty: model.difficulty
                        revision: model.revision
                        tags: model.tags
                        subtasks: model.subtasks
                        showCalendarMetadata: true
                        deadlineOnSelectedDate: String(model.dueDate || "") === root.controller.selectedDate
                        scheduleMetadata: calendarSchedule
                        plannedWorkOnSelectedDate: String(calendarSchedule.plannedDate || "")
                                                     === root.controller.selectedDate
                        onOpenTaskRequested: function(task) { taskEditor.openForTask(task) }
                    }
                }
            }
        }
    }

    Dialogs.TaskEditorDialog {
        id: taskEditor
        taskController: root.taskActions
    }
}
