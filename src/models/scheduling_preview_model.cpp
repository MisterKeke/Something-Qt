#include "scheduling_preview_model.h"

#include <QByteArray>
#include <QVariantMap>

#include <utility>

namespace something::models {

SchedulingPreviewModel::SchedulingPreviewModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int SchedulingPreviewModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant SchedulingPreviewModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const QVariantMap row = m_rows.at(index.row()).toMap();
    switch (role) {
    case TaskIdRole: return row.value(QStringLiteral("taskId"));
    case TitleRole: return row.value(QStringLiteral("title"));
    case DueDateRole: return row.value(QStringLiteral("dueDate"));
    case PriorityRole: return row.value(QStringLiteral("priority"));
    case DifficultyRole: return row.value(QStringLiteral("difficulty"));
    case EstimatedDurationMinutesRole: return row.value(QStringLiteral("estimatedDurationMinutes"));
    case PlannedDateRole: return row.value(QStringLiteral("plannedDate"));
    case PlannedStartTimeRole: return row.value(QStringLiteral("plannedStartTime"));
    case ScoreRole: return row.value(QStringLiteral("score"));
    case StateRole: return row.value(QStringLiteral("state"));
    case OverdueRole: return row.value(QStringLiteral("overdue"));
    case UsedDefaultDurationRole: return row.value(QStringLiteral("usedDefaultDuration"));
    case PreviouslyScheduledRole: return row.value(QStringLiteral("previouslyScheduled"));
    case RetainedExistingScheduleRole: return row.value(QStringLiteral("retainedExistingSchedule"));
    case HasConflictRole: return row.value(QStringLiteral("hasConflict"));
    case OverloadedRole: return row.value(QStringLiteral("overloaded"));
    case ReasonRole: return row.value(QStringLiteral("reason"));
    case WarningsRole: return row.value(QStringLiteral("warnings"));
    case TaskRevisionRole: return row.value(QStringLiteral("taskRevision"));
    case ScheduleRevisionRole: return row.value(QStringLiteral("scheduleRevision"));
    case CanAcceptRole: return row.value(QStringLiteral("canAccept"));
    case AcceptedRole: return row.value(QStringLiteral("accepted"));
    default: return {};
    }
}

QHash<int, QByteArray> SchedulingPreviewModel::roleNames() const
{
    return {
        {TaskIdRole, "taskId"},
        {TitleRole, "title"},
        {DueDateRole, "dueDate"},
        {PriorityRole, "priority"},
        {DifficultyRole, "difficulty"},
        {EstimatedDurationMinutesRole, "estimatedDurationMinutes"},
        {PlannedDateRole, "plannedDate"},
        {PlannedStartTimeRole, "plannedStartTime"},
        {ScoreRole, "score"},
        {StateRole, "state"},
        {OverdueRole, "overdue"},
        {UsedDefaultDurationRole, "usedDefaultDuration"},
        {PreviouslyScheduledRole, "previouslyScheduled"},
        {RetainedExistingScheduleRole, "retainedExistingSchedule"},
        {HasConflictRole, "hasConflict"},
        {OverloadedRole, "overloaded"},
        {ReasonRole, "reason"},
        {WarningsRole, "warnings"},
        {TaskRevisionRole, "taskRevision"},
        {ScheduleRevisionRole, "scheduleRevision"},
        {CanAcceptRole, "canAccept"},
        {AcceptedRole, "accepted"},
    };
}

int SchedulingPreviewModel::acceptedCount() const noexcept
{
    int count = 0;
    for (const QVariant &value : m_rows)
        if (value.toMap().value(QStringLiteral("accepted")).toBool()) ++count;
    return count;
}

int SchedulingPreviewModel::validCount() const noexcept
{
    int count = 0;
    for (const QVariant &value : m_rows)
        if (canAccept(value.toMap())) ++count;
    return count;
}

bool SchedulingPreviewModel::canAccept(const QVariantMap &row)
{
    return row.value(QStringLiteral("canAccept")).toBool();
}

void SchedulingPreviewModel::setRecommendations(QVariantList recommendations)
{
    beginResetModel();
    m_rows = std::move(recommendations);
    for (QVariant &value : m_rows) {
        QVariantMap row = value.toMap();
        row.insert(QStringLiteral("accepted"), false);
        value = row;
    }
    endResetModel();
    emit countChanged();
    emit acceptedCountChanged();
}

void SchedulingPreviewModel::clear()
{
    setRecommendations({});
}

void SchedulingPreviewModel::clearAcceptance()
{
    bool changed = false;
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        QVariantMap row = m_rows.at(rowIndex).toMap();
        if (!row.value(QStringLiteral("accepted")).toBool()) continue;
        row.insert(QStringLiteral("accepted"), false);
        m_rows[rowIndex] = row;
        changed = true;
    }
    if (changed) notifyAcceptanceChanged();
}

bool SchedulingPreviewModel::setAccepted(qint64 taskId, bool accepted)
{
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        QVariantMap row = m_rows.at(rowIndex).toMap();
        if (row.value(QStringLiteral("taskId")).toLongLong() != taskId) continue;
        if (accepted && !canAccept(row)) return false;
        if (row.value(QStringLiteral("accepted")).toBool() == accepted) return true;
        row.insert(QStringLiteral("accepted"), accepted);
        m_rows[rowIndex] = row;
        const QModelIndex changedIndex = index(rowIndex, 0);
        emit dataChanged(changedIndex, changedIndex, {AcceptedRole});
        emit acceptedCountChanged();
        return true;
    }
    return false;
}

void SchedulingPreviewModel::acceptAllValid()
{
    bool changed = false;
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        QVariantMap row = m_rows.at(rowIndex).toMap();
        const bool shouldAccept = canAccept(row);
        if (row.value(QStringLiteral("accepted")).toBool() == shouldAccept) continue;
        row.insert(QStringLiteral("accepted"), shouldAccept);
        m_rows[rowIndex] = row;
        changed = true;
    }
    if (changed) notifyAcceptanceChanged();
}

void SchedulingPreviewModel::notifyAcceptanceChanged()
{
    if (!m_rows.isEmpty()) emit dataChanged(index(0, 0), index(m_rows.size() - 1, 0), {AcceptedRole});
    emit acceptedCountChanged();
}

} // namespace something::models
