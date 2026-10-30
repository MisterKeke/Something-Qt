#pragma once

#include <QJsonValue>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <optional>

namespace something::domain {

enum class TaskHistoryEventType {
    TaskCreated,
    TaskUpdated,
    TaskTitleChanged,
    DescriptionChanged,
    DueDateChanged,
    PriorityChanged,
    DifficultyChanged,
    TagsChanged,
    TaskCompleted,
    TaskReopened,
    SubtaskCreated,
    SubtaskUpdated,
    SubtaskCompleted,
    SubtaskReopened,
    SubtaskRemoved,
    TaskDeleted,
};

inline constexpr int taskHistorySerializationVersion = 1;

QString taskHistoryEventTypeName(TaskHistoryEventType type);
std::optional<TaskHistoryEventType> taskHistoryEventTypeFromName(const QString &name);

struct TaskHistoryValue final {
    enum class State { Unset, Null, Value };

    State state{State::Unset};
    QJsonValue value;

    static TaskHistoryValue unset();
    static TaskHistoryValue null();
    static TaskHistoryValue fromJsonValue(QJsonValue value);
};

struct TaskHistoryChange final {
    QString field;
    TaskHistoryValue previous;
    TaskHistoryValue current;
};

struct TaskHistoryEvent final {
    qint64 eventId{0};
    qint64 taskId{0};
    TaskHistoryEventType type{TaskHistoryEventType::TaskUpdated};
    QString occurredAtUtc; // ISO 8601 timestamp in UTC, ending in Z
    std::optional<qint64> taskRevision;
    QString taskTitle;
    int serializationVersion{taskHistorySerializationVersion};
    QVector<TaskHistoryChange> changes;
};

struct SerializedTaskHistoryDiff final {
    static constexpr int currentVersion = taskHistorySerializationVersion;

    int version{currentVersion};
    QString changedFieldsJson;
    QString previousValuesJson;
    QString newValuesJson;
};

// Version 1 stores field names as a JSON string array and each value map as a
// JSON object. Canonical task fields are title, description, dueDate, priority,
// difficulty, tags, and completed; full subtask-list changes use "subtasks",
// and focused subtask snapshots/properties use "subtasks.<id>" or
// "subtasks.<id>.<property>". Values use {"state":"unset"},
// {"state":"null"}, or {"state":"value","value":...}, so unset,
// SQL NULL, and empty values remain distinct (including empty strings, arrays,
// and objects).
std::optional<SerializedTaskHistoryDiff> serializeTaskHistoryDiff(
    const QVector<TaskHistoryChange> &changes);
std::optional<QVector<TaskHistoryChange>> deserializeTaskHistoryDiff(
    int version, const QString &changedFieldsJson,
    const QString &previousValuesJson, const QString &newValuesJson);

} // namespace something::domain
