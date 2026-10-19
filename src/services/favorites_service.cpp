#include "favorites_service.h"

#include "../domain/favorite_rules.h"
#include "../providers/provider_policy.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QUrlQuery>

#include <utility>

namespace something::services {
namespace {

using namespace something;

storage::StorageReply replyError(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

QString stringValue(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString().trimmed();
}

QJsonObject payloadObject(const QString &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject{};
}

QString canonicalNewsUrl(const storage::FavoriteNewsItem &item, const QJsonObject &payload)
{
    if (item.source == QStringLiteral("telegram"))
        return domain::telegramPostUrl(item.sourceId, stringValue(payload, QStringLiteral("postId"))).toString();
    return domain::youTubeVideoUrl(stringValue(payload, QStringLiteral("videoId"))).toString();
}

QVariantList emptyImageSources(qsizetype count)
{
    QVariantList values;
    values.reserve(count);
    for (qsizetype i = 0; i < count; ++i) values.push_back(QString());
    return values;
}

QStringList stringList(const QVariant &value)
{
    QStringList output;
    const QVariantList list = value.toList();
    output.reserve(list.size());
    for (const QVariant &entry : list) {
        const QString text = entry.toString();
        if (!text.isEmpty()) output.push_back(text);
    }
    if (output.isEmpty()) output = value.toStringList();
    return output;
}

QString firstValue(const QJsonObject &object, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString value = stringValue(object, key);
        if (!value.isEmpty()) return value;
    }
    return {};
}

} // namespace

QVariantMap FavoritesService::categoryMap(const storage::FavoriteCategory &category)
{
    return {{QStringLiteral("id"), category.id}, {QStringLiteral("name"), category.name},
            {QStringLiteral("source"), category.source}, {QStringLiteral("color"), category.color},
            {QStringLiteral("displayOrder"), category.displayOrder},
            {QStringLiteral("createdAt"), category.createdAt},
            {QStringLiteral("updatedAt"), category.updatedAt}};
}

QVariantMap FavoritesService::favoriteMap(const storage::Favorite &favorite)
{
    QVariantMap map{{QStringLiteral("source"), favorite.source},
                    {QStringLiteral("sourceId"), favorite.sourceId},
                    {QStringLiteral("label"), favorite.username.isEmpty()
                         ? favorite.sourceId : favorite.username},
                    {QStringLiteral("addedAt"), favorite.addedAt}};
    if (favorite.categoryId) map.insert(QStringLiteral("categoryId"), *favorite.categoryId);
    return map;
}

QVariantMap FavoritesService::newsItemMap(const storage::FavoriteNewsItem &item)
{
    const QJsonObject payload = payloadObject(item.payloadJson);
    const QVariantList imageValues = payload.value(QStringLiteral("images")).toArray().toVariantList();
    QStringList images;
    images.reserve(imageValues.size());
    for (const QVariant &value : imageValues) images.push_back(value.toString());
    const QString title = item.source == QStringLiteral("telegram")
        ? QStringLiteral("@%1").arg(item.sourceId)
        : firstValue(payload, {QStringLiteral("title"), QStringLiteral("channelTitle")});
    const QString preview = firstValue(payload, {QStringLiteral("preview"), QStringLiteral("description")});
    QStringList validImages;
    QSet<QString> seenImages;
    const providers::ProviderId mediaPolicy = item.source == QStringLiteral("telegram")
        ? providers::ProviderId::TelegramMedia : providers::ProviderId::YouTubeMedia;
    for (const QString &image : images) {
        const QUrl url(image, QUrl::StrictMode);
        if (providers::isAllowedProviderUrl(mediaPolicy, url)) {
            const QString normalized = url.toString(QUrl::FullyEncoded);
            if (!seenImages.contains(normalized)) {
                seenImages.insert(normalized);
                validImages.push_back(normalized);
            }
        }
    }
    QVariantList imageSources = emptyImageSources(validImages.size());
    const QString thumb = stringValue(payload, QStringLiteral("thumbnail"));
    if (validImages.isEmpty() && !thumb.isEmpty()) {
        const QUrl thumbUrl(thumb, QUrl::StrictMode);
        if (providers::isAllowedProviderUrl(providers::ProviderId::YouTubeMedia, thumbUrl))
            validImages.push_back(thumbUrl.toString(QUrl::FullyEncoded));
        imageSources = emptyImageSources(validImages.size());
    }
    return {{QStringLiteral("source"), item.source},
            {QStringLiteral("sourceId"), item.sourceId},
            {QStringLiteral("itemId"), item.itemId},
            {QStringLiteral("title"), title},
            {QStringLiteral("preview"), preview},
            {QStringLiteral("publishedAt"), item.publishedAt},
            {QStringLiteral("views"), stringValue(payload, QStringLiteral("views"))},
            {QStringLiteral("images"), validImages},
            {QStringLiteral("imageSources"), imageSources},
            {QStringLiteral("externalUrl"), canonicalNewsUrl(item, payload)}};
}

storage::StorageCommand FavoritesService::loadSourceSnapshot(QString source)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    return [source = std::move(source)](storage::StorageRepositories &repositories,
                                         const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        if (source.isEmpty()) return replyError(domain::AppError(domain::ErrorCategory::Validation));
        const auto categoryResult = repositories.favorites.categories(source);
        if (!categoryResult.succeeded()) return replyError(categoryResult.error);
        const auto favoriteResult = repositories.favorites.favorites(source);
        if (!favoriteResult.succeeded()) return replyError(favoriteResult.error);

        QVariantList categories;
        QHash<qint64, QString> categoryNames;
        QHash<qint64, QString> categoryColors;
        categories.reserve(categoryResult.value.size());
        for (const auto &category : categoryResult.value) {
            categories.push_back(categoryMap(category));
            categoryNames.insert(category.id, category.name);
            categoryColors.insert(category.id, category.color);
        }
        QVariantList favorites;
        favorites.reserve(favoriteResult.value.size());
        for (const auto &favorite : favoriteResult.value) {
            if (cancelled()) return replyError(domain::AppError::cancelled());
            QVariantMap map = favoriteMap(favorite);
            if (favorite.categoryId)
                map.insert(QStringLiteral("categoryName"), categoryNames.value(*favorite.categoryId));
            if (favorite.categoryId)
                map.insert(QStringLiteral("categoryColor"), categoryColors.value(*favorite.categoryId));
            favorites.push_back(std::move(map));
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("source"), source);
        reply.values.insert(QStringLiteral("categories"), categories);
        reply.values.insert(QStringLiteral("favorites"), favorites);
        return reply;
    };
}

