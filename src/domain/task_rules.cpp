#include "task_rules.h"

#include <QDate>
#include <QSet>

#include <utility>

namespace something::domain {

bool isValidIsoDate(const QString &value)
{
    if (value.isEmpty()) return true;
    const QDate date = QDate::fromString(value, Qt::ISODate);
    return date.isValid() && date.toString(Qt::ISODate) == value;
}

AppError normalizeTaskInput(TaskInput &input)
{
    input.title = input.title.trimmed();
    input.description = input.description.trimmed();
    // An omitted description is empty text, not SQL NULL. Both GUI and
    // console callers may supply a default-constructed (null) QString.
    if (input.description.isNull()) input.description = QStringLiteral("");
    input.dueDate = input.dueDate.trimmed();
    input.priority = input.priority.trimmed().toLower();
    if (input.priority.isEmpty()) input.priority = QStringLiteral("medium");
    if (input.difficulty) {
        *input.difficulty = input.difficulty->trimmed().toLower();
    }

    const QString difficulty = input.difficulty.value_or(QString{});
    if (input.title.isEmpty() || !isValidIsoDate(input.dueDate)
        || (input.priority != QStringLiteral("low")
            && input.priority != QStringLiteral("medium")
            && input.priority != QStringLiteral("high"))
        || (!difficulty.isEmpty() && difficulty != QStringLiteral("easy")
            && difficulty != QStringLiteral("medium")
            && difficulty != QStringLiteral("hard"))) {
        return AppError(ErrorCategory::Validation);
    }

    if (input.tags) {
        QStringList unique;
        QSet<QString> seen;
        for (QString &tag : *input.tags) {
            tag = tag.trimmed();
            if (tag.isEmpty() || tag.toUcs4().size() > 64) {
                return AppError(ErrorCategory::Validation);
            }
            const QString normalized = tag.toCaseFolded();
            if (seen.contains(normalized)) continue;
            seen.insert(normalized);
            unique.push_back(tag);
        }
        if (unique.size() > 32) return AppError(ErrorCategory::Validation);
        *input.tags = std::move(unique);
    }

    if (input.subtasks) {
        if (input.subtasks->size() > 100
            || (!input.subtasks->isEmpty() && difficulty != QStringLiteral("hard"))) {
            return AppError(ErrorCategory::Validation);
        }
        QSet<qint64> seenIds;
        for (auto &subtask : *input.subtasks) {
            subtask.title = subtask.title.trimmed();
            if (subtask.id < 0 || subtask.title.isEmpty()
                || (subtask.id > 0 && seenIds.contains(subtask.id))) {
                return AppError(ErrorCategory::Validation);
            }
            if (subtask.id > 0) seenIds.insert(subtask.id);
        }
    }

    return AppError::none();
}

AppError normalizeTaskFilter(TaskFilter &filter)
{
    filter.query = filter.query.trimmed();
    filter.completion = filter.completion.trimmed().toLower();
    filter.priority = filter.priority.trimmed().toLower();
    filter.difficulty = filter.difficulty.trimmed().toLower();
    filter.sort = filter.sort.trimmed().toLower();
    filter.direction = filter.direction.trimmed().toLower();
    filter.dueDate = filter.dueDate.trimmed();
    filter.dueFrom = filter.dueFrom.trimmed();
    filter.dueTo = filter.dueTo.trimmed();
    if (filter.completion.isEmpty()) filter.completion = QStringLiteral("all");
    if (filter.sort.isEmpty()) filter.sort = QStringLiteral("created_at");
    if (filter.direction.isEmpty()) filter.direction = QStringLiteral("desc");
    if (filter.limit == 0) filter.limit = 100;

    QStringList tags;
    QSet<QString> seen;
    for (QString tag : std::as_const(filter.tags)) {
        tag = tag.trimmed();
        if (tag.isEmpty() || tag.toUcs4().size() > 64) {
            return AppError(ErrorCategory::Validation);
        }
        const QString normalized = tag.toCaseFolded();
        if (seen.contains(normalized)) continue;
        seen.insert(normalized);
        tags.push_back(tag);
    }
    if (tags.size() > 32 || filter.query.toUcs4().size() > 256
        || filter.limit < 1 || filter.limit > 200 || filter.offset < 0
        || (filter.completion != QStringLiteral("all")
            && filter.completion != QStringLiteral("complete")
            && filter.completion != QStringLiteral("incomplete"))
        || (!filter.priority.isEmpty() && filter.priority != QStringLiteral("low")
            && filter.priority != QStringLiteral("medium")
            && filter.priority != QStringLiteral("high"))
        || (!filter.difficulty.isEmpty() && filter.difficulty != QStringLiteral("unset")
            && filter.difficulty != QStringLiteral("easy")
            && filter.difficulty != QStringLiteral("medium")
            && filter.difficulty != QStringLiteral("hard"))
        || !isValidIsoDate(filter.dueDate) || !isValidIsoDate(filter.dueFrom)
        || !isValidIsoDate(filter.dueTo)
        || (!filter.dueFrom.isEmpty() && !filter.dueTo.isEmpty()
            && filter.dueFrom > filter.dueTo)
        || (filter.overdue && filter.undated)
        || (!filter.dueDate.isEmpty()
            && (!filter.dueFrom.isEmpty() || !filter.dueTo.isEmpty()
                || filter.overdue || filter.undated))
        || (filter.undated && (!filter.dueFrom.isEmpty() || !filter.dueTo.isEmpty()))) {
        return AppError(ErrorCategory::Validation);
    }

    static const QSet<QString> sortColumns{
        QStringLiteral("created_at"), QStringLiteral("updated_at"),
        QStringLiteral("due_date"), QStringLiteral("priority"),
        QStringLiteral("title"), QStringLiteral("id")};
    if (!sortColumns.contains(filter.sort)
        || (filter.direction != QStringLiteral("asc")
            && filter.direction != QStringLiteral("desc"))) {
        return AppError(ErrorCategory::Validation);
    }
    filter.tags = std::move(tags);
    return AppError::none();
}

} // namespace something::domain
