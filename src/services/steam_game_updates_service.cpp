#include "steam_game_updates_service.h"

#include <QVariantList>

#include <utility>

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

void removeArtworkIfUnowned(const QString &profileDirectory, const QString &filename,
                            storage::SteamRepository &steam,
                            storage::SteamReleaseRepository &steamReleases,
                            storage::SteamGameUpdatesRepository &gameUpdates,
                            storage::SteamGameCatalogRepository &catalog)
{
    if (filename.isEmpty()) return;
    const auto priceOwner = steam.hasArtworkFilename(filename);
    if (!priceOwner.succeeded() || priceOwner.value) return;
    const auto releaseOwner = steamReleases.hasArtworkFilename(filename);
    if (!releaseOwner.succeeded() || releaseOwner.value) return;
    const auto updatesOwner = gameUpdates.hasArtworkFilename(filename);
    if (!updatesOwner.succeeded() || updatesOwner.value) return;
    const auto catalogOwner = catalog.hasArtworkFilename(filename);
    if (!catalogOwner.succeeded() || catalogOwner.value) return;

    QString originalPath;
    QString stagedPath;
    if (SteamArtworkStore::stageDeletion(profileDirectory, filename, &stagedPath,
                                         &originalPath)
        && !stagedPath.isEmpty()) {
        SteamArtworkStore::finishDeletion(stagedPath);
    }
}

void removeInstalledArtworkIfUnowned(const QString &profileDirectory,
                                     const StagedSteamArtwork &artwork,
                                     bool installedNewFile,
                                     storage::SteamRepository &steam,
                                     storage::SteamReleaseRepository &steamReleases,
                                     storage::SteamGameUpdatesRepository &gameUpdates,
                                     storage::SteamGameCatalogRepository &catalog)
{
    if (!installedNewFile) return;
    const auto priceOwner = steam.hasArtworkFilename(artwork.asset.filename);
    if (!priceOwner.succeeded() || priceOwner.value) return;
    const auto releaseOwner = steamReleases.hasArtworkFilename(artwork.asset.filename);
    if (!releaseOwner.succeeded() || releaseOwner.value) return;
    const auto updatesOwner = gameUpdates.hasArtworkFilename(artwork.asset.filename);
    if (!updatesOwner.succeeded() || updatesOwner.value) return;
    const auto catalogOwner = catalog.hasArtworkFilename(artwork.asset.filename);
    if (!catalogOwner.succeeded() || catalogOwner.value) return;
    SteamArtworkStore::removeInstalledIfNew(profileDirectory, artwork, true);
}

bool validStagedArtwork(const StagedSteamArtwork &artwork)
{
    return !artwork.asset.filename.isEmpty() && artwork.asset.byteSize > 0
        && (artwork.asset.mimeType == QStringLiteral("image/jpeg")
            || artwork.asset.mimeType == QStringLiteral("image/png")
            || artwork.asset.mimeType == QStringLiteral("image/webp"));
}

} // namespace