storage::StorageCommand FavoritesService::assignFavoriteCategory(QString source, QString sourceId,
                                                                 qint64 categoryId)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source == QStringLiteral("telegram")) sourceId = domain::normalizeTelegramChannel(std::move(sourceId));
    else if (source == QStringLiteral("youtube")) {
        bool id = false;
        sourceId = domain::normalizeYouTubeReference(std::move(sourceId), &id);
        if (!id) sourceId.clear();
    }
    const std::optional<qint64> category = categoryId > 0 ? std::optional<qint64>(categoryId) : std::nullopt;
    return [source = std::move(source), sourceId = std::move(sourceId), category]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.favorites.assignFavoriteCategory(source, sourceId, category);
        return result.succeeded() ? storage::StorageReply{} : replyError(result.error);
    };
}

storage::StorageCommand FavoritesService::addFavorite(QString source, QString sourceId, QString label)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source == QStringLiteral("telegram")) {
        sourceId = domain::normalizeTelegramChannel(std::move(sourceId));
    } else if (source == QStringLiteral("youtube")) {
        bool isId = false;
        sourceId = domain::normalizeYouTubeReference(std::move(sourceId), &isId);
        if (!isId) sourceId.clear();
    }
    label = label.trimmed();
    return [source = std::move(source), sourceId = std::move(sourceId), label = std::move(label)]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.favorites.addFavorite(source, sourceId, label);
        return result.succeeded() ? storage::StorageReply{} : replyError(result.error);
    };
}

storage::StorageCommand FavoritesService::removeFavorite(QString source, QString sourceId)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source == QStringLiteral("telegram")) sourceId = domain::normalizeTelegramChannel(std::move(sourceId));
    else if (source == QStringLiteral("youtube")) {
        bool id = false;
        sourceId = domain::normalizeYouTubeReference(std::move(sourceId), &id);
        if (!id) sourceId.clear();
    }
    return [source = std::move(source), sourceId = std::move(sourceId)]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.favorites.removeFavorite(source, sourceId);
        return result.succeeded() ? storage::StorageReply{} : replyError(result.error);
    };
}

storage::StorageCommand FavoritesService::createCategory(QString source, QString name, QString color)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    return [source = std::move(source), name = std::move(name), color = std::move(color)]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.favorites.createCategory(source, name, color);
        if (!result.succeeded()) return replyError(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("category"), categoryMap(result.value));
        return reply;
    };
}

storage::StorageCommand FavoritesService::updateCategory(qint64 id, QString source,
                                                        QString name, QString color)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    return [id, source = std::move(source), name = std::move(name), color = std::move(color)]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.favorites.updateCategory(id, source, name, color);
        if (!result.succeeded()) return replyError(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("category"), categoryMap(result.value));
        return reply;
    };
}

