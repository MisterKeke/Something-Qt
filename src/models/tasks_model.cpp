#include "tasks_model.h"

#include <QVariantList>
#include <QVariantMap>

#include <utility>

namespace {

bool sameTask(const something::domain::Task &left, const something::domain::Task &right)
{
    if (left.id != right.id || left.title != right.title
        || left.description != right.description || left.completed != right.completed
        || left.createdAt != right.createdAt || left.updatedAt != right.updatedAt
        || left.dueDate != right.dueDate || left.priority != right.priority
        || left.difficulty != right.difficulty || left.revision != right.revision
        || left.tags != right.tags || left.subtasks.size() != right.subtasks.size()) {
        return false;
    }
    for (int i = 0; i < left.subtasks.size(); ++i) {
        const auto &a = left.subtasks.at(i);
        const auto &b = right.subtasks.at(i);
        if (a.id != b.id || a.title != b.title || a.completed != b.completed
            || a.position != b.position) return false;
    }
    return true;
}

void notifyTaskChanged(something::models::TasksModel *model, int row)
{
    model->dataChanged(model->index(row, 0), model->index(row, 0), {
        something::models::TasksModel::TaskIdRole,
        something::models::TasksModel::TitleRole,
        something::models::TasksModel::DescriptionRole,
        something::models::TasksModel::CompletedRole,
        something::models::TasksModel::CreatedAtRole,
        something::models::TasksModel::UpdatedAtRole,
        something::models::TasksModel::DueDateRole,
        something::models::TasksModel::PriorityRole,
        something::models::TasksModel::DifficultyRole,
        something::models::TasksModel::RevisionRole,
        something::models::TasksModel::TagsRole,
        something::models::TasksModel::SubtasksRole,
        something::models::TasksModel::SubtaskCountRole,
        something::models::TasksModel::CompletedSubtasksRole,
        something::models::TasksModel::OverdueRole,
    });
}

} // namespace

namespace something::models {

TasksModel::TasksModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_today()
{
}

int TasksModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_tasks.size();
}

QVariant TasksModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tasks.size()) return {};
    const domain::Task &task = m_tasks.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return task.title;
    case TaskIdRole: return task.id;
    case DescriptionRole: return task.description;
    case CompletedRole: return task.completed;
    case CreatedAtRole: return task.createdAt;
    case UpdatedAtRole: return task.updatedAt;
    case DueDateRole: return task.dueDate;
    case PriorityRole: return task.priority;
    case DifficultyRole: return task.difficulty;
    case RevisionRole: return task.revision;
    case TagsRole: return task.tags;
    case SubtasksRole: {
        QVariantList result;
        result.reserve(task.subtasks.size());
        for (const domain::Subtask &subtask : task.subtasks) {
            result.push_back(QVariantMap{
                {QStringLiteral("id"), subtask.id},
                {QStringLiteral("title"), subtask.title},
                {QStringLiteral("completed"), subtask.completed},
                {QStringLiteral("position"), subtask.position},
            });
        }
        return result;
    }
    case SubtaskCountRole: return task.subtasks.size();
    case CompletedSubtasksRole: {
        int completed = 0;
        for (const domain::Subtask &subtask : task.subtasks)
            if (subtask.completed) ++completed;
        return completed;
    }
    case OverdueRole:
        return !task.completed && !task.dueDate.isEmpty()
            && QDate::fromString(task.dueDate, Qt::ISODate).isValid()
            && QDate::fromString(task.dueDate, Qt::ISODate) < m_today;
    default: return {};
    }
}

QHash<int, QByteArray> TasksModel::roleNames() const
{
    return {
        {TaskIdRole, "taskId"}, {TitleRole, "title"},
        {DescriptionRole, "description"}, {CompletedRole, "completed"},
        {CreatedAtRole, "createdAt"}, {UpdatedAtRole, "updatedAt"},
        {DueDateRole, "dueDate"}, {PriorityRole, "priority"},
        {DifficultyRole, "difficulty"}, {RevisionRole, "revision"},
        {TagsRole, "tags"}, {SubtasksRole, "subtasks"},
        {SubtaskCountRole, "subtaskCount"},
        {CompletedSubtasksRole, "completedSubtasks"}, {OverdueRole, "overdue"},
    };
}

const domain::Task *TasksModel::taskAt(int row) const noexcept
{
    return row >= 0 && row < m_tasks.size() ? &m_tasks.at(row) : nullptr;
}

const domain::Task *TasksModel::taskById(qint64 id) const noexcept
{
    const int row = rowForId(id);
    return row >= 0 ? &m_tasks.at(row) : nullptr;
}

