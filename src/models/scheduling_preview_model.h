#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>

namespace something::models {

class SchedulingPreviewModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int acceptedCount READ acceptedCount NOTIFY acceptedCountChanged)
    Q_PROPERTY(int validCount READ validCount NOTIFY acceptedCountChanged)

public:
    enum Role {
        TaskIdRole = Qt::UserRole + 1,
        TitleRole,
        DueDateRole,
        PriorityRole,
        DifficultyRole,
        EstimatedDurationMinutesRole,
        PlannedDateRole,
        PlannedStartTimeRole,
        ScoreRole,
        StateRole,
        OverdueRole,
        UsedDefaultDurationRole,
        PreviouslyScheduledRole,
        RetainedExistingScheduleRole,
        HasConflictRole,
        OverloadedRole,
        ReasonRole,
        WarningsRole,
        TaskRevisionRole,
        ScheduleRevisionRole,
        CanAcceptRole,
        AcceptedRole,
    };

    explicit SchedulingPreviewModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int count() const noexcept { return m_rows.size(); }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int acceptedCount() const noexcept;
    int validCount() const noexcept;

    void setRecommendations(QVariantList recommendations);
    void clear();
    void clearAcceptance();
    Q_INVOKABLE bool setAccepted(qint64 taskId, bool accepted);
    Q_INVOKABLE void acceptAllValid();

signals:
    void countChanged();
    void acceptedCountChanged();

private:
    static bool canAccept(const QVariantMap &row);
    void notifyAcceptanceChanged();

    QVariantList m_rows;
};

} // namespace something::models