storage::StorageCommand FavoritesService::reorderCategories(QString source, QVector<qint64> ids)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    return [source = std::move(source), ids = std::move(ids)]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.favorites.reorderCategories(source, ids);
        return result.succeeded() ? storage::StorageReply{} : replyError(result.error);
    };
}

storage::StorageCommand FavoritesService::deleteCategory(qint64 id, QString mode, qint64 targetCategoryId)
{
    mode = mode.trimmed().toLower();
    if (mode.isEmpty()) mode = QStringLiteral("unassign");
    const std::optional<qint64> target = targetCategoryId > 0
        ? std::optional<qint64>(targetCategoryId) : std::nullopt;
    return [id, mode = std::move(mode), target]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.favorites.removeCategory(id, mode, target);
        if (!result.succeeded()) return replyError(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("affectedFavorites"), result.value.affectedFavorites);
        reply.values.insert(QStringLiteral("source"), result.value.source);
        return reply;
    };
}

storage::StorageCommand FavoritesService::prepareNewsScan(bool appOpen,
                                                          QString scanStartedAt,
                                                          QString retentionCutoff)
{
    return [appOpen, scanStartedAt = std::move(scanStartedAt),
            retentionCutoff = std::move(retentionCutoff)]
           (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        if (scanStartedAt.trimmed().isEmpty())
            return replyError(domain::AppError(domain::ErrorCategory::Validation));
        if (appOpen) {
            const auto recorded = repositories.news.recordAppOpen(scanStartedAt, retentionCutoff);
            if (!recorded.succeeded()) return replyError(recorded.error);
        }
        auto preference = [&repositories](const QString &key) {
            const auto result = repositories.preferences.get(key);
            return result.succeeded() ? result.value : QString();
        };
        const QString fallback = appOpen
            ? (preference(QStringLiteral("favorite_updates.previous_opened_at"))).isEmpty()
                ? scanStartedAt : preference(QStringLiteral("favorite_updates.previous_opened_at"))
            : (preference(QStringLiteral("favorite_updates.last_refresh_at")).isEmpty()
                ? scanStartedAt : preference(QStringLiteral("favorite_updates.last_refresh_at")));
        QVariantList sources;
        for (const QString &source : {QStringLiteral("telegram"), QStringLiteral("youtube")}) {
            if (cancelled()) return replyError(domain::AppError::cancelled());
            const auto favorites = repositories.favorites.favorites(source);
            if (!favorites.succeeded()) return replyError(favorites.error);
            for (const auto &favorite : favorites.value) {
                const auto checkpoint = repositories.news.checkpoint(source, favorite.sourceId);
                if (!checkpoint.succeeded()) return replyError(checkpoint.error);
                const auto hasSeen = repositories.news.sourceHasSeenItems(source, favorite.sourceId);
                if (!hasSeen.succeeded()) return replyError(hasSeen.error);
                const QString checkedThrough = checkpoint.value &&
                        !checkpoint.value->checkedThrough.isEmpty()
                    ? checkpoint.value->checkedThrough : fallback;
                sources.push_back(QVariantMap{
                    {QStringLiteral("source"), source},
                    {QStringLiteral("sourceId"), favorite.sourceId},
                    {QStringLiteral("label"), favorite.username},
                    {QStringLiteral("checkedThrough"), checkedThrough},
                    {QStringLiteral("updateWindowStart"), fallback},
                    {QStringLiteral("sourceHasSeenItems"), hasSeen.value},
                });
            }
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("scanStartedAt"), scanStartedAt);
        reply.values.insert(QStringLiteral("fallbackCheckedThrough"), fallback);
        reply.values.insert(QStringLiteral("sources"), sources);
        return reply;
    };
}

storage::StorageCommand FavoritesService::commitNewsScan(storage::FavoriteNewsScanCommit scan)
{
    return [scan = std::move(scan)](storage::StorageRepositories &repositories,
                                    const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        const auto result = repositories.news.commitScan(scan, cancelled);
        if (!result.succeeded()) return replyError(result.error);
        QVariantList newItems;
        for (const auto &item : result.value) newItems.push_back(newsItemMap(item));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("newItems"), newItems);
        reply.values.insert(QStringLiteral("scanStartedAt"), scan.scanStartedAt);
        return reply;
    };
}

storage::StorageCommand FavoritesService::recentNews(int limit)
{
    return [limit](storage::StorageRepositories &repositories,
                   const std::function<bool()> &cancelled) {
        if (cancelled()) return replyError(domain::AppError::cancelled());
        if (limit < 1 || limit > 500)
            return replyError(domain::AppError(domain::ErrorCategory::Validation));
        const auto state = repositories.news.scanState();
        if (!state.succeeded()) return replyError(state.error);
        if (!state.value || state.value->scanStartedAt.trimmed().isEmpty()) {
            storage::StorageReply reply;
            reply.values.insert(QStringLiteral("items"), QVariantList{});
            return reply;
        }
        const auto result = repositories.news.newsForScan(state.value->scanStartedAt, limit);
        if (!result.succeeded()) return replyError(result.error);
        QVariantList items;
        items.reserve(result.value.size());
        for (const auto &item : result.value) {
            if (cancelled()) return replyError(domain::AppError::cancelled());
            items.push_back(newsItemMap(item));
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("items"), items);
        return reply;
    };
}