int TasksModel::rowForId(qint64 id) const noexcept
{
    for (int row = 0; row < m_tasks.size(); ++row)
        if (m_tasks.at(row).id == id) return row;
    return -1;
}

void TasksModel::replacePage(QVector<domain::Task> tasks)
{
    if (tasks.size() == m_tasks.size()) {
        bool unchanged = true;
        for (int row = 0; row < tasks.size(); ++row) {
            if (!sameTask(m_tasks.at(row), tasks.at(row))) {
                unchanged = false;
                break;
            }
        }
        if (unchanged) return;
    }
    const int previousCount = m_tasks.size();
    beginResetModel();
    m_tasks = std::move(tasks);
    endResetModel();
    if (previousCount != m_tasks.size()) emit countChanged();
}

bool TasksModel::applyTask(domain::Task task)
{
    const int row = rowForId(task.id);
    if (row < 0) return false;
    m_tasks[row] = std::move(task);
    notifyTaskChanged(this, row);
    return true;
}

void TasksModel::upsertByCreatedOrder(domain::Task task)
{
    const int oldRow = rowForId(task.id);
    if (oldRow >= 0 && oldRow < m_tasks.size()
        && sameTask(m_tasks.at(oldRow), task)) return;

    QVector<domain::Task> withoutCurrent = m_tasks;
    if (oldRow >= 0) withoutCurrent.removeAt(oldRow);
    int targetRow = 0;
    while (targetRow < withoutCurrent.size()) {
        const domain::Task &candidate = withoutCurrent.at(targetRow);
        const bool candidatePrecedes = candidate.createdAt > task.createdAt
            || (candidate.createdAt == task.createdAt && candidate.id > task.id);
        if (!candidatePrecedes) break;
        ++targetRow;
    }

    if (oldRow < 0) {
        beginInsertRows({}, targetRow, targetRow);
        m_tasks.insert(targetRow, std::move(task));
        endInsertRows();
        emit countChanged();
        return;
    }

    if (targetRow == oldRow) {
        m_tasks[oldRow] = std::move(task);
        notifyTaskChanged(this, oldRow);
        return;
    }
    const int destinationChild = targetRow > oldRow ? targetRow + 1 : targetRow;
    if (!beginMoveRows({}, oldRow, oldRow, {}, destinationChild)) return;
    m_tasks.removeAt(oldRow);
    m_tasks.insert(targetRow, std::move(task));
    endMoveRows();
    notifyTaskChanged(this, targetRow);
}

void TasksModel::reconcilePage(QVector<domain::Task> tasks)
{
    const int oldCount = m_tasks.size();
    for (int targetRow = 0; targetRow < tasks.size(); ++targetRow) {
        const qint64 wantedId = tasks.at(targetRow).id;
        if (targetRow < m_tasks.size() && m_tasks.at(targetRow).id == wantedId) {
            if (!sameTask(m_tasks.at(targetRow), tasks.at(targetRow))) {
                m_tasks[targetRow] = std::move(tasks[targetRow]);
                notifyTaskChanged(this, targetRow);
            }
            continue;
        }

        int existingRow = -1;
        for (int row = targetRow + 1; row < m_tasks.size(); ++row) {
            if (m_tasks.at(row).id == wantedId) {
                existingRow = row;
                break;
            }
        }
        if (existingRow >= 0) {
            beginMoveRows({}, existingRow, existingRow, {}, targetRow);
            m_tasks.move(existingRow, targetRow);
            endMoveRows();
            if (!sameTask(m_tasks.at(targetRow), tasks.at(targetRow))) {
                m_tasks[targetRow] = std::move(tasks[targetRow]);
                notifyTaskChanged(this, targetRow);
            }
        } else {
            beginInsertRows({}, targetRow, targetRow);
            m_tasks.insert(targetRow, std::move(tasks[targetRow]));
            endInsertRows();
        }
    }
    if (m_tasks.size() > tasks.size()) {
        const int first = tasks.size();
        const int last = m_tasks.size() - 1;
        beginRemoveRows({}, first, last);
        m_tasks.erase(m_tasks.begin() + first, m_tasks.end());
        endRemoveRows();
    }
    if (oldCount != m_tasks.size()) emit countChanged();
}

bool TasksModel::removeTask(qint64 id)
{
    const int row = rowForId(id);
    if (row < 0) return false;
    beginRemoveRows({}, row, row);
    m_tasks.removeAt(row);
    endRemoveRows();
    emit countChanged();
    return true;
}

void TasksModel::setToday(QDate today)
{
    if (!today.isValid() || today == m_today) return;
    m_today = std::move(today);
    if (!m_tasks.isEmpty()) {
        emit dataChanged(index(0, 0), index(m_tasks.size() - 1, 0), {OverdueRole});
    }
}

} // namespace something::models
