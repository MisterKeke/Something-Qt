#include "task_history_service.h"

#include <utility>

namespace something::services {
namespace {

storage::StorageReply errorReply(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

storage::StorageReply pageReply(const storage::TaskHistoryPage &page)
{
    QVariantList events;
    events.reserve(page.events.size());
    for (const auto &event : page.events)
        events.push_back(TaskHistoryService::toVariantMap(event));

    storage::StorageReply reply;
    reply.values.insert(QStringLiteral("events"), events);
    reply.values.insert(QStringLiteral("hasMore"), page.hasMore);
    reply.values.insert(QStringLiteral("nextOccurredAtUtc"), page.nextCursor.occurredAtUtc);
    reply.values.insert(QStringLiteral("nextEventId"), page.nextCursor.eventId);
    return reply;
}

QString valueState(domain::TaskHistoryValue::State state)
{
    switch (state) {
    case domain::TaskHistoryValue::State::Unset: return QStringLiteral("unset");
    case domain::TaskHistoryValue::State::Null: return QStringLiteral("null");
    case domain::TaskHistoryValue::State::Value: return QStringLiteral("value");
    }
    return {};
}

QVariant valueVariant(const domain::TaskHistoryValue &value)
{
    return value.state == domain::TaskHistoryValue::State::Value
        ? value.value.toVariant() : QVariant{};
}

} // namespace

storage::StorageCommand TaskHistoryService::forTask(
    qint64 taskId, std::optional<domain::TaskHistoryEventType> eventType,
    int pageSize, storage::TaskHistoryCursor before)
{
    return [taskId, eventType, pageSize, before](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.taskHistory.forTask(taskId, eventType, pageSize, before);
        if (!result.succeeded()) return errorReply(result.error);
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        return pageReply(result.value);
    };
}

storage::StorageCommand TaskHistoryService::recent(
    QString taskTitleQuery, std::optional<domain::TaskHistoryEventType> eventType,
    int pageSize, storage::TaskHistoryCursor before)
{
    return [taskTitleQuery = std::move(taskTitleQuery), eventType, pageSize, before](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.taskHistory.recent(
            taskTitleQuery, eventType, pageSize, before);
        if (!result.succeeded()) return errorReply(result.error);
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        return pageReply(result.value);
    };
}

QVariantMap TaskHistoryService::toVariantMap(const domain::TaskHistoryEvent &event)
{
    QVariantList changes;
    changes.reserve(event.changes.size());
    for (const auto &change : event.changes) {
        changes.push_back(QVariantMap{
            {QStringLiteral("field"), change.field},
            {QStringLiteral("previousState"), valueState(change.previous.state)},
            {QStringLiteral("previousValue"), valueVariant(change.previous)},
            {QStringLiteral("currentState"), valueState(change.current.state)},
            {QStringLiteral("currentValue"), valueVariant(change.current)},
        });
    }
    QVariant revision;
    if (event.taskRevision) revision = *event.taskRevision;
    return {
        {QStringLiteral("eventId"), event.eventId},
        {QStringLiteral("taskId"), event.taskId},
        {QStringLiteral("eventType"), domain::taskHistoryEventTypeName(event.type)},
        {QStringLiteral("occurredAtUtc"), event.occurredAtUtc},
        {QStringLiteral("taskRevision"), revision},
        {QStringLiteral("taskTitle"), event.taskTitle},
        {QStringLiteral("serializationVersion"), event.serializationVersion},
        {QStringLiteral("changes"), changes},
    };
}

} // namespace something::services