QVariantMap FavoritesService::telegramPostToMap(const QString &channel,
                                               const providers::TelegramPost &post)
{
    const QString itemId = post.postId.trimmed();
    const QString channelId = domain::normalizeTelegramChannel(channel);
    QStringList images;
    QSet<QString> seenImages;
    for (const QString &image : post.images) {
        if (!seenImages.contains(image)) {
            seenImages.insert(image);
            images.push_back(image);
        }
    }
    QVariantList urls;
    for (const QString &image : images) urls.push_back(image);
    return {{QStringLiteral("source"), QStringLiteral("telegram")},
            {QStringLiteral("sourceId"), channelId},
            {QStringLiteral("itemId"), itemId},
            {QStringLiteral("title"), QStringLiteral("@%1").arg(channelId)},
            {QStringLiteral("text"), post.text},
            {QStringLiteral("preview"), post.text},
            {QStringLiteral("publishedAt"), post.date},
            {QStringLiteral("views"), post.views},
            {QStringLiteral("images"), urls},
            {QStringLiteral("imageSources"), emptyImageSources(urls.size())},
            {QStringLiteral("externalUrl"), domain::telegramPostUrl(channelId, itemId).toString()}};
}

QVariantMap FavoritesService::youTubeVideoToMap(const providers::YouTubeVideo &video)
{
    QVariantList images;
    if (!video.thumbnail.isEmpty()) images.push_back(video.thumbnail);
    return {{QStringLiteral("source"), QStringLiteral("youtube")},
            {QStringLiteral("sourceId"), video.channelId},
            {QStringLiteral("itemId"), video.videoId},
            {QStringLiteral("title"), video.title},
            {QStringLiteral("text"), video.description},
            {QStringLiteral("preview"), video.description},
            {QStringLiteral("publishedAt"), video.publishedAt},
            {QStringLiteral("views"), video.views},
            {QStringLiteral("channelTitle"), video.channelTitle},
            {QStringLiteral("images"), images},
            {QStringLiteral("imageSources"), emptyImageSources(images.size())},
            {QStringLiteral("externalUrl"), domain::youTubeVideoUrl(video.videoId).toString()}};
}

QByteArray FavoritesService::encodeCachedItems(const QVariantList &items)
{
    return QJsonDocument::fromVariant(items).toJson(QJsonDocument::Compact);
}

QVariantList FavoritesService::decodeCachedItems(const QByteArray &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    return error.error == QJsonParseError::NoError && document.isArray()
        ? document.array().toVariantList() : QVariantList{};
}

storage::FavoriteNewsItem FavoritesService::newsItemFromMap(const QString &source,
                                                           const QString &sourceId,
                                                           const QVariantMap &item,
                                                           const QString &discoveredAt)
{
    QJsonObject payload;
    if (source == QStringLiteral("telegram")) {
        payload.insert(QStringLiteral("username"), sourceId);
        payload.insert(QStringLiteral("postId"), item.value(QStringLiteral("itemId")).toString());
        payload.insert(QStringLiteral("preview"), item.value(QStringLiteral("preview")).toString());
        payload.insert(QStringLiteral("images"), QJsonArray::fromStringList(stringList(item.value(QStringLiteral("images")))));
        payload.insert(QStringLiteral("views"), item.value(QStringLiteral("views")).toString());
    } else {
        payload.insert(QStringLiteral("channelId"), sourceId);
        payload.insert(QStringLiteral("videoId"), item.value(QStringLiteral("itemId")).toString());
        payload.insert(QStringLiteral("channelTitle"), item.value(QStringLiteral("channelTitle")).toString());
        payload.insert(QStringLiteral("title"), item.value(QStringLiteral("title")).toString());
        payload.insert(QStringLiteral("thumbnail"), stringList(item.value(QStringLiteral("images"))).value(0));
        payload.insert(QStringLiteral("videoUrl"), item.value(QStringLiteral("externalUrl")).toString());
        payload.insert(QStringLiteral("description"), item.value(QStringLiteral("preview")).toString());
        payload.insert(QStringLiteral("views"), item.value(QStringLiteral("views")).toString());
    }
    return {source, sourceId, item.value(QStringLiteral("itemId")).toString(),
            item.value(QStringLiteral("publishedAt")).toString(), discoveredAt,
            QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))};
}

} // namespace something::services
