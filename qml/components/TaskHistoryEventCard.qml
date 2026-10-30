import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Something.App as ThemeModule

Rectangle {
    id: root

    property string eventType: ""
    property string occurredAtUtc: ""
    property string taskTitle: ""
    property var changes: []
    property bool showTaskTitle: true
    property bool detailsExpanded: false
    readonly property string eventHeading: headingForEvent()
    readonly property bool hasLongValues: changes.some(function(change) {
        const field = String(change.field || "")
        if (field === "description")
            return String(change.previousValue || "").length > 180
                    || String(change.currentValue || "").length > 180
        return (field === "subtasks" || field.indexOf("subtasks.") === 0)
                && (JSON.stringify(change.previousValue || []).length > 180
                    || JSON.stringify(change.currentValue || []).length > 180)
    })

    function eventLabel() {
        switch (eventType) {
        case "task_created": return qsTr("Task created")
        case "task_updated": return qsTr("Task updated")
        case "task_title_changed": return qsTr("Task title changed")
        case "description_changed": return qsTr("Description changed")
        case "due_date_changed": return qsTr("Due date changed")
        case "priority_changed": return qsTr("Priority changed")
        case "difficulty_changed": return qsTr("Difficulty changed")
        case "tags_changed": return qsTr("Tags changed")
        case "task_completed": return qsTr("Task completed")
        case "task_reopened": return qsTr("Task reopened")
        case "subtask_created": return qsTr("Subtask created")
        case "subtask_updated": return qsTr("Subtask updated")
        case "subtask_completed": return qsTr("Subtask completed")
        case "subtask_reopened": return qsTr("Subtask reopened")
        case "subtask_removed": return qsTr("Subtask removed")
        case "task_deleted": return qsTr("Task deleted")
        default: return qsTr("Task changed")
        }
    }

    function subtaskTitleFromChange(change) {
        const field = String(change.field || "")
        if (field === "subtasks") {
            const before = Array.isArray(change.previousValue) ? change.previousValue : []
            const after = Array.isArray(change.currentValue) ? change.currentValue : []
            if (eventType === "subtask_removed") {
                for (let i = 0; i < before.length; ++i) {
                    const oldItem = before[i]
                    if (!after.some(function(item) { return String(item.id) === String(oldItem.id) }))
                        return String(oldItem.title || "")
                }
            }
            if (eventType === "subtask_created") {
                for (let i = 0; i < after.length; ++i) {
                    const newItem = after[i]
                    if (!before.some(function(item) { return String(item.id) === String(newItem.id) }))
                        return String(newItem.title || "")
                }
            }
            for (let i = 0; i < after.length; ++i) {
                const next = after[i]
                const previous = before.find(function(item) { return String(item.id) === String(next.id) })
                if (!previous || previous.title !== next.title
                        || previous.completed !== next.completed
                        || previous.position !== next.position)
                    return String(next.title || "")
            }
            for (let i = 0; i < before.length; ++i) {
                const previous = before[i]
                if (!after.some(function(item) { return String(item.id) === String(previous.id) }))
                    return String(previous.title || "")
            }
        }
        if (field.indexOf("subtasks.") === 0) {
            const current = change.currentValue
            const previous = change.previousValue
            if (current && typeof current === "object" && current.title)
                return String(current.title)
            if (previous && typeof previous === "object" && previous.title)
                return String(previous.title)
            const pieces = field.split(".")
            return pieces.length > 1 ? qsTr("Subtask #%1").arg(pieces[1]) : qsTr("Subtask")
        }
        return ""
    }

    function headingForEvent() {
        const label = eventLabel()
        if (eventType.indexOf("subtask_") !== 0) return label
        for (let i = 0; i < changes.length; ++i) {
            const subtaskTitle = subtaskTitleFromChange(changes[i])
            if (subtaskTitle.length > 0) return qsTr("%1: %2").arg(label).arg(subtaskTitle)
        }
        return label
    }

    function localTimestamp(value) {
        const date = new Date(value)
        if (isNaN(date.getTime())) return value
        return Qt.formatDateTime(date, "dd MMM yyyy, HH:mm")
    }

    function fieldLabel(field) {
        if (field === "title") return qsTr("Title")
        if (field === "description") return qsTr("Description")
        if (field === "dueDate") return qsTr("Due date")
        if (field === "plannedDate") return qsTr("Planned work date")
        if (field === "plannedStartTime") return qsTr("Planned start time")
        if (field === "estimatedDurationMinutes") return qsTr("Estimated duration")
        if (field === "priority") return qsTr("Priority")
        if (field === "difficulty") return qsTr("Difficulty")
        if (field === "completed") return qsTr("Completion")
        if (field === "tags") return qsTr("Tags")
        if (field === "subtasks") return qsTr("Subtasks")
        if (field.indexOf("subtasks.") === 0) return qsTr("Subtask")
        return field
    }

    function valueText(state, value, field) {
        if (state === "unset") return qsTr("(unset)")
        if (state === "null" || value === null || typeof value === "undefined")
            return qsTr("(none)")
        if (field === "completed" || field.endsWith(".completed"))
            return value ? qsTr("Completed") : qsTr("Pending")
        if (field === "dueDate") {
            const rawDate = String(value)
            if (/^\d{4}-\d{2}-\d{2}$/.test(rawDate))
                return Qt.formatDate(new Date(rawDate + "T00:00:00"), "MMM d")
        }
        if (field === "plannedDate") {
            const plannedDate = String(value)
            if (/^\d{4}-\d{2}-\d{2}$/.test(plannedDate))
                return Qt.formatDate(new Date(plannedDate + "T00:00:00"), "MMM d, yyyy")
        }
        if (field === "estimatedDurationMinutes")
            return qsTr("%1 min").arg(Number(value))
        if (Array.isArray(value)) {
            if (field === "tags") return value.length ? value.join(", ") : qsTr("(none)")
            if (field === "subtasks") {
                if (!value.length) return qsTr("(none)")
                return value.map(function(item) {
                    const status = item.completed ? qsTr("Completed") : qsTr("Pending")
                    return qsTr("%1 (%2)").arg(String(item.title || "")).arg(status)
                }).join("; ")
            }
            return value.join(", ")
        }
        if (value && typeof value === "object") {
            if (value.title) {
                const status = typeof value.completed === "boolean"
                        ? (value.completed ? qsTr("Completed") : qsTr("Pending")) : ""
                return status.length > 0
                        ? qsTr("%1 (%2)").arg(String(value.title)).arg(status)
                        : String(value.title)
            }
            return JSON.stringify(value)
        }
        const text = String(value)
        if (field === "priority" || field === "difficulty")
            return text.length ? text.charAt(0).toUpperCase() + text.slice(1) : qsTr("(none)")
        return text.length ? text : qsTr("(empty)")
    }

    function changeSummary(change) {
        const field = String(change.field || "")
        const previous = valueText(change.previousState, change.previousValue, field)
        const current = valueText(change.currentState, change.currentValue, field)
        const impliedFields = {
            task_title_changed: "title",
            description_changed: "description",
            due_date_changed: "dueDate",
            priority_changed: "priority",
            difficulty_changed: "difficulty",
            tags_changed: "tags",
            task_completed: "completed",
            task_reopened: "completed"
        }
        const impliedField = impliedFields[eventType]
        if (impliedField === field
                || (eventType.indexOf("subtask_") === 0
                    && (field === "subtasks" || field.indexOf("subtasks.") === 0)))
            return qsTr("%1 → %2").arg(previous).arg(current)

        let label = fieldLabel(field)
        if (field.indexOf("subtasks.") === 0 && field.endsWith(".completed"))
            label = qsTr("Subtask completion")
        return qsTr("%1: %2 → %3").arg(label).arg(previous).arg(current)
    }

    function iconForEvent() {
        if (eventType === "task_created" || eventType === "subtask_created") return "+"
        if (eventType === "task_deleted" || eventType === "subtask_removed") return "−"
        if (eventType.indexOf("completed") >= 0) return "✓"
        if (eventType.indexOf("reopened") >= 0) return "↶"
        return "↻"
    }

    function colorForEvent() {
        if (eventType === "task_deleted" || eventType === "subtask_removed")
            return ThemeModule.Theme.error
        if (eventType.indexOf("completed") >= 0) return ThemeModule.Theme.success
        if (eventType.indexOf("created") >= 0) return ThemeModule.Theme.accentCyan
        return ThemeModule.Theme.focusHighlight
    }

    implicitHeight: content.implicitHeight + 24
    color: ThemeModule.Theme.panelSurface
    border.color: ThemeModule.Theme.borderColor
    border.width: ThemeModule.Theme.panelBorderWidth
    radius: ThemeModule.Theme.panelRadius

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 12
        spacing: 11

        Rectangle {
            Layout.alignment: Qt.AlignTop
            implicitWidth: 34
            implicitHeight: 34
            radius: ThemeModule.Theme.badgeRadius
            color: ThemeModule.Theme.insetSurface
            border.color: root.colorForEvent()
            border.width: 1
            Label {
                anchors.centerIn: parent
                text: root.iconForEvent()
                color: root.colorForEvent()
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 17
                font.weight: Font.Bold
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5

            Label {
                Layout.fillWidth: true
                text: root.eventHeading
                color: ThemeModule.Theme.textPrimary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 14
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Label {
                visible: root.showTaskTitle && root.taskTitle.length > 0
                Layout.fillWidth: true
                text: qsTr("Task: %1").arg(root.taskTitle)
                color: ThemeModule.Theme.accentCyanLight
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 11
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Label {
                Layout.fillWidth: true
                text: root.localTimestamp(root.occurredAtUtc)
                color: ThemeModule.Theme.textSecondary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: 11
                textFormat: Text.PlainText
            }
            Repeater {
                model: root.changes
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    text: root.changeSummary(modelData)
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    maximumLineCount: root.detailsExpanded ? 0 : 3
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
            }
            Button {
                visible: root.hasLongValues
                text: root.detailsExpanded ? qsTr("Hide full details") : qsTr("Show full details")
                padding: 0
                leftPadding: 0
                rightPadding: 0
                implicitHeight: 24
                contentItem: Text {
                    text: parent.text
                    color: parent.hovered ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.focusHighlight
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 11
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { color: "transparent" }
                onClicked: root.detailsExpanded = !root.detailsExpanded
                Accessible.name: text
            }
        }
    }
}
