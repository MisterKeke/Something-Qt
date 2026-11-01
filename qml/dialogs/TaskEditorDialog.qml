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
    property bool automationExpanded: false
    property bool automationDetailsLoaded: true
    property bool automationConfigured: false
    property bool automationEnabled: false
    property string automationActionType: "launch_application"
    property double automationTargetId: 0
    property string automationUrl: ""
    property string automationMessage: "Task completed: {task.title}"
    property int automationDelayMs: 0
    property var automationStatus: ({})
    property string automationLoadError: ""
    onAutomationActionTypeChanged: Qt.callLater(function() {
        if (automationActionBox) root.setComboValue(automationActionBox, root.automationActionType)
    })
    onAutomationDelayMsChanged: Qt.callLater(function() {
        if (automationDelayBox) root.setComboValue(automationDelayBox, root.automationDelayMs)
    })

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

    function setModelComboValue(combo, value) {
        for (var i = 0; i < combo.count; ++i) {
            if (String(combo.valueAt(i)) === String(value)) {
                combo.currentIndex = i
                return true
            }
        }
        return false
    }

    function resetAutomation() {
        automationDetailsLoaded = true
        automationConfigured = false
        automationEnabled = false
        automationActionType = "launch_application"
        automationTargetId = 0
        automationUrl = ""
        automationMessage = "Task completed: {task.title}"
        automationDelayMs = 0
        automationStatus = ({})
        automationLoadError = ""
    }

    function applyAutomation(value) {
        const automation = value || ({})
        automationDetailsLoaded = true
        automationConfigured = Boolean(automation.configured)
        automationEnabled = Boolean(automation.enabled)
        automationActionType = String(automation.actionType || "launch_application")
        automationTargetId = Number(automation.targetId || 0)
        automationUrl = String(automation.url || "")
        automationMessage = String(automation.notificationMessage || "Task completed: {task.title}")
        automationDelayMs = Number(automation.delayMs || 0)
        automationStatus = automation
        automationLoadError = ""
        Qt.callLater(root.syncAutomationTargetSelection)
    }

    function syncAutomationTargetSelection() {
        if (automationActionType === "launch_application")
            setModelComboValue(applicationTargetBox, automationTargetId)
        else if (automationActionType === "launch_setup")
            setModelComboValue(setupTargetBox, automationTargetId)
    }

    function loadAutomationTargets() {
        desktopApplicationsController.loadApplicationTargets()
        setupsController.reload()
        Qt.callLater(root.syncAutomationTargetSelection)
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
        resetAutomation()
        loadAutomationTargets()
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
        if (task.automation !== undefined) {
            applyAutomation(task.automation)
        } else {
            automationDetailsLoaded = false
            automationLoadError = ""
            automationStatus = ({})
            taskController.loadTaskForEditor(taskId)
        }
        loadAutomationTargets()
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
        var result = {
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
        if (automationDetailsLoaded) {
            result.automation = {
                configured: automationConfigured,
                enabled: automationEnabled,
                triggerType: "task_completed",
                actionType: automationActionType,
                targetId: automationTargetId,
                url: automationUrl,
                notificationMessage: automationMessage,
                delayMs: automationDelayMs
            }
        }
        return result
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
        applyAutomation(task.automation)
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
        function onEditorTaskLoaded(task) {
            if (Number(task.id) === root.taskId)
                root.applyAutomation(task.automation)
        }
        function onEditorTaskLoadFailed(taskId, message) {
            if (Number(taskId) !== root.taskId) return
            root.automationDetailsLoaded = false
            root.automationLoadError = String(message || qsTr("Automation settings could not be loaded."))
        }
    }

    Connections {
        target: automationRulesController
        function onExecutionChanged(taskId, status, errorMessage, executedAtUtc) {
            if (Number(taskId) <= 0) return
            if (Number(taskId) !== root.taskId) return
            var updated = Object.assign({}, root.automationStatus)
            updated.latestExecutionStatus = String(status)
            updated.latestExecutionError = String(errorMessage || "")
            updated.latestExecutionAt = String(executedAtUtc || "")
            root.automationStatus = updated
        }
    }

    Connections {
        target: desktopApplicationsController.applications
        function onCountChanged() { root.syncAutomationTargetSelection() }
    }

    Connections {
        target: setupsController.setups
        function onCountChanged() { root.syncAutomationTargetSelection() }
    }

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            width: parent.width
            spacing: 11

            Label {
                text: qsTr("A title is required. Dates use YYYY-MM-DD and may be left blank.")
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Label { text: qsTr("Title"); color: ThemeModule.Theme.textPrimary; font.bold: true }
            Shared.AppTextField {
                id: titleField
                Layout.fillWidth: true
                placeholderText: qsTr("Task title")
                accessibleLabel: qsTr("Task title")
                Keys.onReturnPressed: root.saveDraft()
            }

            Label { text: qsTr("Description"); color: ThemeModule.Theme.textPrimary; font.bold: true }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
                TextArea {
                    id: descriptionField
                    placeholderText: qsTr("Optional details")
                    hoverEnabled: true
                    color: ThemeModule.Theme.textPrimary
                    placeholderTextColor: ThemeModule.Theme.textSecondary
                    selectionColor: ThemeModule.Theme.accentCyan
                    selectedTextColor: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Task description")
                    background: Rectangle {
                        color: descriptionField.hovered && !descriptionField.activeFocus
                               ? ThemeModule.Theme.raisedSurface : ThemeModule.Theme.insetSurface
                        radius: ThemeModule.Theme.controlRadius
                        border.width: descriptionField.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
                        border.color: descriptionField.activeFocus
                                      ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.borderColor
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
                    Label { text: qsTr("Due date"); color: ThemeModule.Theme.textPrimary; font.bold: true }
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
                    Label { text: qsTr("Priority"); color: ThemeModule.Theme.textPrimary; font.bold: true }
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
                    Label { text: qsTr("Difficulty"); color: ThemeModule.Theme.textPrimary; font.bold: true }
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
                    Label { text: qsTr("Tags"); color: ThemeModule.Theme.textPrimary; font.bold: true }
                    Shared.AppTextField {
                        id: tagsField
                        Layout.fillWidth: true
                        placeholderText: qsTr("work, urgent")
                        accessibleLabel: qsTr("Task tags, comma separated")
                    }
                    Label {
                        text: qsTr("Up to 32 tags, 64 characters each")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: ThemeModule.Theme.metadataFontSize
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
                        color: ThemeModule.Theme.textPrimary
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

                        ThemeModule.AppCheckBox {
                            checked: Boolean(modelData.completed)
                            Accessible.name: qsTr("Subtask complete")
                            onClicked: root.updateSubtask(index, { completed: checked })
                            indicator: Rectangle {
                                implicitWidth: 20
                                implicitHeight: 20
                                radius: ThemeModule.Theme.controlRadius
                                color: parent.checked ? ThemeModule.Theme.selectionBlue : ThemeModule.Theme.insetSurface
                                border.color: parent.activeFocus ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.borderColor
                                border.width: parent.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
                                Text {
                                    anchors.centerIn: parent
                                    text: parent.parent.checked ? "✓" : ""
                                    color: ThemeModule.Theme.textPrimary
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
                    color: ThemeModule.Theme.textSecondary
                    font.pixelSize: 12
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: automationColumn.implicitHeight + 18
                color: ThemeModule.Theme.insetSurface
                radius: ThemeModule.Theme.panelRadius
                border.color: ThemeModule.Theme.borderColor
                border.width: ThemeModule.Theme.panelBorderWidth

                ColumnLayout {
                    id: automationColumn
                    anchors.fill: parent
                    anchors.margins: 9
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        ToolButton {
                            Layout.fillWidth: true
                            text: (root.automationExpanded ? "▾  " : "▸  ") + qsTr("Automation")
                            Accessible.name: qsTr("Expand or collapse task automation settings")
                            onClicked: root.automationExpanded = !root.automationExpanded
                            contentItem: Label {
                                text: parent.text
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Shared.AppButton {
                            visible: root.automationConfigured
                            text: qsTr("Remove automation")
                            quiet: true
                            onClicked: root.resetAutomation()
                        }
                    }

                    ColumnLayout {
                        visible: root.automationExpanded
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            visible: !root.automationDetailsLoaded
                            Layout.fillWidth: true
                            text: root.automationLoadError.length > 0
                                  ? root.automationLoadError : qsTr("Loading automation settings…")
                            color: root.automationLoadError.length > 0
                                   ? ThemeModule.Theme.warning : ThemeModule.Theme.textSecondary
                            wrapMode: Text.Wrap
                        }

                        ThemeModule.AppCheckBox {
                            text: qsTr("Enable automation")
                            checked: root.automationEnabled
                            enabled: root.automationDetailsLoaded
                            Accessible.name: qsTr("Enable automation when this task is completed")
                            onToggled: {
                                root.automationEnabled = checked
                                if (checked) root.automationConfigured = true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("When this task becomes completed")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 12
                        }

                        Label { text: qsTr("Action"); color: ThemeModule.Theme.textPrimary; font.bold: true }
                        Shared.AppComboBox {
                            id: automationActionBox
                            Layout.fillWidth: true
                            enabled: root.automationDetailsLoaded && root.automationEnabled
                            accessibleLabel: qsTr("Automation action")
                            textRole: "label"
                            valueRole: "value"
                            model: [
                                { label: qsTr("Launch an application"), value: "launch_application" },
                                { label: qsTr("Launch a Setup"), value: "launch_setup" },
                                { label: qsTr("Open a URL"), value: "open_url" },
                                { label: qsTr("Show a notification"), value: "show_notification" }
                            ]
                            Component.onCompleted: root.setComboValue(automationActionBox, root.automationActionType)
                            onActivated: {
                                root.automationActionType = String(currentValue)
                                root.automationTargetId = 0
                                if (root.automationActionType !== "open_url")
                                    root.automationUrl = ""
                                Qt.callLater(root.syncAutomationTargetSelection)
                            }
                        }

                        Label { text: qsTr("Action target"); color: ThemeModule.Theme.textPrimary; font.bold: true }
                        Shared.AppComboBox {
                            id: applicationTargetBox
                            Layout.fillWidth: true
                            visible: root.automationActionType === "launch_application"
                            enabled: root.automationDetailsLoaded && root.automationEnabled
                            accessibleLabel: qsTr("Saved application to launch")
                            textRole: "displayName"
                            valueRole: "applicationId"
                            model: desktopApplicationsController.applications
                            onActivated: root.automationTargetId = Number(currentValue)
                            delegate: ItemDelegate {
                                required property int index
                                required property string displayName
                                required property string executablePath
                                required property string iconSource
                                width: applicationTargetBox.width - 8
                                highlighted: applicationTargetBox.highlightedIndex === index
                                contentItem: RowLayout {
                                    spacing: 8
                                    Image {
                                        Layout.preferredWidth: 28
                                        Layout.preferredHeight: 28
                                        source: iconSource
                                        fillMode: Image.PreserveAspectFit
                                        visible: source.toString().length > 0
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label {
                                            Layout.fillWidth: true
                                            text: displayName
                                            color: ThemeModule.Theme.textPrimary
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: String(executablePath).split(/[\\/]/).pop()
                                            color: ThemeModule.Theme.textSecondary
                                            font.pixelSize: 10
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }
                            }
                        }
                        Label {
                            visible: root.automationConfigured
                                     && root.automationActionType === "launch_application"
                                     && root.automationTargetId > 0
                                     && applicationTargetBox.currentIndex < 0
                            text: qsTr("Selected application is no longer available. Select a replacement.")
                            color: ThemeModule.Theme.warning
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        Shared.AppComboBox {
                            id: setupTargetBox
                            Layout.fillWidth: true
                            visible: root.automationActionType === "launch_setup"
                            enabled: root.automationDetailsLoaded && root.automationEnabled
                            accessibleLabel: qsTr("Setup to launch")
                            textRole: "name"
                            valueRole: "setupId"
                            model: setupsController.setups
                            onActivated: root.automationTargetId = Number(currentValue)
                            delegate: ItemDelegate {
                                required property int index
                                required property string name
                                required property string iconSource
                                width: setupTargetBox.width - 8
                                highlighted: setupTargetBox.highlightedIndex === index
                                contentItem: RowLayout {
                                    spacing: 8
                                    Image {
                                        Layout.preferredWidth: 28
                                        Layout.preferredHeight: 28
                                        source: iconSource
                                        fillMode: Image.PreserveAspectFit
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: name
                                        color: ThemeModule.Theme.textPrimary
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                        Label {
                            visible: root.automationConfigured
                                     && root.automationActionType === "launch_setup"
                                     && root.automationTargetId > 0
                                     && setupTargetBox.currentIndex < 0
                            text: qsTr("Selected Setup is no longer available. Select a replacement.")
                            color: ThemeModule.Theme.warning
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        Shared.AppTextField {
                            Layout.fillWidth: true
                            visible: root.automationActionType === "open_url"
                            enabled: root.automationDetailsLoaded && root.automationEnabled
                            text: root.automationUrl
                            placeholderText: qsTr("https://example.com")
                            accessibleLabel: qsTr("HTTP or HTTPS URL to open")
                            onTextEdited: root.automationUrl = text
                        }

                        ColumnLayout {
                            visible: root.automationActionType === "show_notification"
                            Layout.fillWidth: true
                            Label { text: qsTr("Notification message"); color: ThemeModule.Theme.textPrimary; font.bold: true }
                            TextArea {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 72
                                enabled: root.automationDetailsLoaded && root.automationEnabled
                                text: root.automationMessage
                                placeholderText: qsTr("Task completed: {task.title}")
                                Accessible.name: qsTr("Custom task completion notification message")
                                color: ThemeModule.Theme.textPrimary
                                placeholderTextColor: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                wrapMode: TextEdit.Wrap
                                background: Rectangle {
                                    color: parent.enabled ? ThemeModule.Theme.raisedSurface : ThemeModule.Theme.disabledSurface
                                    radius: ThemeModule.Theme.controlRadius
                                    border.color: ThemeModule.Theme.borderColor
                                    border.width: ThemeModule.Theme.panelBorderWidth
                                }
                                onTextChanged: if (activeFocus) root.automationMessage = text
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Supported placeholders: {task.title}, {task.id}, {task.completedAt}")
                                color: ThemeModule.Theme.textSecondary
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }

                        Label { text: qsTr("Delay"); color: ThemeModule.Theme.textPrimary; font.bold: true }
                        Shared.AppComboBox {
                            id: automationDelayBox
                            Layout.fillWidth: true
                            enabled: root.automationDetailsLoaded && root.automationEnabled
                            accessibleLabel: qsTr("Automation delay")
                            textRole: "label"
                            valueRole: "value"
                            model: [
                                { label: qsTr("Immediately"), value: 0 },
                                { label: qsTr("5 seconds"), value: 5000 },
                                { label: qsTr("30 seconds"), value: 30000 }
                            ]
                            Component.onCompleted: root.setComboValue(automationDelayBox, root.automationDelayMs)
                            onActivated: root.automationDelayMs = Number(currentValue)
                        }

                        Label {
                            visible: root.automationStatus.latestExecutionStatus !== undefined
                                     && String(root.automationStatus.latestExecutionStatus).length > 0
                            Layout.fillWidth: true
                            text: {
                                const status = String(root.automationStatus.latestExecutionStatus || "")
                                if (status === "succeeded"
                                        && (root.automationActionType === "launch_application"
                                            || root.automationActionType === "launch_setup"))
                                    return qsTr("Latest execution: Succeeded (the operating system accepted the launch request)")
                                return qsTr("Latest execution: %1").arg(status)
                            }
                            color: root.automationStatus.latestExecutionStatus === "failed"
                                   ? ThemeModule.Theme.error
                                   : root.automationStatus.latestExecutionStatus === "succeeded"
                                     ? ThemeModule.Theme.success : ThemeModule.Theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        Label {
                            visible: String(root.automationStatus.latestExecutionError || "").length > 0
                            Layout.fillWidth: true
                            text: String(root.automationStatus.latestExecutionError || "")
                            color: ThemeModule.Theme.error
                            wrapMode: Text.Wrap
                        }
                        Label {
                            visible: root.automationConfigured
                                     && (!root.automationStatus.latestExecutionStatus
                                         || String(root.automationStatus.latestExecutionStatus).length === 0)
                            Layout.fillWidth: true
                            text: qsTr("No executions yet.")
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 11
                        }
                    }
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
        color: ThemeModule.Theme.raisedSurface
        border.color: ThemeModule.Theme.accentCyan
        border.width: 1
        radius: ThemeModule.Theme.panelRadius
    }
}
