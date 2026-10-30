#include "task_history.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <utility>

namespace something::domain {
namespace {

QJsonValue encodeValue(const TaskHistoryValue &value, bool *ok)
{
    QJsonObject encoded;
    switch (value.state) {
    case TaskHistoryValue::State::Unset:
        encoded.insert(QStringLiteral("state"), QStringLiteral("unset"));
        break;
    case TaskHistoryValue::State::Null:
        encoded.insert(QStringLiteral("state"), QStringLiteral("null"));
        break;
    case TaskHistoryValue::State::Value:
        if (value.value.isUndefined() || value.value.isNull()) {
            *ok = false;
            return {};
        }
        encoded.insert(QStringLiteral("state"), QStringLiteral("value"));
        encoded.insert(QStringLiteral("value"), value.value);
        break;
    }
    return encoded;
}

bool decodeValue(const QJsonValue &encoded, TaskHistoryValue *value)
{
    if (!encoded.isObject()) return false;
    const QJsonObject object = encoded.toObject();
    const QJsonValue state = object.value(QStringLiteral("state"));
    if (!state.isString()) return false;

    if (state.toString() == QStringLiteral("unset")) {
        if (object.size() != 1) return false;
        *value = TaskHistoryValue::unset();
        return true;
    }
    if (state.toString() == QStringLiteral("null")) {
        if (object.size() != 1) return false;
        *value = TaskHistoryValue::null();
        return true;
    }
    if (state.toString() != QStringLiteral("value") || object.size() != 2
        || !object.contains(QStringLiteral("value"))) {
        return false;
    }
    const QJsonValue raw = object.value(QStringLiteral("value"));
    if (raw.isUndefined() || raw.isNull()) return false;
    *value = TaskHistoryValue::fromJsonValue(raw);
    return true;
}

bool parseDocument(const QString &json, QJsonDocument *document)
{
    QJsonParseError error;
    *document = QJsonDocument::fromJson(json.toUtf8(), &error);
    return error.error == QJsonParseError::NoError;
}

} // namespace

QString taskHistoryEventTypeName(TaskHistoryEventType type)
{
    switch (type) {
    case TaskHistoryEventType::TaskCreated: return QStringLiteral("task_created");
    case TaskHistoryEventType::TaskUpdated: return QStringLiteral("task_updated");
    case TaskHistoryEventType::TaskTitleChanged: return QStringLiteral("task_title_changed");
    case TaskHistoryEventType::DescriptionChanged: return QStringLiteral("description_changed");
    case TaskHistoryEventType::DueDateChanged: return QStringLiteral("due_date_changed");
    case TaskHistoryEventType::PriorityChanged: return QStringLiteral("priority_changed");
    case TaskHistoryEventType::DifficultyChanged: return QStringLiteral("difficulty_changed");
    case TaskHistoryEventType::TagsChanged: return QStringLiteral("tags_changed");
    case TaskHistoryEventType::TaskCompleted: return QStringLiteral("task_completed");
    case TaskHistoryEventType::TaskReopened: return QStringLiteral("task_reopened");
    case TaskHistoryEventType::SubtaskCreated: return QStringLiteral("subtask_created");
    case TaskHistoryEventType::SubtaskUpdated: return QStringLiteral("subtask_updated");
    case TaskHistoryEventType::SubtaskCompleted: return QStringLiteral("subtask_completed");
    case TaskHistoryEventType::SubtaskReopened: return QStringLiteral("subtask_reopened");
    case TaskHistoryEventType::SubtaskRemoved: return QStringLiteral("subtask_removed");
    case TaskHistoryEventType::TaskDeleted: return QStringLiteral("task_deleted");
    }
    return {};
}

std::optional<TaskHistoryEventType> taskHistoryEventTypeFromName(const QString &name)
{
    if (name == QStringLiteral("task_created")) return TaskHistoryEventType::TaskCreated;
    if (name == QStringLiteral("task_updated")) return TaskHistoryEventType::TaskUpdated;
    if (name == QStringLiteral("task_title_changed")) return TaskHistoryEventType::TaskTitleChanged;
    if (name == QStringLiteral("description_changed")) return TaskHistoryEventType::DescriptionChanged;
    if (name == QStringLiteral("due_date_changed")) return TaskHistoryEventType::DueDateChanged;
    if (name == QStringLiteral("priority_changed")) return TaskHistoryEventType::PriorityChanged;
    if (name == QStringLiteral("difficulty_changed")) return TaskHistoryEventType::DifficultyChanged;
    if (name == QStringLiteral("tags_changed")) return TaskHistoryEventType::TagsChanged;
    if (name == QStringLiteral("task_completed")) return TaskHistoryEventType::TaskCompleted;
    if (name == QStringLiteral("task_reopened")) return TaskHistoryEventType::TaskReopened;
    if (name == QStringLiteral("subtask_created")) return TaskHistoryEventType::SubtaskCreated;
    if (name == QStringLiteral("subtask_updated")) return TaskHistoryEventType::SubtaskUpdated;
    if (name == QStringLiteral("subtask_completed")) return TaskHistoryEventType::SubtaskCompleted;
    if (name == QStringLiteral("subtask_reopened")) return TaskHistoryEventType::SubtaskReopened;
    if (name == QStringLiteral("subtask_removed")) return TaskHistoryEventType::SubtaskRemoved;
    if (name == QStringLiteral("task_deleted")) return TaskHistoryEventType::TaskDeleted;
    return std::nullopt;
}

TaskHistoryValue TaskHistoryValue::unset()
{
    return {State::Unset, {}};
}

TaskHistoryValue TaskHistoryValue::null()
{
    return {State::Null, QJsonValue(QJsonValue::Null)};
}

TaskHistoryValue TaskHistoryValue::fromJsonValue(QJsonValue value)
{
    if (value.isUndefined()) return unset();
    if (value.isNull()) return null();
    return {State::Value, std::move(value)};
}

std::optional<SerializedTaskHistoryDiff> serializeTaskHistoryDiff(
    const QVector<TaskHistoryChange> &changes)
{
    QJsonArray fields;
    QJsonObject previous;
    QJsonObject current;
    QSet<QString> seen;
    for (const TaskHistoryChange &change : changes) {
        if (change.field.isEmpty() || seen.contains(change.field)) return std::nullopt;
        seen.insert(change.field);
        bool valid = true;
        const QJsonValue oldValue = encodeValue(change.previous, &valid);
        if (!valid) return std::nullopt;
        const QJsonValue newValue = encodeValue(change.current, &valid);
        if (!valid) return std::nullopt;
        fields.push_back(change.field);
        previous.insert(change.field, oldValue);
        current.insert(change.field, newValue);
    }

    SerializedTaskHistoryDiff result;
    result.changedFieldsJson = QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact));
    result.previousValuesJson = QString::fromUtf8(QJsonDocument(previous).toJson(QJsonDocument::Compact));
    result.newValuesJson = QString::fromUtf8(QJsonDocument(current).toJson(QJsonDocument::Compact));
    return result;
}