QVariantMap SteamGameUpdatesService::followValue(
    const domain::SteamGameUpdateFollow &follow, const QString &profileDirectory,
    int unreadNewsCount)
{
    const QString ownershipStatus =
        domain::steamGameUpdateOwnershipStatusName(follow.ownershipStatus);
    return {
        {QStringLiteral("appId"), follow.appId},
        {QStringLiteral("steamAppId"), follow.appId},
        {QStringLiteral("storeUrl"), follow.canonicalStoreUrl},
        {QStringLiteral("name"), follow.name},
        {QStringLiteral("artworkUrl"), follow.artwork.sourceUrl},
        {QStringLiteral("artworkFilename"), follow.artwork.cachedFilename},
        {QStringLiteral("artworkSource"), follow.artwork.cachedFilename.isEmpty()
             ? QString() : SteamArtworkStore::imageSource(
                   profileDirectory, follow.artwork.cachedFilename)},
        {QStringLiteral("ownershipStatus"), ownershipStatus},
        {QStringLiteral("ownershipLabel"),
             follow.ownershipStatus == domain::SteamGameUpdateOwnershipStatus::Owned
                 ? QStringLiteral("Owned") : QStringLiteral("Following")},
        {QStringLiteral("followingStatus"), QStringLiteral("following")},
        {QStringLiteral("isFollowing"), true},
        {QStringLiteral("unreadNewsCount"), unreadNewsCount},
        {QStringLiteral("addedAtUtc"), follow.addedAtUtc},
        {QStringLiteral("lastSuccessfulNewsCheckAtUtc"),
             follow.lastSuccessfulNewsCheckAtUtc},
        {QStringLiteral("lastAttemptedNewsCheckAtUtc"),
             follow.lastAttemptedNewsCheckAtUtc},
        {QStringLiteral("refreshState"),
             domain::steamGameUpdateRefreshStateName(follow.refreshState)},
        {QStringLiteral("lastRefreshError"), follow.lastRefreshError},
    };
}

