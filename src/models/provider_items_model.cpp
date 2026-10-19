#include "provider_items_model.h"

#include <QSet>

#include <utility>

namespace something::models {

ProviderItemsModel::ProviderItemsModel(QObject *parent) : QAbstractListModel(parent) {}
int ProviderItemsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_items.size(); }
QVariant ProviderItemsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const QVariantMap item = m_items.at(index.row()).toMap();
    switch (role) {
    case Qt::DisplayRole: case TitleRole: return item.value(QStringLiteral("title"));
    case ItemIdRole: return item.value(QStringLiteral("itemId"));
    case TextRole: return item.value(QStringLiteral("text"));
    case PreviewRole: return item.value(QStringLiteral("preview"));
    case PublishedAtRole: return item.value(QStringLiteral("publishedAt"));
    case ViewsRole: return item.value(QStringLiteral("views"));
    case ImagesRole: return item.value(QStringLiteral("images"));
    case ImageSourcesRole: return item.value(QStringLiteral("imageSources"));
    case ExternalUrlRole: return item.value(QStringLiteral("externalUrl"));
    case ChannelTitleRole: return item.value(QStringLiteral("channelTitle"));
    case SourceIdRole: return item.value(QStringLiteral("sourceId"));
    default: return {};
    }
}
QHash<int, QByteArray> ProviderItemsModel::roleNames() const
{
    return {{ItemIdRole, "itemId"}, {TitleRole, "title"}, {TextRole, "text"},
            {PreviewRole, "preview"}, {PublishedAtRole, "publishedAt"}, {ViewsRole, "views"},
            {ImagesRole, "images"}, {ImageSourcesRole, "imageSources"},
            {ExternalUrlRole, "externalUrl"}, {ChannelTitleRole, "channelTitle"},
            {SourceIdRole, "sourceId"}};
}
QVariantMap ProviderItemsModel::itemAt(int row) const
{
    return row >= 0 && row < m_items.size() ? m_items.at(row).toMap() : QVariantMap{};
}
void ProviderItemsModel::replace(QVariantList items)
{
    if (items == m_items) return;
    const int before = m_items.size();
    beginResetModel(); m_items = std::move(items); endResetModel();
    if (before != m_items.size()) emit countChanged();
}
void ProviderItemsModel::appendUnique(QVariantList items)
{
    QSet<QString> known;
    for (const QVariant &row : std::as_const(m_items)) known.insert(row.toMap().value(QStringLiteral("itemId")).toString());
    QVariantList additions;
    for (QVariant &row : items) {
        const QString id = row.toMap().value(QStringLiteral("itemId")).toString();
        if (!id.isEmpty() && !known.contains(id)) { known.insert(id); additions.push_back(std::move(row)); }
    }
    if (additions.isEmpty()) return;
    const int first = m_items.size();
    beginInsertRows({}, first, first + additions.size() - 1);
    for (QVariant &row : additions) m_items.push_back(std::move(row));
    endInsertRows();
    emit countChanged();
}
void ProviderItemsModel::prependUnique(QVariantList items)
{
    QSet<QString> known;
    for (const QVariant &row : std::as_const(m_items))
        known.insert(row.toMap().value(QStringLiteral("itemId")).toString());
    QVariantList additions;
    for (QVariant &row : items) {
        const QString id = row.toMap().value(QStringLiteral("itemId")).toString();
        if (!id.isEmpty() && !known.contains(id)) {
            known.insert(id);
            additions.push_back(std::move(row));
        }
    }
    if (additions.isEmpty()) return;
    const int last = additions.size() - 1;
    beginInsertRows({}, 0, last);
    for (int row = last; row >= 0; --row)
        m_items.prepend(std::move(additions[row]));
    endInsertRows();
    emit countChanged();
}
void ProviderItemsModel::updateImageSource(const QString &itemId, int imageIndex, const QString &source)
{
    for (int row = 0; row < m_items.size(); ++row) {
        QVariantMap item = m_items[row].toMap();
        if (item.value(QStringLiteral("itemId")).toString() != itemId) continue;
        QVariantList values = item.value(QStringLiteral("imageSources")).toList();
        if (imageIndex < 0 || imageIndex >= values.size()) return;
        values[imageIndex] = source;
        item.insert(QStringLiteral("imageSources"), values);
        m_items[row] = item;
        emit dataChanged(index(row), index(row), {ImageSourcesRole});
        return;
    }
}

