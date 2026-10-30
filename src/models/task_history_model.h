#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace something::models {

class TaskHistoryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        EventIdRole = Qt::UserRole + 1,
        TaskIdRole,
        EventTypeRole,
        OccurredAtUtcRole,
        TaskRevisionRole,
        TaskTitleRole,
        SerializationVersionRole,
        ChangesRole,
    };
    Q_ENUM(Role)

    explicit TaskHistoryModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_events.size(); }
    QVariantMap eventAt(int row) const;
    void replace(QVariantList events);
    void appendOlder(QVariantList events);

signals:
    void countChanged();

private:
    QVector<QVariantMap> m_events;
};

} // namespace something::models
