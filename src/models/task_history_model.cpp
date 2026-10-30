#include "task_history_model.h"

#include <QSet>

#include <utility>

namespace something::models {

TaskHistoryModel::TaskHistoryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TaskHistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_events.size();
}

QVariant TaskHistoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_events.size()) return {};
    const QVariantMap &event = m_events.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case EventTypeRole: return event.value(QStringLiteral("eventType"));
    case EventIdRole: return event.value(QStringLiteral("eventId"));
    case TaskIdRole: return event.value(QStringLiteral("taskId"));
    case OccurredAtUtcRole: return event.value(QStringLiteral("occurredAtUtc"));
    case TaskRevisionRole: return event.value(QStringLiteral("taskRevision"));
    case TaskTitleRole: return event.value(QStringLiteral("taskTitle"));
    case SerializationVersionRole: return event.value(QStringLiteral("serializationVersion"));
    case ChangesRole: return event.value(QStringLiteral("changes"));
    default: return {};
    }
}

QHash<int, QByteArray> TaskHistoryModel::roleNames() const
{
    return {{EventIdRole, "eventId"}, {TaskIdRole, "taskId"},
            {EventTypeRole, "eventType"}, {OccurredAtUtcRole, "occurredAtUtc"},
            {TaskRevisionRole, "taskRevision"}, {TaskTitleRole, "taskTitle"},
            {SerializationVersionRole, "serializationVersion"}, {ChangesRole, "changes"}};
}

QVariantMap TaskHistoryModel::eventAt(int row) const
{
    return row >= 0 && row < m_events.size() ? m_events.at(row) : QVariantMap{};
}

void TaskHistoryModel::replace(QVariantList events)
{
    QVector<QVariantMap> next;
    next.reserve(events.size());
    for (const QVariant &value : events) next.push_back(value.toMap());
    if (next == m_events) return;
    const int previousCount = m_events.size();
    beginResetModel();
    m_events = std::move(next);
    endResetModel();
    if (previousCount != m_events.size()) emit countChanged();
}

void TaskHistoryModel::appendOlder(QVariantList events)
{
    QSet<qint64> existing;
    existing.reserve(m_events.size() + events.size());
    for (const auto &event : m_events)
        existing.insert(event.value(QStringLiteral("eventId")).toLongLong());

    QVector<QVariantMap> older;
    older.reserve(events.size());
    for (const QVariant &value : events) {
        QVariantMap event = value.toMap();
        const qint64 id = event.value(QStringLiteral("eventId")).toLongLong();
        if (id <= 0 || existing.contains(id)) continue;
        existing.insert(id);
        older.push_back(std::move(event));
    }
    if (older.isEmpty()) return;
    const int first = m_events.size();
    const int last = first + older.size() - 1;
    beginInsertRows({}, first, last);
    m_events += older;
    endInsertRows();
    emit countChanged();
}

} // namespace something::models
