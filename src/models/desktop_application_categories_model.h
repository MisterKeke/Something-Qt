#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>

namespace something::models {

class DesktopApplicationCategoriesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { IdRole = Qt::UserRole + 1, NameRole, DescriptionRole, ColorRole,
                IconIdentifierRole, DisplayOrderRole, ApplicationCountRole };
    Q_ENUM(Role)

    explicit DesktopApplicationCategoriesModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_rows.size(); }
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    void replace(QVariantList rows);

signals:
    void countChanged();

private:
    QVariantList m_rows;
};

} // namespace something::models
