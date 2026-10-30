import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root

    property var controller: taskController
    property var historyController: taskHistoryController
    property var filterState: taskController.filters
    property string activeView: "tasks"
    property var schedulingSettingsDraft: ({})
    property bool schedulingSettingsDirty: false
    property string schedulingSettingsStatus: ""
    readonly property var schedulingWeekdays: [
        { label: qsTr("Mon"), accessibleLabel: qsTr("Monday"), value: 1 },
        { label: qsTr("Tue"), accessibleLabel: qsTr("Tuesday"), value: 2 },
        { label: qsTr("Wed"), accessibleLabel: qsTr("Wednesday"), value: 3 },
        { label: qsTr("Thu"), accessibleLabel: qsTr("Thursday"), value: 4 },
        { label: qsTr("Fri"), accessibleLabel: qsTr("Friday"), value: 5 },
        { label: qsTr("Sat"), accessibleLabel: qsTr("Saturday"), value: 6 },
        { label: qsTr("Sun"), accessibleLabel: qsTr("Sunday"), value: 7 }
    ]
    Component.onCompleted: {
        controller.setPageVisible(visible)
        historyController.setVisible(false)
        refreshControls()
        if (schedulingController.preferencesAvailable) captureSchedulingSettings()
    }
    Component.onDestruction: {
        controller.setPageVisible(false)
        historyController.setVisible(false)
    }
    onVisibleChanged: {
        if (controller) controller.setPageVisible(visible)
        if (historyController) historyController.setVisible(visible && activeView === "history")
    }
    onActiveViewChanged: {
        if (activeView === "history") {
            historyController.loadRecentHistory(historyQueryField.text,
                                                String(historyTypeBox.currentValue || ""))
            historyController.setVisible(visible)
        } else {
            historyController.setVisible(false)
        }
    }
    readonly property var dateModes: [
        { label: qsTr("Any date"), value: "any" },
        { label: qsTr("Today"), value: "today" },
        { label: qsTr("Tomorrow"), value: "tomorrow" },
        { label: qsTr("Next 7 days"), value: "next7" },
        { label: qsTr("Overdue"), value: "overdue" },
        { label: qsTr("Undated"), value: "undated" },
        { label: qsTr("Exact date"), value: "exact" },
        { label: qsTr("Date range"), value: "range" },
    ]
    readonly property var completionModes: [
        { label: qsTr("All completion states"), value: "all" },
        { label: qsTr("Incomplete"), value: "incomplete" },
        { label: qsTr("Completed"), value: "complete" },
    ]
    readonly property var priorityModes: [
        { label: qsTr("All priorities"), value: "" },
        { label: qsTr("Low"), value: "low" },
        { label: qsTr("Medium"), value: "medium" },
        { label: qsTr("High"), value: "high" },
    ]
    readonly property var difficultyModes: [
        { label: qsTr("All difficulties"), value: "" },
        { label: qsTr("Not set"), value: "unset" },
        { label: qsTr("Easy"), value: "easy" },
        { label: qsTr("Medium"), value: "medium" },
        { label: qsTr("Hard"), value: "hard" },
    ]
    readonly property var sortModes: [
        { label: qsTr("Created"), value: "created_at" },
        { label: qsTr("Updated"), value: "updated_at" },
        { label: qsTr("Due date"), value: "due_date" },
        { label: qsTr("Priority"), value: "priority" },
        { label: qsTr("Title"), value: "title" },
    ]
    readonly property var historyEventTypes: [
        { label: qsTr("All event types"), value: "" },
        { label: qsTr("Task created"), value: "task_created" },
        { label: qsTr("Task updated"), value: "task_updated" },
        { label: qsTr("Title changed"), value: "task_title_changed" },
        { label: qsTr("Description changed"), value: "description_changed" },
        { label: qsTr("Due date changed"), value: "due_date_changed" },
        { label: qsTr("Priority changed"), value: "priority_changed" },
        { label: qsTr("Difficulty changed"), value: "difficulty_changed" },
        { label: qsTr("Tags changed"), value: "tags_changed" },
        { label: qsTr("Task completed"), value: "task_completed" },
        { label: qsTr("Task reopened"), value: "task_reopened" },
        { label: qsTr("Subtask created"), value: "subtask_created" },
        { label: qsTr("Subtask updated"), value: "subtask_updated" },
        { label: qsTr("Subtask completed"), value: "subtask_completed" },
        { label: qsTr("Subtask reopened"), value: "subtask_reopened" },
        { label: qsTr("Subtask removed"), value: "subtask_removed" },
        { label: qsTr("Task deleted"), value: "task_deleted" },
    ]

    implicitHeight: pageColumn.implicitHeight

    function indexForValue(items, value) {
        for (var i = 0; i < items.length; ++i)
            if (String(items[i].value) === String(value)) return i
        return 0
    }

    function enforceSearchLimit() {
        const characters = Array.from(queryField.text)
        if (characters.length > 256) {
            queryField.text = characters.slice(0, 256).join("")
            queryField.cursorPosition = queryField.text.length
        }
    }

    function captureViewState() {
        return { taskListY: taskList.contentY }
    }

    function restoreViewState(state) {
        Qt.callLater(function() {
            taskList.contentY = Number(state.taskListY || 0)
        })
    }

    function refreshControls() {
        const state = taskController.filters
        filterState = state
        queryField.text = String(state.query || "")
        dateModeBox.currentIndex = indexForValue(dateModes, state.dateMode || "any")
        exactDateField.text = String(state.exactDate || "")
        dueFromField.text = String(state.dueFrom || "")
        dueToField.text = String(state.dueTo || "")
        completionBox.currentIndex = indexForValue(completionModes, state.completion || "all")
        priorityFilterBox.currentIndex = indexForValue(priorityModes, state.priority || "")
        difficultyFilterBox.currentIndex = indexForValue(difficultyModes, state.difficulty || "")
        tagsFilterField.text = Array.isArray(state.tags) ? state.tags.join(", ") : String(state.tags || "")
        sortBox.currentIndex = indexForValue(sortModes, state.sort || "created_at")
        ascendingCheck.checked = String(state.direction || "desc") === "asc"
    }

    function sendFilters() {
        const next = Object.assign({}, filterState)
        next.query = queryField.text
        next.dateMode = String(dateModeBox.currentValue || "any")
        next.exactDate = exactDateField.text.trim()
        next.dueFrom = dueFromField.text.trim()
        next.dueTo = dueToField.text.trim()
        next.completion = String(completionBox.currentValue || "all")
        next.priority = String(priorityFilterBox.currentValue || "")
        next.difficulty = String(difficultyFilterBox.currentValue || "")
        next.tags = tagsFilterField.text
        next.sort = String(sortBox.currentValue || "created_at")
        next.direction = ascendingCheck.checked ? "asc" : "desc"
        filterState = next
        taskController.setFilters(next)
    }

    function loadGlobalHistory() {
        historyController.loadRecentHistory(historyQueryField.text,
                                           String(historyTypeBox.currentValue || ""))
    }

    function captureSchedulingSettings() {
        if (!schedulingController.preferencesAvailable) return
        const preferences = schedulingController.preferences
        const breakValue = preferences.breakBetweenWorkBlocksMinutes
        schedulingSettingsDraft = {
            preferredWorkingDays: (preferences.preferredWorkingDays || []).map(function(day) {
                return Number(day)
            }),
            workdayStart: String(preferences.workdayStart || ""),
            workdayEnd: String(preferences.workdayEnd || ""),
            maximumPlannedWorkMinutesPerDay: String(preferences.maximumPlannedWorkMinutesPerDay || ""),
            defaultEstimatedDurationMinutes: String(preferences.defaultEstimatedDurationMinutes || ""),
            breakBetweenWorkBlocksMinutes: breakValue === null || breakValue === undefined
                ? "" : String(breakValue)
        }
        schedulingSettingsDirty = false
    }

    function updateSchedulingSetting(name, value) {
        const next = Object.assign({}, schedulingSettingsDraft)
        next[name] = value
        schedulingSettingsDraft = next
        schedulingSettingsDirty = true
        schedulingSettingsStatus = ""
    }

    function toggleSchedulingDay(day) {
        const days = (schedulingSettingsDraft.preferredWorkingDays || []).slice()
        const index = days.indexOf(Number(day))
        if (index >= 0) days.splice(index, 1)
        else days.push(Number(day))
        days.sort(function(left, right) { return left - right })
        updateSchedulingSetting("preferredWorkingDays", days)
    }

    function hasSchedulingDay(day) {
        return (schedulingSettingsDraft.preferredWorkingDays || []).indexOf(Number(day)) >= 0
    }

    function schedulingSettingsValues() {
        const draft = schedulingSettingsDraft
        const breakText = String(draft.breakBetweenWorkBlocksMinutes || "").trim()
        return {
            preferredWorkingDays: (draft.preferredWorkingDays || []).map(function(day) {
                return Number(day)
            }),
            workdayStart: String(draft.workdayStart || "").trim(),
            workdayEnd: String(draft.workdayEnd || "").trim(),
            maximumPlannedWorkMinutesPerDay: Number(draft.maximumPlannedWorkMinutesPerDay),
            defaultEstimatedDurationMinutes: Number(draft.defaultEstimatedDurationMinutes),
            breakBetweenWorkBlocksMinutes: breakText.length > 0 ? Number(breakText) : null
        }
    }

    function dateSummary(date, overdue) {
        if (!date) return qsTr("No due date")
        if (overdue) return qsTr("Overdue · due %1").arg(date)
        if (date === taskController.todayDate) return qsTr("Due today")
        const dueMillis = Date.parse(date + "T00:00:00Z")
        const todayMillis = Date.parse(taskController.todayDate + "T00:00:00Z")
        const days = Math.round((dueMillis - todayMillis) / 86400000)
        if (days === 1) return qsTr("Due tomorrow")
        if (days > 1 && days < 8) return qsTr("Due in %1 days").arg(days)
        return qsTr("Due %1").arg(date)
    }

    function taskRecord(taskId, title, description, completed, createdAt, updatedAt,
                        dueDate, priority, difficulty, revision, tags, subtasks) {
        return { id: Number(taskId), title: title, description: description,
                 completed: completed, createdAt: createdAt, updatedAt: updatedAt,
                 dueDate: dueDate, priority: priority, difficulty: difficulty,
                 revision: Number(revision), tags: tags, subtasks: subtasks }
    }

    function hasActiveFilters() {
        return String(filterState.query || "").trim().length > 0
            || String(filterState.dateMode || "any") !== "any"
            || String(filterState.completion || "all") !== "all"
            || String(filterState.priority || "").length > 0
            || String(filterState.difficulty || "").length > 0
            || (Array.isArray(filterState.tags) ? filterState.tags.length > 0
                                                : String(filterState.tags || "").trim().length > 0)
    }

    Connections {
        target: taskController
        function onFiltersChanged() { root.refreshControls() }
    }

    Connections {
        target: schedulingController
        function onPreferencesLoaded() {
            root.captureSchedulingSettings()
            if (root.schedulingSettingsStatus.length > 0)
                root.schedulingSettingsStatus = qsTr("Settings saved. Recommendations use the new preferences.")
        }
        function onPreferencesSaved() {
            root.captureSchedulingSettings()
            root.schedulingSettingsStatus = qsTr("Settings saved. Refreshing recommendations…")
        }
        function onStateChanged() {
            if (!schedulingController.previewVisible) {
                root.schedulingSettingsDraft = ({})
                root.schedulingSettingsDirty = false
                root.schedulingSettingsStatus = ""
            }
        }
    }

    Timer {
        id: historySearchTimer
        interval: 220
        repeat: false
        onTriggered: if (root.activeView === "history") root.loadGlobalHistory()
    }

    ColumnLayout {
        id: pageColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 14

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 16

            ColumnLayout {
                width: parent.width
                spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Shared.AppButton {
                    text: qsTr("Tasks")
                    quiet: root.activeView !== "tasks"
                    onClicked: root.activeView = "tasks"
                }
                Shared.AppButton {
                    text: qsTr("History")
                    quiet: root.activeView !== "history"
                    onClicked: root.activeView = "history"
                }
                Label {
                    Layout.fillWidth: true
                    text: root.activeView === "history"
                          ? qsTr("Chronological task changes")
                          : qsTr("Create, organize, and complete your tasks")
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
                BusyIndicator {
                    visible: root.activeView === "tasks" && taskController.loading
                    running: visible
                    width: 25
                    height: 25
                    Accessible.name: qsTr("Loading tasks")
                }
                Shared.AppButton {
                    text: qsTr("Refresh")
                    visible: root.activeView === "tasks"
                    quiet: true
                    enabled: taskController.ready && !taskController.loading
                    onClicked: taskController.refresh()
                }
                Shared.AppButton {
                    text: qsTr("Smart schedule")
                    visible: root.activeView === "tasks"
                    quiet: true
                    enabled: taskController.ready && !schedulingController.loading
                             && !schedulingController.applying
                             && !root.schedulingSettingsDirty
                    onClicked: schedulingController.generatePreview()
                }
                Shared.AppButton {
                    text: qsTr("Add task")
                    visible: root.activeView === "tasks"
                    enabled: taskController.ready && !taskController.mutationBusy
                    onClicked: editor.openNew()
                }
            }

            GridLayout {
                visible: root.activeView === "tasks"
                Layout.fillWidth: true
                columns: width > 1000 ? 4 : width > 620 ? 3 : 2
                columnSpacing: 10
                rowSpacing: 9

                Shared.AppTextField {
                    id: queryField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Title, description, tag, or subtask")
                    accessibleLabel: qsTr("Search tasks by title, description, tag, or subtask")
                    maximumLength: 512
                    onTextEdited: {
                        root.enforceSearchLimit()
                        root.sendFilters()
                    }
                }
                Shared.AppComboBox {
                    id: dateModeBox
                    Layout.fillWidth: true
                    accessibleLabel: qsTr("Due date filter")
                    textRole: "label"
                    valueRole: "value"
                    model: root.dateModes
                    onActivated: root.sendFilters()
                }
                Shared.AppTextField {
                    id: exactDateField
                    visible: dateModeBox.currentValue === "exact"
                    Layout.fillWidth: true
                    placeholderText: qsTr("YYYY-MM-DD")
                    accessibleLabel: qsTr("Exact due date, YYYY-MM-DD")
                    maximumLength: 10
                    onTextEdited: root.sendFilters()
                }
                Shared.AppTextField {
                    id: dueFromField
                    visible: dateModeBox.currentValue === "range"
                    Layout.fillWidth: true
                    placeholderText: qsTr("From YYYY-MM-DD")
                    accessibleLabel: qsTr("Due date range start, YYYY-MM-DD")
                    maximumLength: 10
                    onTextEdited: root.sendFilters()
                }
                Shared.AppTextField {
                    id: dueToField
                    visible: dateModeBox.currentValue === "range"
                    Layout.fillWidth: true
                    placeholderText: qsTr("To YYYY-MM-DD")
                    accessibleLabel: qsTr("Due date range end, YYYY-MM-DD")
                    maximumLength: 10
                    onTextEdited: root.sendFilters()
                }
                Shared.AppComboBox {
                    id: completionBox
                    Layout.fillWidth: true
                    accessibleLabel: qsTr("Completion filter")
                    textRole: "label"
                    valueRole: "value"
                    model: root.completionModes
                    onActivated: root.sendFilters()
                }
                Shared.AppComboBox {
                    id: priorityFilterBox
                    Layout.fillWidth: true
                    accessibleLabel: qsTr("Priority filter")
                    textRole: "label"
                    valueRole: "value"
                    model: root.priorityModes
                    onActivated: root.sendFilters()
                }
                Shared.AppComboBox {
                    id: difficultyFilterBox
                    Layout.fillWidth: true
                    accessibleLabel: qsTr("Difficulty filter")
                    textRole: "label"
                    valueRole: "value"
                    model: root.difficultyModes
                    onActivated: root.sendFilters()
                }
                Shared.AppTextField {
                    id: tagsFilterField
                    Layout.fillWidth: true
                    placeholderText: qsTr("work, urgent")
                    accessibleLabel: qsTr("Filter by all listed tags")
                    onTextEdited: root.sendFilters()
                }
                Shared.AppComboBox {
                    id: sortBox
                    Layout.fillWidth: true
                    accessibleLabel: qsTr("Task sort order")
                    textRole: "label"
                    valueRole: "value"
                    model: root.sortModes
                    onActivated: root.sendFilters()
                }
                ThemeModule.AppCheckBox {
                    id: ascendingCheck
                    text: qsTr("Ascending")
                    Accessible.name: qsTr("Sort ascending")
                    contentItem: Label {
                        text: ascendingCheck.text
                        color: ThemeModule.Theme.textSecondary
                        leftPadding: ascendingCheck.indicator.width + ascendingCheck.spacing
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: root.sendFilters()
                }
                Shared.AppButton {
                    text: qsTr("Clear filters")
                    quiet: true
                    onClicked: taskController.clearFilters()
                }
            }
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            visible: root.activeView === "tasks" && schedulingController.previewVisible
            padding: 16

            ColumnLayout {
                width: parent.width
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Smart schedule preview")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: ThemeModule.Theme.headingFontSize
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    BusyIndicator {
                        visible: schedulingController.loading || schedulingController.applying
                        running: visible
                        width: 24
                        height: 24
                        Accessible.name: qsTr("Calculating schedule recommendations")
                    }
                    Shared.AppButton {
                        text: qsTr("Refresh preview")
                        quiet: true
                        enabled: taskController.ready && !schedulingController.loading
                                 && !schedulingController.applying
                                 && !root.schedulingSettingsDirty
                        onClicked: schedulingController.generatePreview()
                    }
                    Shared.AppButton {
                        text: qsTr("Cancel preview")
                        quiet: true
                        enabled: !schedulingController.applying
                                 && !schedulingController.preferencesSaving
                        onClicked: schedulingController.rejectProposal()
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: ThemeModule.Theme.borderColor
                }

                Label {
                    text: qsTr("Scheduling settings")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Recommendations use local dates and these working hours. Settings are saved to this profile.")
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                    wrapMode: Text.Wrap
                }

                Shared.StatePanel {
                    Layout.fillWidth: true
                    visible: !schedulingController.preferencesAvailable
                    state: schedulingController.loading ? "loading"
                           : schedulingController.errorMessage.length > 0 ? "error" : "empty"
                    title: schedulingController.loading ? qsTr("Loading scheduling settings")
                           : schedulingController.errorMessage.length > 0
                             ? qsTr("Scheduling settings could not be loaded")
                             : qsTr("Settings are not available")
                    message: schedulingController.loading
                             ? qsTr("Reading the saved workdays, hours, and capacity from this profile.")
                             : qsTr("Refresh the schedule preview to load profile settings.")
                }

                GridLayout {
                    id: schedulingSettingsGrid
                    Layout.fillWidth: true
                    visible: schedulingController.preferencesAvailable
                    columns: width >= 900 ? 3 : width >= 600 ? 2 : 1
                    columnSpacing: 12
                    rowSpacing: 10

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.columnSpan: schedulingSettingsGrid.columns
                        Label {
                            text: qsTr("Preferred working days")
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 5
                            Repeater {
                                model: root.schedulingWeekdays
                                delegate: ThemeModule.AppCheckBox {
                                    required property var modelData
                                    text: modelData.label
                                    Accessible.name: modelData.accessibleLabel
                                    checked: root.hasSchedulingDay(modelData.value)
                                    enabled: !schedulingController.loading
                                             && !schedulingController.applying
                                             && !schedulingController.preferencesSaving
                                    onClicked: root.toggleSchedulingDay(modelData.value)
                                }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: (root.schedulingSettingsDraft.preferredWorkingDays || []).length === 0
                            text: qsTr("No working days are selected. Recommendations will be marked unschedulable.")
                            color: ThemeModule.Theme.warning
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                            wrapMode: Text.Wrap
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Workday starts · HH:mm")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                        }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.schedulingSettingsDraft.workdayStart || "")
                            placeholderText: qsTr("09:00")
                            accessibleLabel: qsTr("Workday start time in 24-hour HH:mm format")
                            maximumLength: 5
                            enabled: !schedulingController.loading && !schedulingController.applying
                                     && !schedulingController.preferencesSaving
                            onTextEdited: root.updateSchedulingSetting("workdayStart", text)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Workday ends · HH:mm")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                        }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.schedulingSettingsDraft.workdayEnd || "")
                            placeholderText: qsTr("17:00")
                            accessibleLabel: qsTr("Workday end time in 24-hour HH:mm format")
                            maximumLength: 5
                            enabled: !schedulingController.loading && !schedulingController.applying
                                     && !schedulingController.preferencesSaving
                            onTextEdited: root.updateSchedulingSetting("workdayEnd", text)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Maximum planned work per day · minutes")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                            wrapMode: Text.Wrap
                        }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.schedulingSettingsDraft.maximumPlannedWorkMinutesPerDay || "")
                            placeholderText: qsTr("360")
                            accessibleLabel: qsTr("Maximum planned work minutes per day")
                            maximumLength: 4
                            inputMethodHints: Qt.ImhDigitsOnly
                            enabled: !schedulingController.loading && !schedulingController.applying
                                     && !schedulingController.preferencesSaving
                            onTextEdited: root.updateSchedulingSetting(
                                              "maximumPlannedWorkMinutesPerDay", text)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Default task estimate · minutes")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                            wrapMode: Text.Wrap
                        }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.schedulingSettingsDraft.defaultEstimatedDurationMinutes || "")
                            placeholderText: qsTr("60")
                            accessibleLabel: qsTr("Default estimated task duration in minutes")
                            maximumLength: 6
                            inputMethodHints: Qt.ImhDigitsOnly
                            enabled: !schedulingController.loading && !schedulingController.applying
                                     && !schedulingController.preferencesSaving
                            onTextEdited: root.updateSchedulingSetting(
                                              "defaultEstimatedDurationMinutes", text)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Break between work blocks · minutes, optional")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                            wrapMode: Text.Wrap
                        }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.schedulingSettingsDraft.breakBetweenWorkBlocksMinutes || "")
                            placeholderText: qsTr("15, or leave blank")
                            accessibleLabel: qsTr("Optional break between work blocks in minutes")
                            maximumLength: 3
                            inputMethodHints: Qt.ImhDigitsOnly
                            enabled: !schedulingController.loading && !schedulingController.applying
                                     && !schedulingController.preferencesSaving
                            onTextEdited: root.updateSchedulingSetting(
                                              "breakBetweenWorkBlocksMinutes", text)
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: schedulingController.preferencesErrorMessage.length > 0
                    text: schedulingController.preferencesErrorMessage
                    color: ThemeModule.Theme.error
                    font.family: ThemeModule.Theme.fontFamily
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: schedulingController.preferencesAvailable
                    Label {
                        Layout.fillWidth: true
                        text: root.schedulingSettingsDirty
                              ? qsTr("Unsaved settings · saving them refreshes the recommendation preview")
                              : root.schedulingSettingsStatus.length > 0
                                ? root.schedulingSettingsStatus
                                : qsTr("Leave the break blank to allow no required break between blocks.")
                        color: root.schedulingSettingsDirty ? ThemeModule.Theme.warning
                               : root.schedulingSettingsStatus.length > 0
                                 ? ThemeModule.Theme.success : ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: ThemeModule.Theme.metadataFontSize
                        wrapMode: Text.Wrap
                    }
                    Shared.AppButton {
                        text: qsTr("Discard changes")
                        quiet: true
                        visible: root.schedulingSettingsDirty
                        enabled: !schedulingController.preferencesSaving
                        onClicked: root.captureSchedulingSettings()
                    }
                    Shared.AppButton {
                        text: schedulingController.preferencesSaving
                              ? qsTr("Saving…") : qsTr("Save settings")
                        enabled: root.schedulingSettingsDirty
                                 && taskController.ready
                                 && !schedulingController.loading
                                 && !schedulingController.applying
                                 && !schedulingController.preferencesSaving
                        onClicked: schedulingController.savePreferences(
                                       root.schedulingSettingsValues())
                    }
                }

                Shared.StatePanel {
                    Layout.fillWidth: true
                    visible: schedulingController.loading
                    state: "loading"
                    title: qsTr("Preparing recommendations")
                    message: qsTr("Loading incomplete tasks and saved scheduling preferences.")
                }

                Label {
                    Layout.fillWidth: true
                    visible: schedulingController.errorMessage.length > 0
                    text: schedulingController.errorMessage
                    color: ThemeModule.Theme.error
                    wrapMode: Text.Wrap
                    font.family: ThemeModule.Theme.fontFamily
                    Accessible.role: Accessible.AlertMessage
                }

                Label {
                    Layout.fillWidth: true
                    visible: schedulingController.previewStale
                    text: schedulingController.statusMessage
                    color: ThemeModule.Theme.warning
                    wrapMode: Text.Wrap
                    font.family: ThemeModule.Theme.fontFamily
                    Accessible.role: Accessible.AlertMessage
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: schedulingController.previewAvailable
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("%1 of %2 valid recommendations accepted · choices are in memory and not saved yet")
                              .arg(schedulingController.acceptedCount)
                              .arg(schedulingController.validCount)
                        color: ThemeModule.Theme.textSecondary
                        wrapMode: Text.Wrap
                        font.family: ThemeModule.Theme.fontFamily
                    }
                    Shared.AppButton {
                        text: qsTr("Accept all valid")
                        enabled: schedulingController.canStage
                                 && !root.schedulingSettingsDirty
                                 && schedulingController.validCount > 0
                        onClicked: schedulingController.acceptAllValid()
                    }
                    Shared.AppButton {
                        text: qsTr("Apply accepted")
                        enabled: schedulingController.canApply
                                 && !root.schedulingSettingsDirty
                        onClicked: schedulingController.applyAccepted()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: schedulingController.statusMessage.length > 0
                             && !schedulingController.previewStale
                    text: schedulingController.statusMessage
                    color: ThemeModule.Theme.accentCyanLight
                    wrapMode: Text.Wrap
                    font.family: ThemeModule.Theme.fontFamily
                }

                Label {
                    Layout.fillWidth: true
                    visible: schedulingController.previewAvailable
                             && schedulingController.warnings.length > 0
                    text: schedulingController.warnings.join("\n")
                    color: ThemeModule.Theme.warning
                    wrapMode: Text.Wrap
                    font.family: ThemeModule.Theme.fontFamily
                    Accessible.role: Accessible.AlertMessage
                }

                Label {
                    Layout.fillWidth: true
                    visible: schedulingController.previewAvailable
                    text: qsTr("Recommendations")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Shared.StatePanel {
                    Layout.fillWidth: true
                    visible: schedulingController.previewAvailable
                             && schedulingController.recommendations.count === 0
                    state: "empty"
                    title: qsTr("No incomplete tasks to schedule")
                    message: qsTr("There is no scheduling work in the current snapshot.")
                }

                ListView {
                    id: scheduleRecommendationList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(430, Math.max(0, contentHeight))
                    visible: schedulingController.previewAvailable
                             && schedulingController.recommendations.count > 0
                    clip: true
                    spacing: 8
                    boundsBehavior: Flickable.StopAtBounds
                    Accessible.name: qsTr("Smart schedule recommendations")
                    model: schedulingController.recommendations
                    ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Shared.AppCard {
                        required property double taskId
                        required property string title
                        required property string dueDate
                        required property string priority
                        required property string difficulty
                        required property int estimatedDurationMinutes
                        required property string plannedDate
                        required property string plannedStartTime
                        required property string state
                        required property bool overdue
                        required property bool usedDefaultDuration
                        required property bool retainedExistingSchedule
                        required property bool hasConflict
                        required property bool overloaded
                        required property string reason
                        required property var warnings
                        required property bool canAccept
                        required property bool accepted

                        width: ListView.view.width
                        height: implicitHeight
                        padding: 12
                        implicitHeight: recommendationContent.implicitHeight + padding * 2

                        ColumnLayout {
                            id: recommendationContent
                            width: parent.width - parent.padding * 2
                            spacing: 7

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: title
                                    color: ThemeModule.Theme.textPrimary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                    wrapMode: Text.Wrap
                                }
                                Shared.StatusBadge {
                                    text: state === "scheduled" ? qsTr("Scheduled") : qsTr("Unscheduled")
                                    badgeColor: state === "scheduled"
                                                ? ThemeModule.Theme.success : ThemeModule.Theme.error
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Deadline: %1 · Priority: %2 · Difficulty: %3 · Estimate: %4 min%5")
                                      .arg(dueDate.length > 0 ? dueDate : qsTr("none"))
                                      .arg(priority)
                                      .arg(difficulty.length > 0 ? difficulty : qsTr("not set"))
                                      .arg(estimatedDurationMinutes)
                                      .arg(usedDefaultDuration ? qsTr(" (default)") : "")
                                color: overdue ? ThemeModule.Theme.error : ThemeModule.Theme.textSecondary
                                wrapMode: Text.Wrap
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }

                            Label {
                                Layout.fillWidth: true
                                text: state === "scheduled"
                                      ? qsTr("Proposed: %1%2")
                                        .arg(plannedDate)
                                        .arg(plannedStartTime.length > 0
                                             ? qsTr(" at %1").arg(plannedStartTime) : qsTr(" · start time unspecified"))
                                      : qsTr("No available work block")
                                color: state === "scheduled" ? ThemeModule.Theme.accentCyanLight
                                                               : ThemeModule.Theme.error
                                wrapMode: Text.Wrap
                                font.family: ThemeModule.Theme.fontFamily
                                font.weight: Font.DemiBold
                            }

                            Label {
                                Layout.fillWidth: true
                                text: reason
                                visible: reason.length > 0
                                color: ThemeModule.Theme.textSecondary
                                wrapMode: Text.Wrap
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: warnings && warnings.length > 0
                                text: warnings ? warnings.join("\n") : ""
                                color: hasConflict || overloaded || overdue
                                       ? ThemeModule.Theme.warning : ThemeModule.Theme.textSecondary
                                wrapMode: Text.Wrap
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                                Accessible.role: Accessible.AlertMessage
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Shared.StatusBadge {
                                    visible: retainedExistingSchedule
                                    text: qsTr("Existing plan kept")
                                    badgeColor: ThemeModule.Theme.accentCyan
                                }
                                Shared.StatusBadge {
                                    visible: hasConflict
                                    text: qsTr("Conflict")
                                    badgeColor: ThemeModule.Theme.error
                                }
                                Shared.StatusBadge {
                                    visible: overloaded
                                    text: qsTr("Over daily capacity")
                                    badgeColor: ThemeModule.Theme.error
                                }
                                Item { Layout.fillWidth: true }
                                Shared.AppButton {
                                    text: accepted ? qsTr("Accepted") : qsTr("Accept")
                                    quiet: accepted
                                    enabled: schedulingController.canStage && canAccept
                                             && !root.schedulingSettingsDirty
                                    Accessible.name: accepted
                                                    ? qsTr("Remove staged acceptance for %1").arg(title)
                                                    : qsTr("Accept recommendation for %1").arg(title)
                                    onClicked: schedulingController.setRecommendationAccepted(
                                                   taskId, !accepted)
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: schedulingController.previewAvailable
                    text: qsTr("Daily workload · local dates")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                GridView {
                    id: scheduleDaySummaries
                    Layout.fillWidth: true
                    Layout.preferredHeight: schedulingController.previewAvailable
                                           ? Math.min(230, Math.max(84, contentHeight)) : 0
                    visible: schedulingController.previewAvailable
                    clip: true
                    cellWidth: width > 840 ? width / 4 : width > 420 ? width / 2 : width
                    cellHeight: 72
                    model: schedulingController.daySummaries
                    Accessible.name: qsTr("Daily scheduled workload and capacity")
                    ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Rectangle {
                        required property var modelData
                        width: scheduleDaySummaries.cellWidth - 6
                        height: 64
                        radius: ThemeModule.Theme.panelRadius
                        color: Boolean(modelData.overloaded) ? ThemeModule.Theme.errorSurface
                                                             : ThemeModule.Theme.insetSurface
                        border.color: Boolean(modelData.overloaded) ? ThemeModule.Theme.error
                                                                      : ThemeModule.Theme.borderColor
                        border.width: ThemeModule.Theme.panelBorderWidth

                        Column {
                            anchors.fill: parent
                            anchors.margins: 7
                            spacing: 3
                            Label {
                                width: parent.width
                                text: String(modelData.date || "")
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.weight: Font.DemiBold
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }
                            Label {
                                width: parent.width
                                text: qsTr("%1 / %2 min planned")
                                      .arg(Number(modelData.plannedMinutes || 0))
                                      .arg(Number(modelData.capacityMinutes || 0))
                                color: Boolean(modelData.overloaded) ? ThemeModule.Theme.error
                                                                     : ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }
                            Label {
                                width: parent.width
                                visible: Boolean(modelData.hasUnspecifiedStartTime)
                                         || Boolean(modelData.overloaded)
                                text: Boolean(modelData.overloaded) ? qsTr("Over capacity")
                                      : qsTr("Start time unspecified")
                                color: ThemeModule.Theme.warning
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }
                        }
                    }
                }
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            visible: root.activeView === "tasks" && taskController.errorMessage.length > 0
            padding: 13
            Label {
                width: parent.width
                text: taskController.errorMessage
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
                font.family: ThemeModule.Theme.fontFamily
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
            }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.activeView === "tasks" && taskController.model.count === 0
            state: taskController.errorMessage.length > 0 ? "error"
                   : !taskController.ready || taskController.loading ? "loading" : "empty"
            title: taskController.errorMessage.length > 0
                   ? qsTr("Tasks could not be loaded")
                   : taskController.ready
                     ? (root.hasActiveFilters() ? qsTr("No tasks match these filters") : qsTr("No tasks yet"))
                     : qsTr("Starting task storage")
            message: taskController.errorMessage.length > 0
                     ? qsTr("Your current filters are unchanged. Retry after the issue is resolved.")
                     : taskController.ready
                       ? (root.hasActiveFilters() ? qsTr("Clear filters or try a different search.")
                                                  : qsTr("Add a task to get started."))
                       : qsTr("Please wait.")
        }

        ListView {
            id: taskList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(720, Math.max(0, contentHeight))
            visible: root.activeView === "tasks" && taskController.model.count > 0
            clip: true
            spacing: 9
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Task results")
            model: taskController.model
            delegate: Shared.AppCard {
                required property double taskId
                required property string title
                required property string description
                required property bool completed
                required property string createdAt
                required property string updatedAt
                required property string dueDate
                required property string priority
                required property string difficulty
                required property double revision
                required property var tags
                required property var subtasks
                required property int subtaskCount
                required property int completedSubtasks
                required property bool overdue

                width: ListView.view.width
                height: implicitHeight
                padding: 15
                implicitHeight: cardContent.implicitHeight + padding * 2

                ColumnLayout {
                    id: cardContent
                    width: parent.width - parent.padding * 2
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 9

                        ThemeModule.AppCheckBox {
                            checked: completed
                            enabled: !taskController.mutationBusy
                            Accessible.name: completed ? qsTr("Mark task incomplete") : qsTr("Mark task complete")
                            onClicked: taskController.toggleTask(taskId, checked)
                            indicator: Rectangle {
                                implicitWidth: 22
                                implicitHeight: 22
                                radius: ThemeModule.Theme.controlRadius
                                color: parent.checked ? ThemeModule.Theme.selectionBlue : ThemeModule.Theme.insetSurface
                                border.color: parent.activeFocus ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.borderColor
                                border.width: parent.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
                                Text {
                                    anchors.centerIn: parent
                                    text: parent.parent.checked ? "✓" : ""
                                    color: ThemeModule.Theme.textPrimary
                                    font.pixelSize: 14
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: title
                                color: completed ? ThemeModule.Theme.textSecondary : ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                font.strikeout: completed
                                wrapMode: Text.Wrap
                            }
                            Label {
                                visible: description.length > 0
                                Layout.fillWidth: true
                                text: description
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                maximumLineCount: 4
                                elide: Text.ElideRight
                            }
                        }

                        Shared.StatusBadge {
                            visible: difficulty.length > 0
                            text: difficulty.length > 0
                                  ? difficulty.charAt(0).toUpperCase() + difficulty.slice(1) : ""
                            badgeColor: difficulty === "hard" ? ThemeModule.Theme.warning
                                        : difficulty === "easy" ? ThemeModule.Theme.success
                                        : ThemeModule.Theme.accentCyan
                        }
                        Shared.AppButton {
                            text: qsTr("%1 priority").arg(priority)
                            quiet: true
                            enabled: !taskController.mutationBusy
                            Accessible.name: qsTr("Cycle task priority, currently %1").arg(priority)
                            onClicked: taskController.cyclePriority(taskId)
                        }
                        Shared.AppButton {
                            text: qsTr("Edit")
                            quiet: true
                            enabled: !taskController.mutationBusy
                            onClicked: editor.openForTask(root.taskRecord(
                                taskId, title, description, completed, createdAt, updatedAt,
                                dueDate, priority, difficulty, revision, tags, subtasks))
                        }
                        Shared.AppButton {
                            text: qsTr("History")
                            quiet: true
                            enabled: historyController.ready
                            onClicked: historyDialog.openForTask(taskId, title)
                        }
                        Shared.AppButton {
                            text: qsTr("Delete")
                            destructive: true
                            quiet: true
                            enabled: !taskController.mutationBusy
                            onClicked: taskController.deleteTask(taskId)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 7
                        Shared.StatusBadge {
                            text: root.dateSummary(dueDate, overdue)
                            badgeColor: overdue ? ThemeModule.Theme.error : ThemeModule.Theme.accentCyan
                        }
                        Shared.StatusBadge {
                            text: completed ? qsTr("Completed") : qsTr("Pending")
                            badgeColor: completed ? ThemeModule.Theme.success : ThemeModule.Theme.accentCyan
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Created %1").arg(createdAt)
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }

                    Flow {
                        visible: tags.length > 0
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: tags
                            delegate: Shared.StatusBadge {
                                required property string modelData
                                text: modelData
                                badgeColor: ThemeModule.Theme.accentCyan
                            }
                        }
                    }

                    ColumnLayout {
                        visible: difficulty === "hard" && subtaskCount > 0
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            text: qsTr("Subtasks · %1/%2 complete")
                                  .arg(completedSubtasks).arg(subtaskCount)
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                        }
                        Repeater {
                            model: subtasks
                            delegate: ThemeModule.AppCheckBox {
                                required property var modelData
                                Layout.fillWidth: true
                                checked: Boolean(modelData.completed)
                                enabled: !taskController.mutationBusy
                                text: String(modelData.title || "")
                                Accessible.name: qsTr("Subtask: %1").arg(text)
                                contentItem: Label {
                                    text: parent.text
                                    color: parent.checked ? ThemeModule.Theme.textSecondary : ThemeModule.Theme.textPrimary
                                    leftPadding: parent.indicator.width + parent.spacing
                                    verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.Wrap
                                }
                                onClicked: taskController.toggleSubtask(
                                    taskId, Number(modelData.id), checked)
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.activeView === "tasks"
                     && (taskController.model.count > 0 || taskController.pageNumber > 1)
            Shared.AppButton {
                text: qsTr("Previous")
                quiet: true
                enabled: taskController.pageNumber > 1 && !taskController.loading
                onClicked: taskController.previousPage()
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Page %1").arg(taskController.pageNumber)
                color: ThemeModule.Theme.textSecondary
                horizontalAlignment: Text.AlignHCenter
                font.family: ThemeModule.Theme.fontFamily
            }
            Shared.AppButton {
                text: qsTr("Next")
                quiet: true
                enabled: taskController.hasMore && !taskController.loading
                onClicked: taskController.nextPage()
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            visible: root.activeView === "history"
            padding: 14
            ColumnLayout {
                width: parent.width
                spacing: 9
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Search task history")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                RowLayout {
                    Layout.fillWidth: true
                    Shared.AppTextField {
                        id: historyQueryField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Search by task title")
                        accessibleLabel: qsTr("Search task history by task title")
                        maximumLength: 256
                        onTextEdited: historySearchTimer.restart()
                    }
                    Shared.AppComboBox {
                        id: historyTypeBox
                        Layout.preferredWidth: Math.min(230, parent.width * 0.38)
                        accessibleLabel: qsTr("Filter task history by event type")
                        textRole: "label"
                        valueRole: "value"
                        model: root.historyEventTypes
                        onActivated: root.loadGlobalHistory()
                    }
                    BusyIndicator {
                        visible: historyController.loading
                        running: visible
                        implicitWidth: 24
                        implicitHeight: 24
                        Accessible.name: qsTr("Loading task history")
                    }
                    Shared.AppButton {
                        text: qsTr("Refresh")
                        quiet: true
                        enabled: historyController.ready && !historyController.loading
                        onClicked: root.loadGlobalHistory()
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.activeView === "history" && historyController.errorMessage.length > 0
            text: historyController.errorMessage
            color: ThemeModule.Theme.error
            wrapMode: Text.Wrap
            font.family: ThemeModule.Theme.fontFamily
            Accessible.role: Accessible.AlertMessage
        }

        Label {
            Layout.fillWidth: true
            visible: root.activeView === "history" && historyController.warningMessage.length > 0
            text: historyController.warningMessage
            color: ThemeModule.Theme.warning
            wrapMode: Text.Wrap
            font.family: ThemeModule.Theme.fontFamily
            Accessible.role: Accessible.AlertMessage
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.activeView === "history" && historyController.model.count === 0
            state: historyController.errorMessage.length > 0 ? "error"
                   : !historyController.ready || historyController.loading ? "loading" : "empty"
            title: historyController.errorMessage.length > 0
                   ? qsTr("History could not be loaded")
                   : historyController.warningMessage.length > 0
                     ? qsTr("Some history entries could not be read")
                   : historyController.ready ? qsTr("No matching task history")
                                             : qsTr("Starting task storage")
            message: historyController.errorMessage.length > 0
                     ? qsTr("Refresh this view after the storage issue is resolved.")
                     : historyController.warningMessage.length > 0
                       ? historyController.warningMessage
                     : historyController.ready
                       ? qsTr("Task changes will appear here as they are recorded.")
                       : qsTr("Please wait.")
        }

        ListView {
            id: historyList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(720, Math.max(100, contentHeight))
            visible: root.activeView === "history" && historyController.model.count > 0
            clip: true
            spacing: 8
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Chronological task history")
            model: historyController.model
            ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Shared.TaskHistoryEventCard {
                width: ListView.view.width
                eventType: model.eventType
                occurredAtUtc: model.occurredAtUtc
                taskTitle: model.taskTitle
                changes: model.changes
            }
        }

        Shared.AppButton {
            Layout.alignment: Qt.AlignHCenter
            visible: root.activeView === "history" && historyController.hasMore
            text: historyController.loading ? qsTr("Loading older events…") : qsTr("Load older events")
            quiet: true
            enabled: !historyController.loading
            onClicked: historyController.loadOlder()
        }
    }

    Dialogs.TaskEditorDialog {
        id: editor
        taskController: root.controller
    }

    Dialogs.TaskHistoryDialog {
        id: historyDialog
        historyController: root.historyController
    }

}
