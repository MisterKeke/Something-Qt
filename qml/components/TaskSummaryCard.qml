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
    }
}
