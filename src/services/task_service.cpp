#include "task_service.h"

#include "../domain/task_date_rules.h"

#include <QDate>
#include <QVariantList>
#include <algorithm>

#include <utility>

namespace something::services {
namespace {

storage::StorageReply errorReply(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

QVariantList taskRows(const QVector<storage::Task> &tasks)
{
    QVariantList rows;
    rows.reserve(tasks.size());
    for (const auto &task : tasks) rows.push_back(TaskService::toVariantMap(task));
    return rows;
}

storage::RepositoryResult<QVector<storage::Task>> queryTasks(
    storage::TaskRepository &repository,
    const std::function<bool()> &cancelled,
    storage::TaskFilter filter)
{
    if (cancelled()) {
        storage::RepositoryResult<QVector<storage::Task>> result;
        result.error = domain::AppError::cancelled();
        return result;
    }
    return repository.list(filter);
}

storage::TaskFilter incompleteFilter()
{
    storage::TaskFilter filter;
    filter.completion = QStringLiteral("incomplete");
    filter.limit = 200;
    filter.offset = 0;
    return filter;
}

} // namespace

storage::StorageCommand TaskService::calendarDate(QDate date)
{
    return [date](storage::StorageRepositories &repositories,
                  const std::function<bool()> &cancelled) {
        if (!date.isValid())
            return errorReply(domain::AppError(domain::ErrorCategory::Validation));

        QVector<storage::Task> tasks;
        storage::TaskFilter filter;
        filter.dueDate = date.toString(Qt::ISODate);
        filter.limit = 200;
        for (int offset = 0; ; offset += 200) {
            filter.offset = offset;
            const auto result = queryTasks(repositories.tasks, cancelled, filter);
            if (!result.succeeded()) return errorReply(result.error);
            const int returned = result.value.size();
            tasks += result.value;
            if (returned < filter.limit) break;
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("tasks"), taskRows(tasks));
        return reply;
    };
}

storage::StorageCommand TaskService::calendarDatePage(QDate date, int offset, int pageSize)
{
    return [date, offset, pageSize](storage::StorageRepositories &repositories,
                                    const std::function<bool()> &cancelled) {
        if (!date.isValid() || offset < 0 || offset > 50000000 || pageSize < 1 || pageSize > 100)
            return errorReply(domain::AppError(domain::ErrorCategory::Validation));
        storage::TaskFilter filter;
        filter.dueDate = date.toString(Qt::ISODate);
        filter.offset = offset;
        filter.limit = pageSize + 1;
        const auto result = queryTasks(repositories.tasks, cancelled, filter);
        if (!result.succeeded()) return errorReply(result.error);
        QVariantList rows = taskRows(result.value);
        const bool hasMore = rows.size() > pageSize;
        if (hasMore) rows.removeLast();
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("tasks"), rows);
        reply.values.insert(QStringLiteral("hasMore"), hasMore);
        return reply;
    };
}

storage::StorageCommand TaskService::dashboardToday(QDate today,
                                                     bool includeOverdue,
                                                     bool includeUndated)
{
    return [today, includeOverdue, includeUndated](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (!today.isValid())
            return errorReply(domain::AppError(domain::ErrorCategory::Validation));

        auto todayFilter = incompleteFilter();
        todayFilter.dueDate = today.toString(Qt::ISODate);
        todayFilter.sort = QStringLiteral("priority");
        todayFilter.direction = QStringLiteral("asc");
        const auto dueToday = queryTasks(repositories.tasks, cancelled, todayFilter);
        if (!dueToday.succeeded()) return errorReply(dueToday.error);

        QVector<storage::Task> overdue;
        if (includeOverdue) {
            auto overdueFilter = incompleteFilter();
            overdueFilter.dueTo = today.addDays(-1).toString(Qt::ISODate);
            overdueFilter.sort = QStringLiteral("due_date");
            overdueFilter.direction = QStringLiteral("asc");
            const auto result = queryTasks(repositories.tasks, cancelled, overdueFilter);
            if (!result.succeeded()) return errorReply(result.error);
            overdue = result.value;
        }

        QVector<storage::Task> undated;
        if (includeUndated) {
            auto undatedFilter = incompleteFilter();
            undatedFilter.undated = true;
            undatedFilter.sort = QStringLiteral("priority");
            undatedFilter.direction = QStringLiteral("asc");
            const auto result = queryTasks(repositories.tasks, cancelled, undatedFilter);
            if (!result.succeeded()) return errorReply(result.error);
            undated = result.value;
        }

        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("todayDate"), today.toString(Qt::ISODate));
        reply.values.insert(QStringLiteral("dueToday"), taskRows(dueToday.value));
        reply.values.insert(QStringLiteral("overdue"), taskRows(overdue));
        reply.values.insert(QStringLiteral("undated"), taskRows(undated));
        return reply;
    };
}

