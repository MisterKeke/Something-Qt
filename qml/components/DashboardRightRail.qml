import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

ColumnLayout {
    id: root
    property var controller: dashboardController
    property var taskActions: taskController
    signal navigateRequested(string routeId)
    spacing: 14

    Shared.AppCard {
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
                    text: qsTr("Today's Tasks")
                    wrapMode: Text.Wrap
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                BusyIndicator {
                    visible: root.controller.todayLoading
                    running: visible
                    implicitWidth: 24
                    implicitHeight: 24
                    Accessible.name: qsTr("Loading today's tasks")
                }
                Shared.AppButton {
                    text: qsTr("Tasks")
                    quiet: true
                    implicitWidth: 68
                    implicitHeight: 30
                    font.pixelSize: 11
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: root.navigateRequested("tasks")
                }
            }
            Label {
                visible: root.controller.todayError.length > 0
                Layout.fillWidth: true
                text: root.controller.todayError
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.overdueTasks.count === 0 && root.controller.todayTasks.count === 0
                text: root.controller.todayError.length > 0 ? qsTr("Try reopening Tasks.")
                    : root.controller.todayLoading || !root.controller.ready ? qsTr("Loading today's tasks…")
                    : qsTr("No incomplete tasks due today.")
                color: ThemeModule.Theme.textSecondary
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Layout.topMargin: 8
                Layout.bottomMargin: 10
            }
            Label {
                visible: root.controller.overdueTasks.count > 0
                text: qsTr("Overdue")
                color: ThemeModule.Theme.error
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Repeater {
                model: root.controller.overdueTasks
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
            Label {
                visible: root.controller.todayTasks.count > 0
                text: qsTr("Due today")
                color: ThemeModule.Theme.accentCyan
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Repeater {
                model: root.controller.todayTasks
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

    Shared.AppCard {
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
                    text: qsTr("This Week's Tasks")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                BusyIndicator {
                    visible: root.controller.weekLoading
                    running: visible
                    implicitWidth: 24
                    implicitHeight: 24
                    Accessible.name: qsTr("Loading this week's tasks")
                }
                Shared.AppButton {
                    text: qsTr("Calendar")
                    quiet: true
                    implicitWidth: 68
                    implicitHeight: 30
                    font.pixelSize: 11
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: root.navigateRequested("calendar")
                }
            }
            Label {
                Layout.fillWidth: true
                text: root.controller.weekRangeLabel
                visible: text.length > 0
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 11
            }
            Label {
                visible: root.controller.weekError.length > 0
                Layout.fillWidth: true
                text: root.controller.weekError
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.weekTasks.count === 0
                text: root.controller.weekError.length > 0 ? qsTr("Try reopening Calendar.")
                    : root.controller.weekLoading || !root.controller.ready ? qsTr("Loading this week's tasks…")
                    : qsTr("No more incomplete tasks due this week.")
                color: ThemeModule.Theme.textSecondary
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Layout.topMargin: 8
                Layout.bottomMargin: 10
            }
            Repeater {
                model: root.controller.weekTasks
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

    Shared.AppCard {
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
                    text: qsTr("Setups")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                Shared.AppButton {
                    text: qsTr("Manage")
                    quiet: true
                    implicitWidth: 68
                    implicitHeight: 30
                    font.pixelSize: 11
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: root.navigateRequested("setups")
                }
            }
            Label {
                Layout.fillWidth: true
                visible: setupsController.setups.count === 0
                text: setupsController.loading ? qsTr("Loading saved setups…")
                    : setupsController.errorMessage.length > 0 ? setupsController.errorMessage
                    : qsTr("Launch your saved application groups. Create your first setup with Manage.")
                color: setupsController.errorMessage.length > 0 ? ThemeModule.Theme.error : ThemeModule.Theme.textSecondary
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Layout.topMargin: 8
                Layout.bottomMargin: 10
            }
            Label {
                Layout.fillWidth: true
                visible: setupsController.setups.count > 0
                         && setupsController.errorMessage.length > 0
                text: setupsController.errorMessage
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            Shared.AppButton {
                visible: setupsController.launching
                text: qsTr("Stop remaining launches")
                quiet: true
                onClicked: setupsController.cancelLaunch()
            }
            Repeater {
                model: setupsController.setups
                delegate: RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: model.name + " · " + model.applicationSummary
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Shared.AppButton {
                        text: qsTr("Start")
                        enabled: !setupsController.busy && model.applicationIds.length > 0
                        onClicked: setupsController.startSetup(model.setupId)
                    }
                }
            }
        }
    }
}
