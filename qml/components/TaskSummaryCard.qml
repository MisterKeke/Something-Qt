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
    radius: 11
    color: ThemeModule.Theme.panelAlt
    border.color: ThemeModule.Theme.border
    border.width: 1

    ColumnLayout {
        id: cardColumn
        anchors.fill: parent
        anchors.margins: 10
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            CheckBox {
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
                    color: root.completed ? ThemeModule.Theme.textDim : ThemeModule.Theme.text
                    font.family: ThemeModule.Theme.displayFont
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    font.strikeout: root.completed
                    wrapMode: Text.Wrap
                }
                Label {
                    visible: root.description.length > 0
                    Layout.fillWidth: true
                    text: root.description
                    color: ThemeModule.Theme.textDim
                    font.family: ThemeModule.Theme.bodyFont
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
                    color: ThemeModule.Theme.text
                    font.family: ThemeModule.Theme.bodyFont
                    font.pixelSize: 10
                    padding: 5
                    background: Rectangle {
                        radius: 7
                        color: root.priority === "high" ? "#4a2335"
                               : root.priority === "low" ? "#203d3a" : "#2d3152"
                    }
                }
                Label {
                    visible: root.difficulty.length > 0
                    text: root.difficulty.charAt(0).toUpperCase() + root.difficulty.slice(1)
                    color: ThemeModule.Theme.textDim
                    font.family: ThemeModule.Theme.bodyFont
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignRight
                }
            }
        }

        Label {
            visible: root.dueDate.length > 0
            text: root.dueDate < root.todayDate ? qsTr("Overdue · %1").arg(root.dueDate)
                                                : qsTr("Due · %1").arg(root.dueDate)
            color: root.dueDate < root.todayDate && !root.completed
                   ? ThemeModule.Theme.error : ThemeModule.Theme.textDim
            font.family: ThemeModule.Theme.bodyFont
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
                    color: ThemeModule.Theme.textDim
                    font.family: ThemeModule.Theme.bodyFont
                    font.pixelSize: 10
                    leftPadding: 7
                    rightPadding: 7
                    topPadding: 3
                    bottomPadding: 3
                    background: Rectangle {
                        radius: 7
                        color: Qt.alpha(ThemeModule.Theme.accent, 0.2)
                        border.color: Qt.alpha(ThemeModule.Theme.accent, 0.5)
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
                delegate: CheckBox {
                    required property var modelData
                    Layout.fillWidth: true
                    checked: Boolean(modelData.completed)
                    enabled: taskController.ready && !taskController.mutationBusy
                    text: String(modelData.title || "")
                    Accessible.name: qsTr("Subtask: %1").arg(text)
                    font.family: ThemeModule.Theme.bodyFont
                    font.pixelSize: 11
                    onClicked: taskController.toggleSubtaskFromView(
                        root.taskId, Number(modelData.id), root.revision, checked)
                }
            }
        }
    }
}
