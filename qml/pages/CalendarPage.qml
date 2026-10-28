import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root

    property var controller: calendarController
    property var taskActions: taskController
    property bool embedded: false
    implicitHeight: calendarColumn.implicitHeight

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
                            color: ThemeModule.Theme.text
                            font.family: ThemeModule.Theme.displayFont
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            horizontalAlignment: root.embedded ? Text.AlignLeft : Text.AlignHCenter
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.controller.rangeLabel
                            color: ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
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
                                        color: ThemeModule.Theme.text
                                        font.family: ThemeModule.Theme.displayFont
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                    }
                                    Label {
                                        text: String(year)
                                        color: ThemeModule.Theme.textDim
                                        font.family: ThemeModule.Theme.bodyFont
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
                                            color: ThemeModule.Theme.textDim
                                            font.family: ThemeModule.Theme.bodyFont
                                            font.pixelSize: 9
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
                                            Layout.preferredHeight: root.embedded ? 30 : 36
                                            Button {
                                                anchors.fill: parent
                                                visible: Number(modelData.day) > 0
                                                enabled: visible
                                                text: visible ? String(modelData.day) : ""
                                                Accessible.name: visible
                                                    ? (Boolean(modelData.selected)
                                                       ? qsTr("Selected date, %1").arg(modelData.date)
                                                       : qsTr("Show tasks for %1").arg(modelData.date)) : ""
                                                onClicked: root.controller.selectDate(String(modelData.date))
                                                contentItem: Text {
                                                    text: parent.text
                                                    color: modelData.selected ? ThemeModule.Theme.text
                                                           : modelData.today ? ThemeModule.Theme.cyan
                                                                             : ThemeModule.Theme.textDim
                                                    font.family: ThemeModule.Theme.bodyFont
                                                    font.pixelSize: 12
                                                    font.weight: modelData.today || modelData.selected
                                                                  ? Font.Bold : Font.Normal
                                                    horizontalAlignment: Text.AlignHCenter
                                                    verticalAlignment: Text.AlignVCenter
                                                }
                                                background: Rectangle {
                                                    radius: 8
                                                    color: modelData.selected ? ThemeModule.Theme.accent
                                                           : parent.down ? "#2b3048"
                                                           : modelData.weekend ? "#151827" : "transparent"
                                                    border.width: modelData.today || parent.activeFocus ? 2 : 1
                                                    border.color: parent.activeFocus ? ThemeModule.Theme.cyan
                                                        : modelData.today ? ThemeModule.Theme.cyan
                                                        : modelData.selected ? ThemeModule.Theme.accentHover
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
                            text: qsTr("Tasks for %1").arg(root.controller.selectedDateLabel)
                            color: ThemeModule.Theme.text
                            font.family: ThemeModule.Theme.displayFont
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            text: root.controller.summary
                            color: ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
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
                    state: root.controller.errorMessage.length > 0 ? "error"
                           : root.controller.loading || !root.controller.ready
                             ? "loading" : "empty"
                    title: root.controller.errorMessage.length > 0
                           ? qsTr("Tasks could not be loaded") : qsTr("No tasks are due on this date")
                    message: root.controller.errorMessage.length > 0
                             ? qsTr("The selected date is unchanged. Retry this card when storage is available.")
                             : qsTr("Choose another date above to inspect its tasks.")
                }
                Repeater {
                    model: root.controller.tasks
                    delegate: Shared.TaskSummaryCard {
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
                    }
                }
            }
        }
    }
}
