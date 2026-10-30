import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Something.App as ThemeModule

Rectangle {
    id: root

    property var taskController
    property double taskId: 0
    property string title: ""
    property string description: ""
    property bool completed: false
    property string dueDate: ""
    property string todayDate: ""
    property string priority: "medium"
    property string difficulty: ""
    property double revision: 0
    property var tags: []
    property var subtasks: []
    property bool showCalendarMetadata: false
    property bool deadlineOnSelectedDate: false
    property var scheduleMetadata: ({})
    property bool plannedWorkOnSelectedDate: false

    signal openTaskRequested(var task)

    readonly property string plannedDate: String(scheduleMetadata.plannedDate || "")
    readonly property string plannedStartTime: String(scheduleMetadata.plannedStartTime || "")
    readonly property int estimatedDurationMinutes: Number(scheduleMetadata.estimatedDurationMinutes || 0)

    function taskDetails() {
        return {
            id: root.taskId,
            revision: root.revision,
            title: root.title,
            description: root.description,
            dueDate: root.dueDate,
            priority: root.priority,
            difficulty: root.difficulty,
            tags: root.tags,
            subtasks: root.subtasks
        }
    }

    implicitHeight: cardColumn.implicitHeight + 20
    radius: ThemeModule.Theme.panelRadius
    color: ThemeModule.Theme.raisedSurface
    border.color: ThemeModule.Theme.borderColor
    border.width: 1

    ColumnLayout {
        id: cardColumn
        anchors.fill: parent
        anchors.margins: 10
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ThemeModule.AppCheckBox {
                checked: root.completed
                enabled: taskController.ready && !taskController.mutationBusy
                Accessible.name: checked ? qsTr("Mark %1 incomplete").arg(root.title)
                                         : qsTr("Mark %1 complete").arg(root.title)
                onClicked: taskController.toggleTaskFromView(
                    root.taskId, root.revision, checked)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: root.title
                    color: root.completed ? ThemeModule.Theme.textSecondary : ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    font.strikeout: root.completed
                    wrapMode: Text.Wrap
                    Accessible.role: root.showCalendarMetadata ? Accessible.Button : Accessible.StaticText
                    TapHandler {
                        enabled: root.showCalendarMetadata
                        acceptedButtons: Qt.LeftButton
                        onTapped: root.openTaskRequested(root.taskDetails())
                    }
                }
                Label {
                    visible: root.description.length > 0
                    Layout.fillWidth: true
                    text: root.description
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }
            }
            Column {
                spacing: 4
                Label {
                    text: root.priority.charAt(0).toUpperCase() + root.priority.slice(1)
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                    padding: 5
                    background: Rectangle {
                        radius: ThemeModule.Theme.badgeRadius
                        color: root.priority === "high" ? ThemeModule.Theme.destructiveSurface
                               : root.priority === "low" ? Qt.alpha(ThemeModule.Theme.success, 0.18)
                               : ThemeModule.Theme.hoverSurface
                    }
                }
                Label {
                    visible: root.difficulty.length > 0
                    text: root.difficulty.charAt(0).toUpperCase() + root.difficulty.slice(1)
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                    horizontalAlignment: Text.AlignRight
                }
            }
        }

        Label {
            visible: root.dueDate.length > 0
            text: root.dueDate < root.todayDate ? qsTr("Overdue · %1").arg(root.dueDate)
                                                : qsTr("Due · %1").arg(root.dueDate)
            color: root.dueDate < root.todayDate && !root.completed
                   ? ThemeModule.Theme.error : ThemeModule.Theme.textSecondary
            font.family: ThemeModule.Theme.fontFamily
            font.pixelSize: 11
            Layout.leftMargin: 44
        }

        Flow {
            visible: root.showCalendarMetadata
            Layout.fillWidth: true
            Layout.leftMargin: 44
            spacing: 6

            ThemeModule.StatusBadge {
                visible: root.deadlineOnSelectedDate
                text: qsTr("Deadline · %1").arg(root.dueDate)
                badgeColor: ThemeModule.Theme.warning
            }
            ThemeModule.StatusBadge {
                visible: root.plannedDate.length > 0
                text: qsTr("Planned work · %1%2")
                    .arg(root.plannedDate)
                    .arg(root.plannedStartTime.length > 0 ? " · " + root.plannedStartTime : "")
                badgeColor: ThemeModule.Theme.accentCyan
            }
            ThemeModule.StatusBadge {
                visible: root.showCalendarMetadata && root.completed
                text: qsTr("Completed")
                badgeColor: ThemeModule.Theme.success
            }
            ThemeModule.StatusBadge {
                visible: root.showCalendarMetadata && root.plannedDate.length > 0
                         && root.dueDate.length > 0 && root.plannedDate > root.dueDate
                text: qsTr("Planned after deadline")
                badgeColor: ThemeModule.Theme.error
            }
            Label {
                visible: root.plannedDate.length > 0 && root.estimatedDurationMinutes > 0
                text: qsTr("%1 min estimate").arg(root.estimatedDurationMinutes)
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: ThemeModule.Theme.metadataFontSize
                verticalAlignment: Text.AlignVCenter
            }
        }

        Flow {
            visible: root.tags.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 44
            spacing: 5
            Repeater {
                model: root.tags
                delegate: Label {
                    required property string modelData
                    text: modelData
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                    leftPadding: 7
                    rightPadding: 7
                    topPadding: 3
                    bottomPadding: 3
                    background: Rectangle {
                        radius: ThemeModule.Theme.badgeRadius
                        color: Qt.alpha(ThemeModule.Theme.accentCyan, 0.12)
                        border.color: Qt.alpha(ThemeModule.Theme.accentCyan, 0.4)
                    }
                }
            }
        }

        ColumnLayout {
            visible: root.difficulty === "hard" && root.subtasks.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 40
            spacing: 2
            Repeater {
                model: root.subtasks
                delegate: ThemeModule.AppCheckBox {
                    required property var modelData
                    Layout.fillWidth: true
                    checked: Boolean(modelData.completed)
                    enabled: taskController.ready && !taskController.mutationBusy
                    text: String(modelData.title || "")
                    Accessible.name: qsTr("Subtask: %1").arg(text)
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 11
                    onClicked: taskController.toggleSubtaskFromView(
                        root.taskId, Number(modelData.id), root.revision, checked)
                }
            }
        }

        RowLayout {
            visible: root.showCalendarMetadata
            Layout.fillWidth: true
            Layout.leftMargin: 44
            ThemeModule.AppButton {
                text: qsTr("Open details")
                quiet: true
                implicitHeight: 32
                onClicked: root.openTaskRequested(root.taskDetails())
            }
            Item { Layout.fillWidth: true }
        }
    }
}