storage::StorageCommand TaskService::dashboardWeek(QDate today,
                                                    bool includeOverdue,
                                                    bool includeUndated)
{
    return [today, includeOverdue, includeUndated](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (!today.isValid())
            return errorReply(domain::AppError(domain::ErrorCategory::Validation));
        const auto preference = repositories.preferences.weekStart();
        if (!preference.succeeded()) return errorReply(preference.error);
        const domain::DashboardWeekRange range =
            domain::dashboardWeekRange(today, preference.value);

        QVector<storage::Task> items;
        if (range.from <= range.through) {
            auto filter = incompleteFilter();
            filter.dueFrom = range.from.toString(Qt::ISODate);
            filter.dueTo = range.through.toString(Qt::ISODate);
            filter.sort = QStringLiteral("due_date");
            filter.direction = QStringLiteral("asc");
            const auto result = queryTasks(repositories.tasks, cancelled, filter);
            if (!result.succeeded()) return errorReply(result.error);
            items = result.value;
            const auto priorityRank = [](const QString &priority) {
                if (priority == QStringLiteral("high")) return 0;
                if (priority == QStringLiteral("medium")) return 1;
                if (priority == QStringLiteral("low")) return 2;
                return 3;
            };
            std::stable_sort(items.begin(), items.end(), [&](const auto &left, const auto &right) {
                if (left.dueDate != right.dueDate) return left.dueDate < right.dueDate;
                const int leftPriority = priorityRank(left.priority);
                const int rightPriority = priorityRank(right.priority);
                if (leftPriority != rightPriority) return leftPriority < rightPriority;
                return left.id < right.id;
            });
        }

        QVector<storage::Task> overdue;
        if (includeOverdue) {
            auto filter = incompleteFilter();
            filter.dueTo = today.addDays(-1).toString(Qt::ISODate);
            filter.sort = QStringLiteral("due_date");
            filter.direction = QStringLiteral("asc");
            const auto result = queryTasks(repositories.tasks, cancelled, filter);
            if (!result.succeeded()) return errorReply(result.error);
            overdue = result.value;
        }

        QVector<storage::Task> undated;
        if (includeUndated) {
            auto filter = incompleteFilter();
            filter.undated = true;
            filter.sort = QStringLiteral("priority");
            filter.direction = QStringLiteral("asc");
            const auto result = queryTasks(repositories.tasks, cancelled, filter);
            if (!result.succeeded()) return errorReply(result.error);
            undated = result.value;
        }

        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("fromDate"), range.from.toString(Qt::ISODate));
        reply.values.insert(QStringLiteral("throughDate"), range.through.toString(Qt::ISODate));
        reply.values.insert(QStringLiteral("weekStart"), range.weekStartSundayZero);
        reply.values.insert(QStringLiteral("items"), taskRows(items));
        reply.values.insert(QStringLiteral("overdue"), taskRows(overdue));
        reply.values.insert(QStringLiteral("undated"), taskRows(undated));
        return reply;
    };
}

QVariantMap TaskService::toVariantMap(const storage::Task &task)
{
    QVariantList subtasks;
    subtasks.reserve(task.subtasks.size());
    for (const storage::Subtask &subtask : task.subtasks) {
        subtasks.push_back(QVariantMap{
            {QStringLiteral("id"), subtask.id},
            {QStringLiteral("title"), subtask.title},
            {QStringLiteral("completed"), subtask.completed},
            {QStringLiteral("position"), subtask.position},
        });
    }
    return {
        {QStringLiteral("id"), task.id},
        {QStringLiteral("title"), task.title},
        {QStringLiteral("description"), task.description},
        {QStringLiteral("completed"), task.completed},
        {QStringLiteral("createdAt"), task.createdAt},
        {QStringLiteral("updatedAt"), task.updatedAt},
        {QStringLiteral("dueDate"), task.dueDate},
        {QStringLiteral("priority"), task.priority},
        {QStringLiteral("difficulty"), task.difficulty},
        {QStringLiteral("revision"), task.revision},
        {QStringLiteral("tags"), task.tags},
        {QStringLiteral("subtasks"), subtasks},
    };
}

