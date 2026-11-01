import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root
    required property var controller
    required property var usageController
    required property var desktopApplicationChoicesController
    required property var setupChoicesController
    required property var steamGameChoicesController
    required property var steamReleaseChoicesController
    required property var gameUpdatesController
    implicitHeight: 820

    property string searchText: ""
    property string triggerFilter: "all"
    property string historyStatus: "all"
    property string historyTrigger: "all"
    property string historyStartDate: ""
    property string historyEndDate: ""
    property int historyOffset: 0
    property bool editorOpen: false
    property bool editingExisting: false
    property var currentRule: ({})
    property string draftName: ""
    property string draftDescription: ""
    property string draftTrigger: "task.completed"
    property var draftConfig: ({})
    property var conditionDrafts: []
    property var actionDrafts: []
    property int draftCooldown: 0
    property string deleteRuleId: ""
    property bool usageChoicesRequested: false
    property string searchTargetMessage: ""

    readonly property var triggerChoices: [
        { value: "task.completed", label: qsTr("Task completed") },
        { value: "task.overdue", label: qsTr("Task becomes overdue") },
        { value: "application.session_started", label: qsTr("Application session starts") },
        { value: "application.foreground_entered", label: qsTr("Application becomes foreground") },
        { value: "steam.price_target_reached", label: qsTr("Steam price target reached") },
        { value: "steam.release_date_changed", label: qsTr("Steam release date changes") },
        { value: "steam.game_news_available", label: qsTr("New Steam game news") },
        { value: "schedule.time_reached", label: qsTr("Scheduled local time") }
    ]
    readonly property var actionChoices: [
        { value: "show_notification", label: qsTr("Show notification") },
        { value: "launch_application", label: qsTr("Launch saved application") },
        { value: "launch_setup", label: qsTr("Launch Setup") },
        { value: "open_url", label: qsTr("Open HTTP or HTTPS URL") }
    ]
    readonly property var conditionChoices: [
        { value: "task_id", label: qsTr("Specific task ID") },
        { value: "task_tag", label: qsTr("Task tag") },
        { value: "task_priority", label: qsTr("Task priority") },
        { value: "application_id", label: qsTr("Application identity") },
        { value: "steam_app_id", label: qsTr("Steam App ID") },
        { value: "price_lte", label: qsTr("Price at or below (minor units)") },
        { value: "day_of_week", label: qsTr("Day of week (1–7)") },
        { value: "time_range", label: qsTr("Local time range (HH:mm/HH:mm)") }
    ]

    function triggerLabel(value) {
        for (let i = 0; i < triggerChoices.length; ++i)
            if (triggerChoices[i].value === value) return triggerChoices[i].label
        return value || qsTr("Unknown trigger")
    }

    function actionLabel(value) {
        for (let i = 0; i < actionChoices.length; ++i)
            if (actionChoices[i].value === value) return actionChoices[i].label
        return value || qsTr("Unknown action")
    }

    function matchingRules() {
        const query = searchText.trim().toLocaleLowerCase()
        return (controller.rules || []).filter(function(rule) {
            if (triggerFilter !== "all" && String(rule.triggerType) !== triggerFilter) return false
            if (!query) return true
            return (String(rule.name || "") + " " + String(rule.description || "") + " "
                    + String(rule.triggerType || "")).toLocaleLowerCase().indexOf(query) >= 0
        })
    }

    function setConfig(key, value) {
        const next = Object.assign({}, draftConfig)
        next[key] = value
        draftConfig = next
    }

    function weekdaysFromText(value) {
        const result = []
        const pieces = String(value || "").split(",")
        for (let i = 0; i < pieces.length; ++i) {
            const day = Number(pieces[i].trim())
            if (Number.isInteger(day) && day >= 1 && day <= 7 && result.indexOf(day) < 0)
                result.push(day)
        }
        return result
    }

    function weekdaysText() {
        return (draftConfig.weekdays || []).join(",")
    }

    function defaultOperator(type) {
        if (type === "price_lte") return "less_equal"
        if (type === "time_range") return "between"
        if (type === "task_tag") return "contains"
        return "equals"
    }

    function availableConditionChoices() {
        if (draftTrigger === "task.completed" || draftTrigger === "task.overdue")
            return conditionChoices.slice(0, 3)
        if (draftTrigger.indexOf("application.") === 0)
            return conditionChoices.slice(3, 4)
        if (draftTrigger === "steam.price_target_reached")
            return conditionChoices.slice(4, 6)
        if (draftTrigger.indexOf("steam.") === 0)
            return conditionChoices.slice(4, 5)
        return conditionChoices.slice(6, 8)
    }

    function loadUsageChoices() {
        if (usageChoicesRequested || !usageController.ready) return
        usageChoicesRequested = true
        usageController.loadApplicationChoices()
    }

    function openNewRule() {
        editingExisting = false
        currentRule = ({})
        draftName = ""
        draftDescription = ""
        draftTrigger = "task.completed"
        draftConfig = ({ taskId: 0, tag: "", priority: "" })
        conditionDrafts = []
        actionDrafts = [{ type: "show_notification", targetId: 0, url: "",
                          message: "Task {task.title} has been completed.", delayMs: 0,
                          enabled: true, order: 0 }]
        draftCooldown = 0
        editorOpen = true
    }

    function openEditor(rule) {
        if (!rule || Number(rule.sourceTaskId || 0) > 0) return
        editingExisting = true
        currentRule = rule
        draftName = String(rule.name || "")
        draftDescription = String(rule.description || "")
        draftTrigger = String(rule.triggerType || "task.completed")
        draftConfig = Object.assign({}, rule.triggerConfig || {})
        conditionDrafts = (rule.conditions || []).map(function(condition) {
            return { type: String(condition.type || ""), operand: String(condition.operand || ""),
                     operator: String(condition.operator || root.defaultOperator(condition.type)) }
        })
        actionDrafts = (rule.actions || []).map(function(action, index) {
            return Object.assign({}, action, { order: index })
        })
        draftCooldown = Number(rule.cooldownSeconds || 0)
        editorOpen = true
    }

    function selectTrigger(value) {
        if (draftTrigger === value) return
        draftTrigger = value
        conditionDrafts = []
        if (value === "task.completed" || value === "task.overdue")
            draftConfig = { taskId: 0, tag: "", priority: "" }
        else if (value.indexOf("application.") === 0)
            draftConfig = { applicationId: "" }
        else if (value === "steam.price_target_reached")
            draftConfig = { appId: 0, country: "TR", currency: "TRY", targetMinor: 0 }
        else if (value.indexOf("steam.") === 0)
            draftConfig = { appId: 0 }
        else
            draftConfig = { time: "09:00", weekdays: [1, 2, 3, 4, 5], date: "" }
    }

    function updateCondition(index, key, value) {
        const next = conditionDrafts.slice()
        const condition = Object.assign({}, next[index] || {})
        condition[key] = value
        if (key === "type") condition.operator = defaultOperator(value)
        next[index] = condition
        conditionDrafts = next
    }

    function removeCondition(index) {
        const next = conditionDrafts.slice()
        next.splice(index, 1)
        conditionDrafts = next
    }

    function updateAction(index, key, value) {
        const next = actionDrafts.slice()
        const action = Object.assign({}, next[index] || {})
        action[key] = value
        if (key === "type") {
            action.targetId = 0
            action.url = ""
            action.message = value === "show_notification" ? "Automation event occurred." : ""
        }
        action.order = index
        next[index] = action
        actionDrafts = next
    }

    function moveAction(index, delta) {
        const destination = index + delta
        if (destination < 0 || destination >= actionDrafts.length) return
        const next = actionDrafts.slice()
        const current = next[index]
        next[index] = next[destination]
        next[destination] = current
        for (let i = 0; i < next.length; ++i) next[i].order = i
        actionDrafts = next
    }

    function removeAction(index) {
        if (actionDrafts.length <= 1) return
        const next = actionDrafts.slice()
        next.splice(index, 1)
        for (let i = 0; i < next.length; ++i) next[i].order = i
        actionDrafts = next
    }

    function saveDraft() {
        if (!draftName.trim() || actionDrafts.length === 0) return
        const map = {
            name: draftName,
            description: draftDescription,
            triggerType: draftTrigger,
            triggerConfig: draftConfig,
            conditions: conditionDrafts,
            actions: actionDrafts,
            enabled: true,
            cooldownSeconds: draftCooldown
        }
        if (editingExisting) {
            map.id = currentRule.id
            map.enabled = currentRule.enabled
            map.revision = currentRule.revision
        }
        controller.saveRule(map, editingExisting ? Number(currentRule.revision) : 0)
    }

    function loadHistoryPage(append) {
        if (!append) historyOffset = 0
        else historyOffset += 50
        controller.loadHistory(historyRuleFilter.currentValue || "",
            historyStatus === "all" ? "" : historyStatus,
            historyTrigger === "all" ? "" : historyTrigger, historyOffset,
            historyStartDate, historyEndDate)
    }

    function openSearchResult(result, action) {
        const id = String(result && result.targetId || "")
        const rule = (controller.rules || []).find(function(item) { return String(item.id) === id })
        searchTargetMessage = ""
        if (!rule) {
            searchTargetMessage = qsTr("This automation rule is no longer available. Refresh the rules and search again.")
            return
        }
        if (String(action || "") === "edit" && Number(rule.sourceTaskId || 0) === 0)
            openEditor(rule)
        else
            searchText = String(rule.name || "")
    }

    Component.onCompleted: {
        controller.loadHistory()
        loadUsageChoices()
        desktopApplicationChoicesController.loadAllApplications()
        setupChoicesController.reload()
    }

    Connections {
        target: root.controller
        function onRuleSaved(ruleId) {
            root.editorOpen = false
            root.currentRule = ({})
        }
        function onRuleDeleted(ruleId) {
            root.loadHistoryPage(false)
        }
    }

    Connections {
        target: root.usageController
        function onStateChanged() { root.loadUsageChoices() }
    }

    Flickable {
        id: pageScroll
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: pageColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: pageColumn
            width: pageScroll.width
            spacing: 14

            Shared.AppCard {
                Layout.fillWidth: true
                RowLayout {
                    width: parent.width
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Automation Rules")
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 20
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.controller.paused
                                ? qsTr("Automations are paused. Existing rules remain saved.")
                                : qsTr("Rules run in the background when their events and conditions match.")
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }
                    }
                    Shared.AppButton {
                        text: qsTr("Retry load")
                        quiet: true
                        visible: root.controller.errorMessage.length > 0 && !root.controller.ready
                        onClicked: root.controller.reload()
                    }
                    Shared.AppButton {
                        text: root.controller.paused ? qsTr("Resume automations") : qsTr("Pause automations")
                        quiet: true
                        enabled: root.controller.ready
                        onClicked: root.controller.setPaused(!root.controller.paused)
                    }
                    Shared.AppButton {
                        text: qsTr("Create rule")
                        enabled: root.controller.ready
                        onClicked: root.openNewRule()
                    }
                }
                Label {
                    width: parent.width
                    visible: root.controller.errorMessage.length > 0
                    text: root.controller.errorMessage
                    color: ThemeModule.Theme.error
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
                Label {
                    width: parent.width
                    visible: root.searchTargetMessage.length > 0
                    text: root.searchTargetMessage
                    color: ThemeModule.Theme.error
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                ColumnLayout {
                    width: parent.width
                    RowLayout {
                        Layout.fillWidth: true
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            placeholderText: qsTr("Search rules by name, description, or trigger")
                            accessibleLabel: qsTr("Search automation rules")
                            text: root.searchText
                            onTextEdited: root.searchText = text
                        }
                        Shared.AppComboBox {
                            Layout.preferredWidth: 250
                            accessibleLabel: qsTr("Filter rules by trigger")
                            textRole: "label"
                            valueRole: "value"
                            model: [{ value: "all", label: qsTr("All trigger types") }].concat(root.triggerChoices)
                            currentIndex: Math.max(0, indexOfValue(root.triggerFilter))
                            onActivated: root.triggerFilter = currentValue
                        }
                    }
                    Repeater {
                        model: root.matchingRules()
                        delegate: Shared.AppCard {
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                RowLayout {
                                    Layout.fillWidth: true
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            color: ThemeModule.Theme.textPrimary
                                            font.family: ThemeModule.Theme.fontFamily
                                            font.pixelSize: 16
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: root.triggerLabel(modelData.triggerType)
                                                  + " · " + String(modelData.actionSummary || qsTr("No actions"))
                                            color: ThemeModule.Theme.textSecondary
                                            font.family: ThemeModule.Theme.fontFamily
                                            font.pixelSize: 12
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                    Shared.AppCheckBox {
                                        text: modelData.enabled ? qsTr("Enabled") : qsTr("Disabled")
                                        checked: modelData.enabled
                                        enabled: Number(modelData.sourceTaskId || 0) === 0
                                        accessibleLabel: qsTr("Enable rule %1").arg(modelData.name)
                                        onToggled: root.controller.setRuleEnabled(modelData.id, checked)
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: String(modelData.description || "").length > 0
                                    text: modelData.description
                                    color: ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 13
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: String(modelData.lastExecutionError || "").length > 0
                                    text: modelData.lastExecutionError
                                    color: ThemeModule.Theme.error
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("If: %1").arg(modelData.conditionSummary || qsTr("No extra conditions"))
                                    color: ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: Number(modelData.sourceTaskId || 0) > 0
                                          ? qsTr("Managed from the Task Editor · latest: %1").arg(modelData.lastExecutionStatus || qsTr("No executions yet"))
                                          : modelData.lastExecutionStatus
                                            ? qsTr("Latest execution: %1 at %2").arg(modelData.lastExecutionStatus).arg(modelData.lastExecutionAtUtc || "")
                                            : qsTr("No executions yet")
                                    color: modelData.lastExecutionStatus === "failed"
                                           ? ThemeModule.Theme.error : ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Item { Layout.fillWidth: true }
                                    Shared.AppButton {
                                        text: qsTr("Duplicate")
                                        quiet: true
                                        enabled: Number(modelData.sourceTaskId || 0) === 0
                                        onClicked: root.controller.duplicateRule(modelData.id)
                                    }
                                    Shared.AppButton {
                                        text: qsTr("Edit")
                                        quiet: true
                                        enabled: Number(modelData.sourceTaskId || 0) === 0
                                        onClicked: root.openEditor(modelData)
                                    }
                                    Shared.AppButton {
                                        text: qsTr("Delete")
                                        quiet: true
                                        destructive: true
                                        enabled: Number(modelData.sourceTaskId || 0) === 0
                                        onClicked: {
                                            root.deleteRuleId = String(modelData.id)
                                            deleteConfirmation.open()
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.matchingRules().length === 0
                        text: root.controller.ready ? qsTr("No matching automation rules.")
                                                     : qsTr("Loading automation rules…")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 13
                    }
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                visible: root.editorOpen
                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    Label {
                        text: root.editingExisting ? qsTr("Edit rule") : qsTr("Create rule")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: qsTr("General")
                        color: ThemeModule.Theme.accentCyan
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Shared.AppTextField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("Rule name")
                        accessibleLabel: qsTr("Rule name")
                        text: root.draftName
                        onTextEdited: root.draftName = text
                    }
                    Shared.AppTextField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("Description (optional)")
                        accessibleLabel: qsTr("Rule description")
                        text: root.draftDescription
                        onTextEdited: root.draftDescription = text
                    }

                    Label {
                        text: qsTr("When")
                        color: ThemeModule.Theme.accentCyan
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Shared.AppComboBox {
                        Layout.fillWidth: true
                        accessibleLabel: qsTr("Automation trigger")
                        textRole: "label"
                        valueRole: "value"
                        model: root.triggerChoices
                        currentIndex: Math.max(0, indexOfValue(root.draftTrigger))
                        enabled: !root.editingExisting
                        onActivated: root.selectTrigger(currentValue)
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        visible: root.draftTrigger === "task.completed" || root.draftTrigger === "task.overdue"
                        Label { text: qsTr("Task ID (0 means any task)"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.draftConfig.taskId || 0)
                            inputMethodHints: Qt.ImhDigitsOnly
                            accessibleLabel: qsTr("Task ID, or zero for any task")
                            onTextEdited: root.setConfig("taskId", Number(text || 0))
                        }
                        Label { text: qsTr("Required tag (optional)"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.draftConfig.tag || "")
                            placeholderText: qsTr("e.g. work")
                            accessibleLabel: qsTr("Required task tag")
                            onTextEdited: root.setConfig("tag", text)
                        }
                        Label { text: qsTr("Priority (optional)"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppComboBox {
                            Layout.fillWidth: true
                            accessibleLabel: qsTr("Task priority filter")
                            model: [{ value: "", label: qsTr("Any priority") },
                                    { value: "low", label: qsTr("Low") },
                                    { value: "medium", label: qsTr("Medium") },
                                    { value: "high", label: qsTr("High") }]
                            textRole: "label"
                            valueRole: "value"
                            currentIndex: Math.max(0, indexOfValue(String(root.draftConfig.priority || "")))
                            onActivated: root.setConfig("priority", currentValue)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.draftTrigger.indexOf("application.") === 0
                        Label { text: qsTr("Tracked application"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppComboBox {
                            id: applicationChoice
                            Layout.fillWidth: true
                            accessibleLabel: qsTr("Choose a discovered application")
                            model: root.usageController.applicationChoices
                            textRole: "name"
                            valueRole: "applicationId"
                            currentIndex: root.draftConfig.applicationId
                                          ? Math.max(0, indexOfValue(String(root.draftConfig.applicationId))) : -1
                            onActivated: root.setConfig("applicationId", currentValue)
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        visible: root.draftTrigger.indexOf("steam.") === 0
                        Label { text: qsTr("Followed game"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppComboBox {
                            Layout.fillWidth: true
                            accessibleLabel: qsTr("Choose a followed Steam game")
                            model: root.draftTrigger === "steam.game_news_available"
                                  ? root.gameUpdatesController.games
                                  : root.draftTrigger === "steam.release_date_changed"
                                    ? root.steamReleaseChoicesController.releases : root.steamGameChoicesController.games
                            textRole: root.draftTrigger === "steam.release_date_changed" ? "title" : "name"
                            valueRole: root.draftTrigger === "steam.release_date_changed"
                                       || root.draftTrigger === "steam.game_news_available"
                                       ? "appId" : "steamAppId"
                            currentIndex: Number(root.draftConfig.appId || 0) > 0
                                          ? Math.max(0, indexOfValue(Number(root.draftConfig.appId))) : -1
                            onActivated: root.setConfig("appId", Number(currentValue))
                        }
                        Label {
                            visible: root.draftTrigger === "steam.price_target_reached"
                            text: qsTr("Store country"); color: ThemeModule.Theme.textSecondary
                        }
                        Shared.AppTextField {
                            visible: root.draftTrigger === "steam.price_target_reached"
                            Layout.fillWidth: true
                            text: String(root.draftConfig.country || "TR")
                            placeholderText: qsTr("Two-letter code, e.g. TR")
                            accessibleLabel: qsTr("Steam store country code")
                            onTextEdited: root.setConfig("country", text.toUpperCase())
                        }
                        Label {
                            visible: root.draftTrigger === "steam.price_target_reached"
                            text: qsTr("Currency"); color: ThemeModule.Theme.textSecondary
                        }
                        Shared.AppTextField {
                            visible: root.draftTrigger === "steam.price_target_reached"
                            Layout.fillWidth: true
                            text: String(root.draftConfig.currency || "TRY")
                            placeholderText: qsTr("Three-letter code, e.g. TRY")
                            accessibleLabel: qsTr("Steam price currency")
                            onTextEdited: root.setConfig("currency", text.toUpperCase())
                        }
                        Label {
                            visible: root.draftTrigger === "steam.price_target_reached"
                            text: qsTr("Target in minor units"); color: ThemeModule.Theme.textSecondary
                        }
                        Shared.AppTextField {
                            visible: root.draftTrigger === "steam.price_target_reached"
                            Layout.fillWidth: true
                            text: String(root.draftConfig.targetMinor || "")
                            placeholderText: qsTr("e.g. 30000 for 300.00")
                            inputMethodHints: Qt.ImhDigitsOnly
                            accessibleLabel: qsTr("Price target in currency minor units")
                            onTextEdited: root.setConfig("targetMinor", Number(text || 0))
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        visible: root.draftTrigger === "schedule.time_reached"
                        Label { text: qsTr("Local time (24-hour)"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.draftConfig.time || "09:00")
                            placeholderText: "09:00"
                            accessibleLabel: qsTr("Scheduled local time in HH colon mm format")
                            onTextEdited: root.setConfig("time", text)
                        }
                        Label { text: qsTr("Weekdays (1 Monday to 7 Sunday)"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: root.weekdaysText()
                            placeholderText: qsTr("1,2,3,4,5")
                            accessibleLabel: qsTr("Selected weekdays, comma separated")
                            onTextEdited: root.setConfig("weekdays", root.weekdaysFromText(text))
                        }
                        Label { text: qsTr("One-time date (optional)"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppTextField {
                            Layout.fillWidth: true
                            text: String(root.draftConfig.date || "")
                            placeholderText: "YYYY-MM-DD"
                            accessibleLabel: qsTr("Optional one-time local date")
                            onTextEdited: root.setConfig("date", text)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("If · all conditions must match"); color: ThemeModule.Theme.accentCyan; font.weight: Font.DemiBold }
                        Item { Layout.fillWidth: true }
                        Shared.AppButton {
                            text: qsTr("Add condition")
                            quiet: true
                            enabled: root.conditionDrafts.length < 12
                            onClicked: {
                                const next = root.conditionDrafts.slice()
                                next.push({ type: "task_tag", operand: "", operator: "contains" })
                                root.conditionDrafts = next
                            }
                        }
                    }
                    Repeater {
                        model: root.conditionDrafts
                        delegate: RowLayout {
                            required property int index
                            Layout.fillWidth: true
                            Shared.AppComboBox {
                                Layout.preferredWidth: 270
                                accessibleLabel: qsTr("Condition type")
                                model: root.availableConditionChoices()
                                textRole: "label"
                                valueRole: "value"
                                currentIndex: Math.max(0, indexOfValue(root.conditionDrafts[index].type))
                                onActivated: root.updateCondition(index, "type", currentValue)
                            }
                            Shared.AppTextField {
                                Layout.fillWidth: true
                                placeholderText: qsTr("Condition value")
                                accessibleLabel: qsTr("Condition operand")
                                text: String(root.conditionDrafts[index].operand || "")
                                onTextEdited: root.updateCondition(index, "operand", text)
                            }
                            Shared.AppButton {
                                text: qsTr("Remove")
                                quiet: true
                                onClicked: root.removeCondition(index)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Then · actions run sequentially and stop after a failure"); color: ThemeModule.Theme.accentCyan; font.weight: Font.DemiBold }
                        Item { Layout.fillWidth: true }
                        Shared.AppButton {
                            text: qsTr("Add action")
                            quiet: true
                            enabled: root.actionDrafts.length < 12
                            onClicked: {
                                const next = root.actionDrafts.slice()
                                next.push({ type: "show_notification", targetId: 0, url: "",
                                            message: "Automation event occurred.", delayMs: 0,
                                            enabled: true, order: next.length })
                                root.actionDrafts = next
                            }
                        }
                    }
                    Repeater {
                        model: root.actionDrafts
                        delegate: Shared.AppCard {
                            required property int index
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("Action %1").arg(index + 1); color: ThemeModule.Theme.textPrimary; font.weight: Font.DemiBold }
                                    Item { Layout.fillWidth: true }
                                    Shared.AppButton { text: "↑"; quiet: true; enabled: index > 0; Accessible.name: qsTr("Move action up"); onClicked: root.moveAction(index, -1) }
                                    Shared.AppButton { text: "↓"; quiet: true; enabled: index + 1 < root.actionDrafts.length; Accessible.name: qsTr("Move action down"); onClicked: root.moveAction(index, 1) }
                                    Shared.AppButton { text: qsTr("Remove"); quiet: true; enabled: root.actionDrafts.length > 1; onClicked: root.removeAction(index) }
                                }
                                Shared.AppComboBox {
                                    Layout.fillWidth: true
                                    accessibleLabel: qsTr("Action type")
                                    model: root.actionChoices
                                    textRole: "label"
                                    valueRole: "value"
                                    currentIndex: Math.max(0, indexOfValue(root.actionDrafts[index].type))
                                    onActivated: root.updateAction(index, "type", currentValue)
                                }
                                Shared.AppComboBox {
                                    Layout.fillWidth: true
                                    visible: root.actionDrafts[index].type === "launch_application"
                                    accessibleLabel: qsTr("Saved application target")
                                    model: root.desktopApplicationChoicesController.applications
                                    textRole: "displayName"
                                    valueRole: "id"
                                    currentIndex: Math.max(0, indexOfValue(Number(root.actionDrafts[index].targetId || 0)))
                                    onActivated: root.updateAction(index, "targetId", Number(currentValue))
                                }
                                Shared.AppComboBox {
                                    Layout.fillWidth: true
                                    visible: root.actionDrafts[index].type === "launch_setup"
                                    accessibleLabel: qsTr("Setup target")
                                    model: root.setupChoicesController.setups
                                    textRole: "name"
                                    valueRole: "id"
                                    currentIndex: Math.max(0, indexOfValue(Number(root.actionDrafts[index].targetId || 0)))
                                    onActivated: root.updateAction(index, "targetId", Number(currentValue))
                                }
                                Shared.AppTextField {
                                    Layout.fillWidth: true
                                    visible: root.actionDrafts[index].type === "open_url"
                                    placeholderText: "https://example.com"
                                    accessibleLabel: qsTr("HTTP or HTTPS URL")
                                    text: String(root.actionDrafts[index].url || "")
                                    onTextEdited: root.updateAction(index, "url", text)
                                }
                                Shared.AppTextField {
                                    Layout.fillWidth: true
                                    visible: root.actionDrafts[index].type === "show_notification"
                                    placeholderText: qsTr("Notification text, with supported placeholders")
                                    accessibleLabel: qsTr("Notification message")
                                    text: String(root.actionDrafts[index].message || "")
                                    onTextEdited: root.updateAction(index, "message", text)
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("Wait before this action (milliseconds)"); color: ThemeModule.Theme.textSecondary }
                                    SpinBox {
                                        from: 0
                                        to: 3600000
                                        stepSize: 1000
                                        value: Number(root.actionDrafts[index].delayMs || 0)
                                        editable: true
                                        onValueModified: root.updateAction(index, "delayMs", value)
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Advanced · cooldown in seconds"); color: ThemeModule.Theme.textSecondary }
                        SpinBox {
                            from: 0
                            to: 86400
                            stepSize: 5
                            value: root.draftCooldown
                            editable: true
                            onValueModified: root.draftCooldown = value
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                                text: qsTr("Placeholders depend on the trigger: task.title and task.completedAt; application.name and application.sessionId; steam.gameName, steam.priceMinor, steam.previousPriceMinor, steam.currency, steam.country, steam.newsTitle, steam.newsId, steam.releaseDate, and steam.datePrecision; schedule.localTime. Unknown or unavailable placeholders fail validation. Delayed work is canceled at application exit and is not replayed.")
                        color: ThemeModule.Theme.textSecondary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Shared.AppButton { text: qsTr("Cancel"); quiet: true; onClicked: root.editorOpen = false }
                        Shared.AppButton {
                            text: qsTr("Save rule")
                            enabled: root.controller.ready && root.draftName.trim().length > 0
                            onClicked: root.saveDraft()
                        }
                    }
                }
            }

            Shared.AppCard {
                Layout.fillWidth: true
                ColumnLayout {
                    width: parent.width
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label {
                                text: qsTr("Execution history")
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 18
                                font.weight: Font.DemiBold
                            }
                            Label {
                                text: qsTr("At most 50 recent records are shown per page.")
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 12
                            }
                        }
                        Shared.AppComboBox {
                            id: historyRuleFilter
                            Layout.preferredWidth: 220
                            accessibleLabel: qsTr("Filter history by rule")
                            model: [{ value: "", label: qsTr("All rules") }].concat(
                                (root.controller.rules || []).map(function(rule) {
                                    return { value: String(rule.id), label: String(rule.name) }
                                }))
                            textRole: "label"
                            valueRole: "value"
                            onActivated: root.loadHistoryPage(false)
                        }
                        Shared.AppComboBox {
                            Layout.preferredWidth: 160
                            accessibleLabel: qsTr("Filter history by status")
                            model: [{ value: "all", label: qsTr("All statuses") },
                                    { value: "succeeded", label: qsTr("Succeeded") },
                                    { value: "failed", label: qsTr("Failed") },
                                    { value: "canceled", label: qsTr("Canceled") },
                                    { value: "skipped", label: qsTr("Skipped") }]
                            textRole: "label"
                            valueRole: "value"
                            currentIndex: Math.max(0, indexOfValue(root.historyStatus))
                            onActivated: { root.historyStatus = currentValue; root.loadHistoryPage(false) }
                        }
                        Shared.AppComboBox {
                            Layout.preferredWidth: 225
                            accessibleLabel: qsTr("Filter history by trigger")
                            model: [{ value: "all", label: qsTr("All triggers") }].concat(root.triggerChoices)
                            textRole: "label"
                            valueRole: "value"
                            currentIndex: Math.max(0, indexOfValue(root.historyTrigger))
                            onActivated: { root.historyTrigger = currentValue; root.loadHistoryPage(false) }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Local date range"); color: ThemeModule.Theme.textSecondary }
                        Shared.AppTextField {
                            Layout.preferredWidth: 170
                            text: root.historyStartDate
                            placeholderText: qsTr("From YYYY-MM-DD")
                            accessibleLabel: qsTr("History start date in local time")
                            onTextEdited: root.historyStartDate = text
                            onEditingFinished: root.loadHistoryPage(false)
                        }
                        Shared.AppTextField {
                            Layout.preferredWidth: 170
                            text: root.historyEndDate
                            placeholderText: qsTr("Through YYYY-MM-DD")
                            accessibleLabel: qsTr("History end date in local time")
                            onTextEdited: root.historyEndDate = text
                            onEditingFinished: root.loadHistoryPage(false)
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Repeater {
                        model: root.controller.history || []
                        delegate: Shared.AppCard {
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.ruleName + " · " + root.triggerLabel(modelData.triggerType)
                                        color: ThemeModule.Theme.textPrimary
                                        font.family: ThemeModule.Theme.fontFamily
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.Wrap
                                    }
                                    Shared.StatusBadge {
                                        text: modelData.status
                                        badgeColor: modelData.status === "succeeded" ? ThemeModule.Theme.success
                                                    : modelData.status === "failed" ? ThemeModule.Theme.error
                                                    : ThemeModule.Theme.warning
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Event %1 · requested %2 · source %3 · actions completed %4 of %5")
                                            .arg(modelData.eventOccurredAtUtc || "")
                                            .arg(modelData.requestedAtUtc || "")
                                            .arg(modelData.sourceModule || "")
                                            .arg(modelData.actionsCompleted || 0)
                                            .arg(modelData.actionsAttempted || 0)
                                    color: ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Started %1 · finished %2")
                                            .arg(modelData.startedAtUtc || qsTr("not started"))
                                            .arg(modelData.finishedAtUtc || qsTr("in progress"))
                                    color: ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: String(modelData.errorMessage || "").length > 0
                                    text: modelData.errorMessage
                                    color: ThemeModule.Theme.error
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: String(modelData.actionDetails || "").length > 0
                                    text: modelData.actionDetails || ""
                                    color: ThemeModule.Theme.textSecondary
                                    font.family: ThemeModule.Theme.fontFamily
                                    font.pixelSize: 12
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Shared.AppButton {
                            text: qsTr("Load older history")
                            quiet: true
                            enabled: root.controller.historyHasMore
                            onClicked: root.loadHistoryPage(true)
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: deleteConfirmation
        modal: true
        title: qsTr("Delete automation rule?")
        standardButtons: Dialog.Yes | Dialog.No
        anchors.centerIn: Overlay.overlay
        contentItem: Label {
            text: qsTr("Execution history will remain available. The rule will stop matching new events.")
            color: ThemeModule.Theme.textPrimary
            wrapMode: Text.Wrap
            padding: 16
        }
        onAccepted: {
            root.controller.deleteRule(root.deleteRuleId)
            root.deleteRuleId = ""
        }
    }
}
