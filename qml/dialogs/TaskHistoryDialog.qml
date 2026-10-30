import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Dialog {
    id: root

    property var historyController: taskHistoryController
    property double taskId: 0
    property string taskTitle: ""

    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(720, parent ? parent.width - 32 : 680)
    height: Math.min(780, parent ? parent.height - 36 : 720)
    padding: 18
    title: qsTr("Task history")
    Accessible.name: title
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function openForTask(id, titleText) {
        taskId = Number(id)
        taskTitle = String(titleText || "")
        open()
    }

    onOpened: {
        historyController.loadTaskHistory(taskId)
        historyController.setVisible(true)
    }
    onClosed: historyController.setVisible(false)

    contentItem: ColumnLayout {
        spacing: 9

        Label {
            Layout.fillWidth: true
            text: root.taskTitle
            color: ThemeModule.Theme.textSecondary
            font.family: ThemeModule.Theme.fontFamily
            font.pixelSize: 12
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
        RowLayout {
            Layout.fillWidth: true
            BusyIndicator {
                visible: root.historyController.loading
                running: visible
                implicitWidth: 24
                implicitHeight: 24
            }
            Label {
                Layout.fillWidth: true
                text: root.historyController.errorMessage.length > 0
                      ? root.historyController.errorMessage
                      : root.historyController.warningMessage
                visible: root.historyController.errorMessage.length > 0
                         || root.historyController.warningMessage.length > 0
                color: root.historyController.errorMessage.length > 0
                       ? ThemeModule.Theme.error : ThemeModule.Theme.warning
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            Shared.AppButton {
                text: qsTr("Refresh")
                quiet: true
                enabled: root.historyController.ready && !root.historyController.loading
                onClicked: root.historyController.loadTaskHistory(root.taskId)
            }
        }
        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.historyController.model.count === 0
            state: root.historyController.errorMessage.length > 0 ? "error"
                   : root.historyController.loading || !root.historyController.ready ? "loading" : "empty"
            title: root.historyController.errorMessage.length > 0
                   ? qsTr("History could not be loaded")
                   : root.historyController.warningMessage.length > 0
                     ? qsTr("Some history entries could not be read")
                   : root.historyController.ready ? qsTr("No history recorded yet")
                                                  : qsTr("Starting task storage")
            message: root.historyController.errorMessage.length > 0
                     ? qsTr("Retry this timeline after storage is available.")
                     : root.historyController.warningMessage.length > 0
                       ? root.historyController.warningMessage
                     : qsTr("New task changes will appear here.")
        }
        ListView {
            id: timeline
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.historyController.model.count > 0
            clip: true
            spacing: 8
            boundsBehavior: Flickable.StopAtBounds
            model: root.historyController.model
            ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Shared.TaskHistoryEventCard {
                width: ListView.view.width
                eventType: model.eventType
                occurredAtUtc: model.occurredAtUtc
                taskTitle: model.taskTitle
                changes: model.changes
                showTaskTitle: false
            }
        }
        Shared.AppButton {
            Layout.alignment: Qt.AlignHCenter
            visible: root.historyController.hasMore
            text: root.historyController.loading ? qsTr("Loading older events…") : qsTr("Load older events")
            quiet: true
            enabled: !root.historyController.loading
            onClicked: root.historyController.loadOlder()
        }
    }

    footer: DialogButtonBox {
        alignment: Qt.AlignRight
        Shared.AppButton {
            text: qsTr("Close")
            quiet: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    background: Rectangle {
        color: ThemeModule.Theme.raisedSurface
        border.color: ThemeModule.Theme.accentCyan
        border.width: 1
        radius: ThemeModule.Theme.panelRadius
    }
}