storage::Task TaskService::fromVariantMap(const QVariantMap &map)
{
    storage::Task task;
    task.id = map.value(QStringLiteral("id")).toLongLong();
    task.title = map.value(QStringLiteral("title")).toString();
    task.description = map.value(QStringLiteral("description")).toString();
    task.completed = map.value(QStringLiteral("completed")).toBool();
    task.createdAt = map.value(QStringLiteral("createdAt")).toString();
    task.updatedAt = map.value(QStringLiteral("updatedAt")).toString();
    task.dueDate = map.value(QStringLiteral("dueDate")).toString();
    task.priority = map.value(QStringLiteral("priority")).toString();
    task.difficulty = map.value(QStringLiteral("difficulty")).toString();
    task.revision = map.value(QStringLiteral("revision")).toLongLong();
    task.tags = map.value(QStringLiteral("tags")).toStringList();
    const QVariantList subtasks = map.value(QStringLiteral("subtasks")).toList();
    task.subtasks.reserve(subtasks.size());
    for (const QVariant &value : subtasks) {
        const QVariantMap row = value.toMap();
        task.subtasks.push_back({row.value(QStringLiteral("id")).toLongLong(),
                                 row.value(QStringLiteral("title")).toString(),
                                 row.value(QStringLiteral("completed")).toBool(),
                                 row.value(QStringLiteral("position")).toInt()});
    }
    return task;
}

storage::StorageCommand TaskService::list(storage::TaskFilter filter, int pageSize)
{
    return [filter = std::move(filter), pageSize](storage::StorageRepositories &repositories,
                                                  const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        if (pageSize < 1 || pageSize > 199) {
            return errorReply(domain::AppError(domain::ErrorCategory::Validation));
        }
        filter.limit = pageSize + 1;
        const auto result = repositories.tasks.list(filter);
        if (!result.succeeded()) return errorReply(result.error);
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        QVector<storage::Task> page = result.value;
        const bool hasMore = page.size() > pageSize;
        if (hasMore) page.resize(pageSize);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("tasks"), taskRows(page));
        reply.values.insert(QStringLiteral("hasMore"), hasMore);
        return reply;
    };
}

storage::StorageCommand TaskService::get(qint64 id)
{
    return [id](storage::StorageRepositories &repositories,
                const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.tasks.get(id);
        if (!result.succeeded()) return errorReply(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("task"), TaskService::toVariantMap(result.value));
        return reply;
    };
}

storage::StorageCommand TaskService::create(storage::TaskInput input)
{
    return [input = std::move(input)](storage::StorageRepositories &repositories,
                                      const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.tasks.create(input);
        if (!result.succeeded()) return errorReply(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("task"), TaskService::toVariantMap(result.value));
        return reply;
    };
}

storage::StorageCommand TaskService::update(qint64 id, qint64 expectedRevision,
                                            storage::TaskInput input)
{
    return [id, expectedRevision, input = std::move(input)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.tasks.update(id, expectedRevision, input);
        if (!result.succeeded()) return errorReply(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("task"), TaskService::toVariantMap(result.value));
        return reply;
    };
}

storage::StorageCommand TaskService::setCompleted(qint64 id, qint64 expectedRevision,
                                                  bool completed)
{
    return [id, expectedRevision, completed](storage::StorageRepositories &repositories,
                                             const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.tasks.setCompleted(id, expectedRevision, completed);
        if (!result.succeeded()) return errorReply(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("task"), TaskService::toVariantMap(result.value));
        return reply;
    };
}

storage::StorageCommand TaskService::setSubtaskCompleted(qint64 id, qint64 subtaskId,
                                                         qint64 expectedRevision,
                                                         bool completed)
{
    return [id, subtaskId, expectedRevision, completed](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.tasks.setSubtaskCompleted(
            id, subtaskId, expectedRevision, completed);
        if (!result.succeeded()) return errorReply(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("task"), TaskService::toVariantMap(result.value));
        return reply;
    };
}

storage::StorageCommand TaskService::remove(qint64 id, qint64 expectedRevision)
{
    return [id, expectedRevision](storage::StorageRepositories &repositories,
                                  const std::function<bool()> &cancelled) {
        if (cancelled()) return errorReply(domain::AppError::cancelled());
        const auto result = repositories.tasks.remove(id, expectedRevision);
        if (!result.succeeded()) return errorReply(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("deletedId"), id);
        return reply;
    };
}

} // namespace something::services