storage::StorageCommand SteamGameUpdatesService::loadSnapshot(QString profileDirectory)
{
    return [profileDirectory = std::move(profileDirectory)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto records = repositories.gameUpdates.list(cancelled);
        if (!records.succeeded()) return failed(records.error);
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        if (!unreadCounts.succeeded()) return failed(unreadCounts.error);
        if (cancelled()) return failed(domain::AppError::cancelled());
        QVariantList games;
        games.reserve(records.value.size());
        for (const domain::SteamGameUpdateFollow &follow : records.value)
            games.push_back(followValue(
                follow, profileDirectory, unreadCounts.value.value(follow.appId, 0)));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("games"), games);
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::addFollow(
    QString profileDirectory, domain::SteamGameUpdateFollow follow)
{
    return [profileDirectory = std::move(profileDirectory), follow = std::move(follow)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const domain::SteamStoreUrl normalized =
            domain::normalizeSteamStoreUrl(follow.canonicalStoreUrl);
        if (!normalized.isValid() || normalized.appId != follow.appId)
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        follow.canonicalStoreUrl = normalized.canonicalUrl;
        const auto inserted = repositories.gameUpdates.add(std::move(follow), cancelled);
        if (!inserted.succeeded()) return failed(inserted.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("game"),
                            followValue(inserted.value, profileDirectory));
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::setOwnershipStatus(
    QString profileDirectory, quint32 appId,
    domain::SteamGameUpdateOwnershipStatus status)
{
    return [profileDirectory = std::move(profileDirectory), appId, status](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto updated = repositories.gameUpdates.setOwnershipStatus(appId, status);
        if (!updated.succeeded()) return failed(updated.error);
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        if (!unreadCounts.succeeded()) return failed(unreadCounts.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("game"),
                            followValue(updated.value, profileDirectory,
                                        unreadCounts.value.value(appId, 0)));
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::recordNewsCheck(
    QString profileDirectory, quint32 appId, QString attemptedAtUtc,
    bool succeeded, QString safeError)
{
    return [profileDirectory = std::move(profileDirectory), appId,
            attemptedAtUtc = std::move(attemptedAtUtc), succeeded,
            safeError = std::move(safeError)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto updated = repositories.gameUpdates.recordNewsCheck(
            appId, attemptedAtUtc, succeeded, safeError);
        if (!updated.succeeded()) return failed(updated.error);
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        if (!unreadCounts.succeeded()) return failed(unreadCounts.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("game"),
                            followValue(updated.value, profileDirectory,
                                        unreadCounts.value.value(appId, 0)));
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::loadNewsPage(
    std::optional<quint32> appId, bool unreadOnly, QString titleQuery, int pageSize,
    storage::SteamGameUpdateNewsCursor before)
{
    return [appId, unreadOnly, titleQuery = std::move(titleQuery), pageSize, before](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto page = repositories.gameUpdates.newsPage(
            appId, unreadOnly, titleQuery, pageSize, before, cancelled);
        if (!page.succeeded()) return failed(page.error);
        QVariantList items;
        items.reserve(page.value.items.size());
        for (const storage::SteamGameUpdateNewsItem &item : page.value.items) {
            const domain::SteamNewsArticle &article = item.article;
            QVariantMap value{
                {QStringLiteral("appId"), article.appId},
                {QStringLiteral("gameName"), item.gameName},
                {QStringLiteral("newsId"), article.newsId},
                {QStringLiteral("title"), article.title},
                {QStringLiteral("publishedAtUnixSeconds"), article.publishedAtUnixSeconds},
                {QStringLiteral("sourceUrl"), article.sourceUrl},
                {QStringLiteral("author"), article.author},
                {QStringLiteral("feedLabel"), article.feedLabel},
                {QStringLiteral("feedName"), article.feedName},
                {QStringLiteral("feedType"), article.feedType
                     ? QVariant(*article.feedType) : QVariant()},
                {QStringLiteral("contentText"), article.contentText},
                {QStringLiteral("externalUrl"), article.externalUrl},
                {QStringLiteral("isRead"), item.isRead},
                {QStringLiteral("discoveredAtUtc"), item.discoveredAtUtc},
            };
            items.push_back(std::move(value));
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("items"), items);
        reply.values.insert(QStringLiteral("hasMore"), page.value.hasMore);
        if (page.value.hasMore) {
            reply.values.insert(QStringLiteral("nextCursor"), QVariantMap{
                {QStringLiteral("publishedAtUnixSeconds"),
                 page.value.nextCursor.publishedAtUnixSeconds},
                {QStringLiteral("appId"), page.value.nextCursor.appId},
                {QStringLiteral("newsId"), page.value.nextCursor.newsId},
            });
        }
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::oldestNewsPublishedAt(quint32 appId)
{
    return [appId](storage::StorageRepositories &repositories,
                   const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto oldest = repositories.gameUpdates.oldestNewsPublishedAt(appId);
        if (!oldest.succeeded()) return failed(oldest.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("publishedAtUnixSeconds"),
                            oldest.value.value_or(0));
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::saveNewsPage(
    quint32 appId, QVector<domain::SteamNewsArticle> articles,
    QString discoveredAtUtc)
{
    return [appId, articles = std::move(articles),
            discoveredAtUtc = std::move(discoveredAtUtc)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failed(domain::AppError::cancelled());
        for (domain::SteamNewsArticle &article : articles) {
            if (article.contentText.size()
                > storage::SteamGameUpdatesRepository::maximumArticleContentCharacters) {
                article.contentText.truncate(
                    storage::SteamGameUpdatesRepository::maximumArticleContentCharacters);
            }
        }
        const auto saved = repositories.gameUpdates.upsertNewsPage(
            appId, articles, discoveredAtUtc, cancelled);
        if (!saved.succeeded()) return failed(saved.error);
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        if (!unreadCounts.succeeded()) return failed(unreadCounts.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("insertedCount"), saved.value.insertedCount);
        reply.values.insert(QStringLiteral("refreshedCount"), saved.value.refreshedCount);
        reply.values.insert(QStringLiteral("unreadNewsCount"),
                            unreadCounts.value.value(appId, 0));
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::setNewsRead(
    quint32 appId, QString newsId, bool isRead)
{
    return [appId, newsId = std::move(newsId), isRead](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto updated = repositories.gameUpdates.setNewsRead(appId, newsId, isRead);
        if (!updated.succeeded()) return failed(updated.error);
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        if (!unreadCounts.succeeded()) return failed(unreadCounts.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("appId"), appId);
        reply.values.insert(QStringLiteral("newsId"), newsId);
        reply.values.insert(QStringLiteral("isRead"), isRead);
        reply.values.insert(QStringLiteral("unreadNewsCount"),
                            unreadCounts.value.value(appId, 0));
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::setAllNewsRead(
    quint32 appId, bool isRead)
{
    return [appId, isRead](storage::StorageRepositories &repositories,
                           const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto updated = repositories.gameUpdates.setAllNewsRead(appId, isRead);
        if (!updated.succeeded()) return failed(updated.error);
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        if (!unreadCounts.succeeded()) return failed(unreadCounts.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("appId"), appId);
        reply.values.insert(QStringLiteral("isRead"), isRead);
        reply.values.insert(QStringLiteral("changedCount"), updated.value);
        reply.values.insert(QStringLiteral("unreadNewsCount"),
                            unreadCounts.value.value(appId, 0));
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::removeFollow(
    QString profileDirectory, quint32 appId)
{
    return [profileDirectory = std::move(profileDirectory), appId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto existing = repositories.gameUpdates.get(appId);
        if (!existing.succeeded()) return failed(existing.error);
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto removed = repositories.gameUpdates.remove(appId);
        if (!removed.succeeded()) return failed(removed.error);
        removeArtworkIfUnowned(profileDirectory, existing.value.artwork.cachedFilename,
                               repositories.steam, repositories.steamReleases,
                               repositories.gameUpdates, repositories.steamCatalog);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("deletedAppId"), appId);
        return reply;
    };
}

storage::StorageCommand SteamGameUpdatesService::saveArtwork(
    QString profileDirectory, quint32 appId, QString expectedSourceUrl,
    StagedSteamArtwork artwork)
{
    return [profileDirectory = std::move(profileDirectory), appId,
            expectedSourceUrl = std::move(expectedSourceUrl), artwork = std::move(artwork)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(domain::AppError::cancelled());
        }
        if (!validStagedArtwork(artwork)) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }
        const auto existing = repositories.gameUpdates.get(appId);
        if (!existing.succeeded()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(existing.error);
        }
        if (expectedSourceUrl.isEmpty()
            || expectedSourceUrl != existing.value.artwork.sourceUrl) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(domain::AppError(domain::ErrorCategory::Conflict));
        }
        bool installedNewFile = false;
        if (!SteamArtworkStore::install(profileDirectory, artwork, &installedNewFile)) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(domain::AppError(domain::ErrorCategory::Storage));
        }
        if (cancelled()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            removeInstalledArtworkIfUnowned(
                profileDirectory, artwork, installedNewFile, repositories.steam,
                repositories.steamReleases, repositories.gameUpdates,
                repositories.steamCatalog);
            return failed(domain::AppError::cancelled());
        }

        domain::SteamGameUpdateArtworkMetadata cached = existing.value.artwork;
        cached.cachedFilename = artwork.asset.filename;
        cached.mimeType = artwork.asset.mimeType;
        cached.byteSize = artwork.asset.byteSize;
        const auto saved = repositories.gameUpdates.setArtwork(
            appId, expectedSourceUrl, cached);
        if (!saved.succeeded()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            removeInstalledArtworkIfUnowned(
                profileDirectory, artwork, installedNewFile, repositories.steam,
                repositories.steamReleases, repositories.gameUpdates,
                repositories.steamCatalog);
            return failed(saved.error);
        }
        SteamArtworkStore::discard(profileDirectory, artwork);
        if (existing.value.artwork.cachedFilename != artwork.asset.filename) {
            removeArtworkIfUnowned(
                profileDirectory, existing.value.artwork.cachedFilename,
                repositories.steam, repositories.steamReleases, repositories.gameUpdates,
                repositories.steamCatalog);
        }
        const auto refreshed = repositories.gameUpdates.get(appId);
        if (!refreshed.succeeded()) return failed(refreshed.error);
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        if (!unreadCounts.succeeded()) return failed(unreadCounts.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("game"),
                            followValue(refreshed.value, profileDirectory,
                                        unreadCounts.value.value(appId, 0)));
        return reply;
    };
}

} // namespace something::services