FavoriteChannelsModel::FavoriteChannelsModel(QObject *parent) : QAbstractListModel(parent) {}
int FavoriteChannelsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_visibleRows.size(); }
QVariant FavoriteChannelsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visibleRows.size()) return {};
    const QVariantMap row = m_visibleRows.at(index.row()).toMap();
    switch (role) {
    case Qt::DisplayRole: case LabelRole: return row.value(QStringLiteral("label"));
    case SourceIdRole: return row.value(QStringLiteral("sourceId"));
    case CategoryIdRole: return row.value(QStringLiteral("categoryId"));
    case CategoryNameRole: return row.value(QStringLiteral("categoryName"));
    case CategoryColorRole: return row.value(QStringLiteral("categoryColor"));
    default: return {};
    }
}
QHash<int, QByteArray> FavoriteChannelsModel::roleNames() const
{
    return {{SourceIdRole, "sourceId"}, {LabelRole, "label"},
            {CategoryIdRole, "categoryId"}, {CategoryNameRole, "categoryName"},
            {CategoryColorRole, "categoryColor"}};
}
void FavoriteChannelsModel::setCategoryFilter(QString filter)
{
    if (filter == m_categoryFilter) return;
    m_categoryFilter = std::move(filter);
    rebuild(); emit categoryFilterChanged();
}
void FavoriteChannelsModel::replace(QVariantList rows)
{
    if (rows == m_allRows) return;
    m_allRows = std::move(rows);
    rebuild();
}
void FavoriteChannelsModel::rebuild()
{
    const int previous = m_visibleRows.size();
    QVariantList filtered;
    for (const QVariant &value : std::as_const(m_allRows)) {
        const QVariantMap row = value.toMap();
        const QVariant category = row.value(QStringLiteral("categoryId"));
        if (m_categoryFilter == QStringLiteral("all") ||
            (m_categoryFilter == QStringLiteral("uncategorized") && !category.isValid()) ||
            (category.isValid() && category.toString() == m_categoryFilter)) filtered.push_back(value);
    }
    if (filtered == m_visibleRows) return;
    beginResetModel(); m_visibleRows = std::move(filtered); endResetModel();
    if (previous != m_visibleRows.size()) emit countChanged();
}

FavoriteCategoriesModel::FavoriteCategoriesModel(QObject *parent) : QAbstractListModel(parent) {}
int FavoriteCategoriesModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }
QVariant FavoriteCategoriesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const QVariantMap row = m_rows.at(index.row()).toMap();
    switch (role) {
    case Qt::DisplayRole: case NameRole: return row.value(QStringLiteral("name"));
    case CategoryIdRole: return row.value(QStringLiteral("id"));
    case ColorRole: return row.value(QStringLiteral("color"));
    case DisplayOrderRole: return row.value(QStringLiteral("displayOrder"));
    default: return {};
    }
}
QHash<int, QByteArray> FavoriteCategoriesModel::roleNames() const
{
    return {{CategoryIdRole, "categoryId"}, {NameRole, "name"},
            {ColorRole, "color"}, {DisplayOrderRole, "displayOrder"}};
}
QVariantMap FavoriteCategoriesModel::rowAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).toMap() : QVariantMap{};
}
void FavoriteCategoriesModel::replace(QVariantList rows)
{
    if (rows == m_rows) return;
    const int previous = m_rows.size();
    beginResetModel(); m_rows = std::move(rows); endResetModel();
    if (previous != m_rows.size()) emit countChanged();
}

} // namespace something::models
