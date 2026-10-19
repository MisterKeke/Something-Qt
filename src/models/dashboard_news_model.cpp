#include "dashboard_news_model.h"

#include <QVariantMap>

#include <utility>

namespace something::models {

DashboardNewsModel::DashboardNewsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DashboardNewsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant DashboardNewsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const Item &item = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return item.title;
    case SourceRole: return item.source;
    case PreviewRole: return item.preview;
    case PublishedAtRole: return item.publishedAt;
    case SourceIdRole: return item.sourceId;
    case ItemIdRole: return item.itemId;
    case ImagesRole: return item.images;
    case ImageSourcesRole: return item.imageSources;
    case ExternalUrlRole: return item.externalUrl;
    case ViewsRole: return item.views;
    default: return {};
    }
}

QHash<int, QByteArray> DashboardNewsModel::roleNames() const
{
    return {{SourceRole, "source"}, {TitleRole, "title"},
            {PreviewRole, "preview"}, {PublishedAtRole, "publishedAt"},
            {SourceIdRole, "sourceId"}, {ItemIdRole, "itemId"},
            {ImagesRole, "images"}, {ImageSourcesRole, "imageSources"},
            {ExternalUrlRole, "externalUrl"}, {ViewsRole, "views"}};
}

void DashboardNewsModel::replace(QVariantList items)
{
    if (items.size() == m_items.size()) {
        bool unchanged = true;
        for (int row = 0; row < items.size(); ++row) {
            if (itemAt(row) != items.at(row).toMap()) {
                unchanged = false;
                break;
            }
        }
        if (unchanged) return;
    }
    const int previousCount = m_items.size();
    beginResetModel();
    m_items.clear();
    m_items.reserve(items.size());
    for (const QVariant &value : items) {
        const QVariantMap row = value.toMap();
        m_items.push_back({row.value(QStringLiteral("source")).toString(),
                           row.value(QStringLiteral("title")).toString(),
                           row.value(QStringLiteral("preview")).toString(),
                           row.value(QStringLiteral("publishedAt")).toString(),
                           row.value(QStringLiteral("sourceId")).toString(),
                           row.value(QStringLiteral("itemId")).toString(),
                           row.value(QStringLiteral("images")).toList(),
                           row.value(QStringLiteral("imageSources")).toList(),
                           row.value(QStringLiteral("externalUrl")).toString(),
                           row.value(QStringLiteral("views")).toString()});
    }
    endResetModel();
    if (previousCount != m_items.size()) emit countChanged();
}

QVariantMap DashboardNewsModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return {};
    const Item &item = m_items.at(row);
    return {{QStringLiteral("source"), item.source},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("preview"), item.preview},
            {QStringLiteral("publishedAt"), item.publishedAt},
            {QStringLiteral("sourceId"), item.sourceId},
            {QStringLiteral("itemId"), item.itemId},
            {QStringLiteral("images"), item.images},
            {QStringLiteral("imageSources"), item.imageSources},
            {QStringLiteral("externalUrl"), item.externalUrl},
            {QStringLiteral("views"), item.views}};
}

void DashboardNewsModel::updateImageSource(const QString &itemId, int imageIndex,
                                            const QString &source)
{
    for (int row = 0; row < m_items.size(); ++row) {
        Item &item = m_items[row];
        if (item.itemId != itemId) continue;
        if (imageIndex < 0 || imageIndex >= item.imageSources.size()) return;
        item.imageSources[imageIndex] = source;
        emit dataChanged(index(row), index(row), {ImageSourcesRole});
        return;
    }
}

} // namespace something::models
