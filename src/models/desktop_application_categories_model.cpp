#include "desktop_application_categories_model.h"

#include <utility>

namespace something::models {

DesktopApplicationCategoriesModel::DesktopApplicationCategoriesModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DesktopApplicationCategoriesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant DesktopApplicationCategoriesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const QVariantMap row = m_rows.at(index.row()).toMap();
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return row.value(QStringLiteral("name"));
    case IdRole: return row.value(QStringLiteral("id"));
    case DescriptionRole: return row.value(QStringLiteral("description"));
    case ColorRole: return row.value(QStringLiteral("color"));
    case IconIdentifierRole: return row.value(QStringLiteral("iconIdentifier"));
    case DisplayOrderRole: return row.value(QStringLiteral("displayOrder"));
    case ApplicationCountRole: return row.value(QStringLiteral("applicationCount"));
    default: return {};
    }
}

QHash<int, QByteArray> DesktopApplicationCategoriesModel::roleNames() const
{
    return {{IdRole, "categoryId"}, {NameRole, "name"},
            {DescriptionRole, "description"}, {ColorRole, "color"},
            {IconIdentifierRole, "iconIdentifier"}, {DisplayOrderRole, "displayOrder"},
            {ApplicationCountRole, "applicationCount"}};
}

QVariantMap DesktopApplicationCategoriesModel::rowAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).toMap() : QVariantMap{};
}

void DesktopApplicationCategoriesModel::replace(QVariantList rows)
{
    if (rows == m_rows) return;
    const int previous = m_rows.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (previous != m_rows.size()) emit countChanged();
}

} // namespace something::models
