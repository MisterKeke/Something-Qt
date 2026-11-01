#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QVariantMap>

namespace something::models {

class UniversalSearchModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { ResultIdRole = Qt::UserRole + 1, TypeRole, GroupRole, TitleRole,
                SubtitleRole, SourceRole, RouteIdRole, TargetIdRole, SecondaryIdRole,
                IconRole, ActionsRole, ScoreRole, PayloadRole };
    Q_ENUM(Role)

    explicit UniversalSearchModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_results.size(); }
    QVariantMap rowAt(int row) const;
    void replace(QVector<QVariantMap> results);
    void clear();

signals:
    void countChanged();

private:
    QVector<QVariantMap> m_results;
};

} // namespace something::models
