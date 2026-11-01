#pragma once

#include "../domain/task.h"

#include <QAbstractListModel>
#include <QDate>

namespace something::models {

class TasksModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        TaskIdRole = Qt::UserRole + 1,
        TitleRole,
        DescriptionRole,
        CompletedRole,
        CreatedAtRole,
        UpdatedAtRole,
        DueDateRole,
        PriorityRole,
        DifficultyRole,
        RevisionRole,
        TagsRole,
        SubtasksRole,
        SubtaskCountRole,
        CompletedSubtasksRole,
        OverdueRole,
        AutomationRole,
    };
    Q_ENUM(Role)

    explicit TasksModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_tasks.size(); }

    const domain::Task *taskAt(int row) const noexcept;
    const domain::Task *taskById(qint64 id) const noexcept;
    int rowForId(qint64 id) const noexcept;
    QVector<domain::Task> snapshot() const { return m_tasks; }
    void replacePage(QVector<domain::Task> tasks);
    void reconcilePage(QVector<domain::Task> tasks);
    bool applyTask(domain::Task task);
    void upsertByCreatedOrder(domain::Task task);
    bool removeTask(qint64 id);
    void setToday(QDate today);

signals:
    void countChanged();

private:
    QVector<domain::Task> m_tasks;
    QDate m_today;
};

} // namespace something::models
