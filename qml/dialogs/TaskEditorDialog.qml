import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Dialog {
    id: root

    property var taskController
    property bool editing: false
    property double taskId: 0
    property double taskRevision: 0
    property var subtaskDrafts: []
    property string pendingDifficulty: ""
    property int previousDifficultyIndex: 0
    property var latestConflictTask: ({})

    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(680, parent ? parent.width - 32 : 640)
    height: Math.min(760, parent ? parent.height - 36 : 700)
    padding: 20
    closePolicy: taskController.editorSaving
                 ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    title: editing ? qsTr("Edit task") : qsTr("Add task")
    Accessible.name: title

    function comboValue(combo) {
        return combo.currentValue === undefined ? "" : String(combo.currentValue)
    }

    function setComboValue(combo, value) {
        for (var i = 0; i < combo.model.length; ++i) {
            if (String(combo.model[i].value) === String(value)) {
                combo.currentIndex = i
                return
            }
        }
    }

    function openNew() {
        editing = false
        taskId = 0
        taskRevision = 0
        titleField.text = ""
        descriptionField.text = ""
        dueDateField.text = ""
        priorityBox.currentIndex = 1
        difficultyBox.currentIndex = 0
        previousDifficultyIndex = difficultyBox.currentIndex
        tagsField.text = ""
        subtaskDrafts = []
        latestConflictTask = ({})
        taskController.clearEditorState()
        open()
        Qt.callLater(function() { titleField.forceActiveFocus() })
    }

    function openForTask(task) {
        editing = true
        taskId = Number(task.taskId !== undefined ? task.taskId : task.id)
        taskRevision = Number(task.revision)
        titleField.text = String(task.title || "")
        descriptionField.text = String(task.description || "")
        dueDateField.text = String(task.dueDate || "")
        setComboValue(priorityBox, task.priority || "medium")
        setComboValue(difficultyBox, task.difficulty || "")
        previousDifficultyIndex = difficultyBox.currentIndex
        tagsField.text = (task.tags || []).join(", ")
        subtaskDrafts = (task.subtasks || []).map(function(subtask) {
            return { id: Number(subtask.id || 0), title: String(subtask.title || ""),
                     completed: Boolean(subtask.completed) }
        })
        latestConflictTask = ({})
        taskController.clearEditorState()
        open()
        Qt.callLater(function() { titleField.forceActiveFocus() })
    }

    function draftMap() {
        var parsedTags = []
        var seen = new Set()
        String(tagsField.text || "").split(",").forEach(function(raw) {
            var tag = raw.trim()
            var key = tag.toLowerCase()
            if (tag.length > 0 && !seen.has(key)) {
                seen.add(key)
                parsedTags.push(tag)
            }
        })
        return {
            id: editing ? taskId : 0,
            revision: taskRevision,
            title: titleField.text,
            description: descriptionField.text,
            dueDate: dueDateField.text.trim(),
            priority: comboValue(priorityBox),
            difficulty: comboValue(difficultyBox),
            tags: parsedTags,
            subtasks: subtaskDrafts.map(function(subtask) {
                return { id: Number(subtask.id || 0), title: String(subtask.title || ""),
                         completed: Boolean(subtask.completed) }
            })
        }
    }

    function saveDraft() {
        if (root.taskController.mutationBusy) return
        root.taskController.saveTask(draftMap())
    }

    function addSubtask() {
        if (subtaskDrafts.length >= 100) return
        var next = subtaskDrafts.slice()
        next.push({ id: 0, title: "", completed: false })
        subtaskDrafts = next
    }

    function updateSubtask(index, patch) {
        var next = subtaskDrafts.slice()
        var row = Object.assign({}, next[index], patch)
        next[index] = row
        subtaskDrafts = next
    }

    function applyLatestTask(task) {
        taskId = Number(task.id)
        taskRevision = Number(task.revision)
        titleField.text = String(task.title || "")
        descriptionField.text = String(task.description || "")
        dueDateField.text = String(task.dueDate || "")
        setComboValue(priorityBox, task.priority || "medium")
        setComboValue(difficultyBox, task.difficulty || "")
        previousDifficultyIndex = difficultyBox.currentIndex
        tagsField.text = (task.tags || []).join(", ")
        subtaskDrafts = (task.subtasks || []).map(function(subtask) {
            return { id: Number(subtask.id || 0), title: String(subtask.title || ""),
                     completed: Boolean(subtask.completed) }
        })
        latestConflictTask = ({})
        taskController.clearEditorState()
    }

    onClosed: {
        latestConflictTask = ({})
        taskController.clearEditorState()
    }

    Connections {
        target: root.taskController
        function onEditorSaveSucceeded() {
            root.close()
        }
        function onConflictTaskLoaded(task) {
            root.latestConflictTask = task
        }
    }

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            width: parent.width
            spacing: 11

            Label {
                text: qsTr("A title is required. Dates use YYYY-MM-DD and may be left blank.")
                color: ThemeModule.Theme.textDim
                font.family: ThemeModule.Theme.bodyFont
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Label { text: qsTr("Title"); color: ThemeModule.Theme.text; font.bold: true }
            Shared.AppTextField {
                id: titleField
                Layout.fillWidth: true
                placeholderText: qsTr("Task title")
                accessibleLabel: qsTr("Task title")
                Keys.onReturnPressed: root.saveDraft()
            }

            Label { text: qsTr("Description"); color: ThemeModule.Theme.text; font.bold: true }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                TextArea {
                    id: descriptionField
                    placeholderText: qsTr("Optional details")
                    color: ThemeModule.Theme.text
                    placeholderTextColor: ThemeModule.Theme.textDim
                    selectionColor: ThemeModule.Theme.accent
                    selectedTextColor: ThemeModule.Theme.text
                    font.family: ThemeModule.Theme.bodyFont
                    font.pixelSize: 13
                    wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Task description")
                    background: Rectangle {
                        color: "#0d101a"
                        radius: 9
                        border.width: descriptionField.activeFocus ? 2 : 1
                        border.color: descriptionField.activeFocus
                                      ? ThemeModule.Theme.cyan : ThemeModule.Theme.border
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width > 520 ? 2 : 1
                columnSpacing: 12
                rowSpacing: 9

                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Due date"); color: ThemeModule.Theme.text; font.bold: true }
                    Shared.AppTextField {
                        id: dueDateField
                        Layout.fillWidth: true
                        placeholderText: qsTr("YYYY-MM-DD")
                        accessibleLabel: qsTr("Due date, optional, YYYY-MM-DD")
                        maximumLength: 10
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Priority"); color: ThemeModule.Theme.text; font.bold: true }
                    Shared.AppComboBox {
                        id: priorityBox
                        Layout.fillWidth: true
                        accessibleLabel: qsTr("Task priority")
                        textRole: "label"
                        valueRole: "value"
                        model: [
                            { label: qsTr("Low"), value: "low" },
                            { label: qsTr("Medium"), value: "medium" },
                            { label: qsTr("High"), value: "high" }
                        ]
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Difficulty"); color: ThemeModule.Theme.text; font.bold: true }
                    Shared.AppComboBox {
                        id: difficultyBox
                        Layout.fillWidth: true
                        accessibleLabel: qsTr("Task difficulty")
                        textRole: "label"
                        valueRole: "value"
                        model: [
                            { label: qsTr("Not set"), value: "" },
                            { label: qsTr("Easy"), value: "easy" },
                            { label: qsTr("Medium"), value: "medium" },
                            { label: qsTr("Hard"), value: "hard" }
                        ]
                        onActivated: function(index) {
                            const nextDifficulty = String(model[index].value)
                            const currentDifficulty = String(model[previousDifficultyIndex].value)
                            if (nextDifficulty !== "hard" && currentDifficulty === "hard"
                                && root.subtaskDrafts.length > 0) {
                                root.pendingDifficulty = nextDifficulty
                                currentIndex = previousDifficultyIndex
                                root.difficultyConfirmation.open()
                                return
                            }
                            root.previousDifficultyIndex = index
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Tags"); color: ThemeModule.Theme.text; font.bold: true }
                    Shared.AppTextField {
                        id: tagsField
                        Layout.fillWidth: true
                        placeholderText: qsTr("work, urgent")
                        accessibleLabel: qsTr("Task tags, comma separated")
                    }
                    Label {
                        text: qsTr("Up to 32 tags, 64 characters each")
                        color: ThemeModule.Theme.textDim
                        font.family: ThemeModule.Theme.bodyFont
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }

            ColumnLayout {
                visible: root.comboValue(difficultyBox) === "hard"
                Layout.fillWidth: true
                spacing: 7

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Subtasks (%1/100)").arg(root.subtaskDrafts.length)
                        color: ThemeModule.Theme.text
                        font.bold: true
                    }
                    Shared.AppButton {
                        text: qsTr("Add subtask")
                        quiet: true
                        enabled: root.subtaskDrafts.length < 100
                        onClicked: root.addSubtask()
                    }
                }
                Repeater {
                    model: root.subtaskDrafts
                    delegate: RowLayout {
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true

                        CheckBox {
                            checked: Boolean(modelData.completed)
                            Accessible.name: qsTr("Subtask complete")
                            onClicked: root.updateSubtask(index, { completed: checked })
                            indicator: Rectangle {
                                implicitWidth: 20
                                implicitHeight: 20
                                radius: 5
                                color: parent.checked ? ThemeModule.Theme.accent : "#0d101a"
                                border.color: parent.activeFocus ? ThemeModule.Theme.cyan : ThemeModule.Theme.border
                                Text {
                                    anchors.centerIn: parent
                                    text: parent.parent.checked ? "✓" : ""
                                    color: ThemeModule.Theme.text
                                    font.pixelSize: 13
                                }
                            }
                        }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(modelData.title || "")
                            placeholderText: qsTr("Subtask title")
                            accessibleLabel: qsTr("Subtask title")
                            onTextEdited: root.updateSubtask(index, { title: text })
                        }
                        Shared.AppButton {
                            text: qsTr("Remove")
                            quiet: true
                            onClicked: {
                                var next = root.subtaskDrafts.slice()
                                next.splice(index, 1)
                                root.subtaskDrafts = next
                            }
                        }
                    }
                }
                Label {
                    visible: root.subtaskDrafts.length === 0
                    text: qsTr("No subtasks yet.")
                    color: ThemeModule.Theme.textDim
                    font.pixelSize: 12
                }
            }

            Label {
                visible: root.taskController.editorErrorMessage.length > 0
                text: root.taskController.editorErrorMessage
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
            }
            Label {
                visible: root.taskController.editorConflict
                text: root.taskController.conflictReloadMessage.length > 0
                      ? root.taskController.conflictReloadMessage
                      : qsTr("Your draft is still open. You can load the latest version separately.")
                color: ThemeModule.Theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                visible: root.taskController.editorConflict
                Layout.fillWidth: true
                Shared.AppButton {
                    text: root.taskController.conflictReloadAvailable
                          ? qsTr("Use loaded version") : qsTr("Load latest task")
                    quiet: true
                    enabled: !root.taskController.conflictReloading
                    onClicked: {
                        if (root.taskController.conflictReloadAvailable)
                            root.applyLatestTask(root.latestConflictTask)
                        else
                            root.taskController.reloadConflictTask()
                    }
                }
            }
        }
    }

    footer: RowLayout {
        spacing: 9
        Shared.AppButton {
            text: qsTr("Cancel")
            quiet: true
            enabled: !root.taskController.editorSaving
            onClicked: root.reject()
        }
        Item { Layout.fillWidth: true }
        Shared.AppButton {
            text: root.taskController.editorSaving ? qsTr("Saving…") : qsTr("Save task")
            enabled: root.taskController.ready && !root.taskController.mutationBusy
            onClicked: root.saveDraft()
        }
    }

    ConfirmationDialog {
        id: difficultyConfirmation
        title: qsTr("Clear subtasks?")
        message: qsTr("Changing difficulty away from Hard clears all subtasks. Continue?")
        confirmText: qsTr("Clear subtasks")
        destructive: true
        onConfirmed: {
            root.subtaskDrafts = []
            for (var i = 0; i < difficultyBox.model.length; ++i) {
                if (String(difficultyBox.model[i].value) === root.pendingDifficulty) {
                    difficultyBox.currentIndex = i
                    root.previousDifficultyIndex = i
                    break
                }
            }
        }
        onRejected: difficultyBox.currentIndex = root.previousDifficultyIndex
    }

    background: Rectangle {
        color: ThemeModule.Theme.panelAlt
        border.color: ThemeModule.Theme.accent
        border.width: 1
        radius: 16
    }
}