std::optional<QVector<TaskHistoryChange>> deserializeTaskHistoryDiff(
    int version, const QString &changedFieldsJson,
    const QString &previousValuesJson, const QString &newValuesJson)
{
    if (version != SerializedTaskHistoryDiff::currentVersion) return std::nullopt;

    QJsonDocument fieldsDocument;
    QJsonDocument previousDocument;
    QJsonDocument newDocument;
    if (!parseDocument(changedFieldsJson, &fieldsDocument)
        || !parseDocument(previousValuesJson, &previousDocument)
        || !parseDocument(newValuesJson, &newDocument)
        || !fieldsDocument.isArray() || !previousDocument.isObject() || !newDocument.isObject()) {
        return std::nullopt;
    }

    const QJsonArray fields = fieldsDocument.array();
    const QJsonObject previous = previousDocument.object();
    const QJsonObject current = newDocument.object();
    if (previous.size() != fields.size() || current.size() != fields.size()) return std::nullopt;

    QVector<TaskHistoryChange> result;
    result.reserve(fields.size());
    QSet<QString> seen;
    for (const QJsonValue &fieldValue : fields) {
        if (!fieldValue.isString()) return std::nullopt;
        const QString field = fieldValue.toString();
        if (field.isEmpty() || seen.contains(field)
            || !previous.contains(field) || !current.contains(field)) {
            return std::nullopt;
        }
        seen.insert(field);
        TaskHistoryChange change;
        change.field = field;
        if (!decodeValue(previous.value(field), &change.previous)
            || !decodeValue(current.value(field), &change.current)) {
            return std::nullopt;
        }
        result.push_back(std::move(change));
    }
    return result;
}

} // namespace something::domain
