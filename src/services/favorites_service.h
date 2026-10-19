#pragma once

#include "../storage/storage_worker.h"
#include "../providers/provider_parsers.h"

#include <QByteArray>
#include <QVariantList>

namespace something::services {

class FavoritesService final {
public:
    static storage::StorageCommand loadSourceSnapshot(QString source);
    static storage::StorageCommand addFavorite(QString source, QString sourceId, QString label);
    static storage::StorageCommand assignFavoriteCategory(QString source, QString sourceId,
                                                          qint64 categoryId = 0);
    static storage::StorageCommand removeFavorite(QString source, QString sourceId);
    static storage::StorageCommand createCategory(QString source, QString name, QString color);
    static storage::StorageCommand updateCategory(qint64 id, QString source,
                                                 QString name, QString color);
    static storage::StorageCommand reorderCategories(QString source, QVector<qint64> ids);
    static storage::StorageCommand deleteCategory(qint64 id, QString mode,
                                                  qint64 targetCategoryId = 0);
    static storage::StorageCommand prepareNewsScan(bool appOpen,
                                                   QString scanStartedAt,
                                                   QString retentionCutoff);
    static storage::StorageCommand commitNewsScan(storage::FavoriteNewsScanCommit scan);
    static storage::StorageCommand recentNews(int limit = 60);

    static QVariantMap telegramPostToMap(const QString &channel,
                                        const something::providers::TelegramPost &post);
    static QVariantMap youTubeVideoToMap(const something::providers::YouTubeVideo &video);
    static QByteArray encodeCachedItems(const QVariantList &items);
    static QVariantList decodeCachedItems(const QByteArray &payload);
    static storage::FavoriteNewsItem newsItemFromMap(const QString &source,
                                                    const QString &sourceId,
                                                    const QVariantMap &item,
                                                    const QString &discoveredAt);

private:
    static QVariantMap categoryMap(const storage::FavoriteCategory &category);
    static QVariantMap favoriteMap(const storage::Favorite &favorite);
    static QVariantMap newsItemMap(const storage::FavoriteNewsItem &item);
};

} // namespace something::services
