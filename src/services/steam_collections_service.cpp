#include "steam_collections_service.h"

#include "steam_artwork_store.h"

#include <QHash>
#include <QVariantList>

#include <functional>
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
                            storage::StorageRepositories &repositories)
{
    if (filename.isEmpty()) return;
    const auto priceOwner = repositories.steam.hasArtworkFilename(filename);
    if (!priceOwner.succeeded() || priceOwner.value) return;
    const auto releaseOwner = repositories.steamReleases.hasArtworkFilename(filename);
    if (!releaseOwner.succeeded() || releaseOwner.value) return;
    const auto updatesOwner = repositories.gameUpdates.hasArtworkFilename(filename);
    if (!updatesOwner.succeeded() || updatesOwner.value) return;
    const auto catalogOwner = repositories.steamCatalog.hasArtworkFilename(filename);
    if (!catalogOwner.succeeded() || catalogOwner.value) return;
    QString originalPath;
    QString stagedPath;
    if (SteamArtworkStore::stageDeletion(profileDirectory, filename, &stagedPath,
                                         &originalPath)
        && !stagedPath.isEmpty()) {
        SteamArtworkStore::finishDeletion(stagedPath);
    }
}

void removeInstalledIfUnowned(const QString &profileDirectory,
                              const StagedSteamArtwork &artwork,
                              bool installedNewFile,
                              storage::StorageRepositories &repositories)
{
    if (!installedNewFile) return;
    const auto priceOwner = repositories.steam.hasArtworkFilename(artwork.asset.filename);
    if (!priceOwner.succeeded() || priceOwner.value) return;
    const auto releaseOwner = repositories.steamReleases.hasArtworkFilename(artwork.asset.filename);
    if (!releaseOwner.succeeded() || releaseOwner.value) return;
    const auto updatesOwner = repositories.gameUpdates.hasArtworkFilename(artwork.asset.filename);
    if (!updatesOwner.succeeded() || updatesOwner.value) return;
    const auto catalogOwner = repositories.steamCatalog.hasArtworkFilename(artwork.asset.filename);
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

QVariantMap collectionValue(const domain::SteamCollection &collection)
{
    return {
        {QStringLiteral("id"), collection.id},
        {QStringLiteral("name"), collection.name},
        {QStringLiteral("description"), collection.description},
        {QStringLiteral("color"), collection.color},
        {QStringLiteral("icon"), collection.icon},
        {QStringLiteral("displayOrder"), collection.displayOrder},
        {QStringLiteral("createdAt"), collection.createdAt},
        {QStringLiteral("updatedAt"), collection.updatedAt},
    };
}

QString statusName(
    const std::optional<domain::SteamCollectionGameStatus> &status)
{
    return status ? domain::steamCollectionGameStatusName(*status) : QString();
}

QVariantMap membershipValue(
    const domain::SteamCollectionGameEntry &entry,
    const QString &collectionName, const QString &profileDirectory)
{
    return {
        {QStringLiteral("collectionId"), entry.collectionId},
        {QStringLiteral("collectionName"), collectionName},
        {QStringLiteral("appId"), entry.game.appId},
        {QStringLiteral("steamAppId"), entry.game.appId},
        {QStringLiteral("storeUrl"), entry.game.canonicalStoreUrl},
        {QStringLiteral("name"), entry.game.name},
        {QStringLiteral("artworkUrl"), entry.game.artwork.sourceUrl},
        {QStringLiteral("artworkFilename"), entry.game.artwork.cachedFilename},
        {QStringLiteral("artworkSource"), entry.game.artwork.cachedFilename.isEmpty()
             ? QString() : SteamArtworkStore::imageSource(
                   profileDirectory, entry.game.artwork.cachedFilename)},
        {QStringLiteral("status"), statusName(entry.status)},
        {QStringLiteral("rating"), entry.personalRating
             ? QVariant(*entry.personalRating) : QVariant()},
        {QStringLiteral("comment"), entry.personalComment},
        {QStringLiteral("createdAt"), entry.createdAt},
        {QStringLiteral("updatedAt"), entry.updatedAt},
    };
}

QVariantMap catalogValue(const domain::SteamGameCatalogEntry &game,
                         const QString &profileDirectory)
{
    return {
        {QStringLiteral("appId"), game.appId},
        {QStringLiteral("steamAppId"), game.appId},
        {QStringLiteral("storeUrl"), game.canonicalStoreUrl},
        {QStringLiteral("name"), game.name},
        {QStringLiteral("artworkUrl"), game.artwork.sourceUrl},
        {QStringLiteral("artworkFilename"), game.artwork.cachedFilename},
        {QStringLiteral("artworkSource"), game.artwork.cachedFilename.isEmpty()
             ? QString() : SteamArtworkStore::imageSource(
                   profileDirectory, game.artwork.cachedFilename)},
    };
}

storage::StorageReply snapshotReply(
    const QString &profileDirectory, storage::StorageRepositories &repositories,
    const std::function<bool()> &cancelled, QVariantMap mutation = {})
{
    if (cancelled()) return failed(domain::AppError::cancelled());
    const auto collections = repositories.steamCollections.list();
    if (!collections.succeeded()) return failed(collections.error);
    const auto memberships = repositories.steamCollections.allGames();
    if (!memberships.succeeded()) return failed(memberships.error);
    const auto catalog = repositories.steamCatalog.list();
    if (!catalog.succeeded()) return failed(catalog.error);
    if (cancelled()) return failed(domain::AppError::cancelled());

    QHash<qint64, QString> collectionNames;
    QVariantList collectionValues;
    collectionValues.reserve(collections.value.size());
    for (const domain::SteamCollection &collection : collections.value) {
        collectionNames.insert(collection.id, collection.name);
        collectionValues.push_back(collectionValue(collection));
    }
    QVariantList membershipValues;
    membershipValues.reserve(memberships.value.size());
    for (const domain::SteamCollectionGameEntry &entry : memberships.value) {
        membershipValues.push_back(membershipValue(
            entry, collectionNames.value(entry.collectionId), profileDirectory));
    }
    QVariantList catalogValues;
    catalogValues.reserve(catalog.value.size());
    for (const domain::SteamGameCatalogEntry &game : catalog.value)
        catalogValues.push_back(catalogValue(game, profileDirectory));

    storage::StorageReply reply;
    reply.values.insert(QStringLiteral("collections"), collectionValues);
    reply.values.insert(QStringLiteral("memberships"), membershipValues);
    reply.values.insert(QStringLiteral("catalog"), catalogValues);
    if (!mutation.isEmpty())
        reply.values.insert(QStringLiteral("mutation"), std::move(mutation));
    return reply;
}

} // namespace

storage::StorageCommand SteamCollectionsService::loadSnapshot(QString profileDirectory)
{
    return [profileDirectory = std::move(profileDirectory)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        return snapshotReply(profileDirectory, repositories, cancelled);
    };
}

storage::StorageCommand SteamCollectionsService::createCollection(
    QString profileDirectory, domain::SteamCollectionInput input)
{
    return [profileDirectory = std::move(profileDirectory), input = std::move(input)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto created = repositories.steamCollections.create(std::move(input));
        if (!created.succeeded()) return failed(created.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("collection-created")},
             {QStringLiteral("collectionId"), created.value.id},
             {QStringLiteral("name"), created.value.name}});
    };
}

storage::StorageCommand SteamCollectionsService::updateCollection(
    QString profileDirectory, qint64 collectionId,
    domain::SteamCollectionInput input)
{
    return [profileDirectory = std::move(profileDirectory), collectionId,
            input = std::move(input)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto updated = repositories.steamCollections.update(
            collectionId, std::move(input));
        if (!updated.succeeded()) return failed(updated.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("collection-updated")},
             {QStringLiteral("collectionId"), updated.value.id},
             {QStringLiteral("name"), updated.value.name}});
    };
}

storage::StorageCommand SteamCollectionsService::reorderCollections(
    QString profileDirectory, QVector<qint64> orderedIds)
{
    return [profileDirectory = std::move(profileDirectory),
            orderedIds = std::move(orderedIds)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto reordered = repositories.steamCollections.reorder(orderedIds);
        if (!reordered.succeeded()) return failed(reordered.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("collections-reordered")}});
    };
}

storage::StorageCommand SteamCollectionsService::deleteCollection(
    QString profileDirectory, qint64 collectionId)
{
    return [profileDirectory = std::move(profileDirectory), collectionId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto removed = repositories.steamCollections.remove(collectionId);
        if (!removed.succeeded()) return failed(removed.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("collection-deleted")},
             {QStringLiteral("collectionId"), removed.value.deletedId},
             {QStringLiteral("removedMemberships"), removed.value.removedMemberships}});
    };
}

storage::StorageCommand SteamCollectionsService::addGame(
    QString profileDirectory, qint64 collectionId,
    domain::SteamGameCatalogDetails details)
{
    return [profileDirectory = std::move(profileDirectory), collectionId,
            details = std::move(details)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failed(domain::AppError::cancelled());
        domain::SteamCollectionGameInput input;
        input.steamAppId = details.appId;
        const auto added = repositories.steamCollections.addGame(
            collectionId, std::move(details), std::move(input));
        if (!added.succeeded()) return failed(added.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("game-added")},
             {QStringLiteral("collectionId"), collectionId},
             {QStringLiteral("appId"), added.value.game.appId},
             {QStringLiteral("name"), added.value.game.name}});
    };
}

storage::StorageCommand SteamCollectionsService::addExistingGame(
    QString profileDirectory, qint64 collectionId, quint32 appId)
{
    return [profileDirectory = std::move(profileDirectory), collectionId, appId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto game = repositories.steamCatalog.get(appId);
        if (!game.succeeded()) return failed(game.error);
        domain::SteamGameCatalogDetails details{
            game.value.appId, game.value.canonicalStoreUrl, game.value.name,
            game.value.artwork.sourceUrl};
        domain::SteamCollectionGameInput input;
        input.steamAppId = appId;
        const auto added = repositories.steamCollections.addGame(
            collectionId, std::move(details), std::move(input));
        if (!added.succeeded()) return failed(added.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("game-added")},
             {QStringLiteral("collectionId"), collectionId},
             {QStringLiteral("appId"), appId},
             {QStringLiteral("name"), added.value.game.name}});
    };
}

storage::StorageCommand SteamCollectionsService::updateGame(
    QString profileDirectory, qint64 collectionId,
    domain::SteamCollectionGameInput input)
{
    return [profileDirectory = std::move(profileDirectory), collectionId,
            input = std::move(input)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto updated = repositories.steamCollections.updateGame(
            collectionId, std::move(input));
        if (!updated.succeeded()) return failed(updated.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("game-updated")},
             {QStringLiteral("collectionId"), collectionId},
             {QStringLiteral("appId"), updated.value.game.appId}});
    };
}

storage::StorageCommand SteamCollectionsService::removeGame(
    QString profileDirectory, qint64 collectionId, quint32 appId)
{
    return [profileDirectory = std::move(profileDirectory), collectionId, appId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto removed = repositories.steamCollections.removeGame(collectionId, appId);
        if (!removed.succeeded()) return failed(removed.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("game-removed")},
             {QStringLiteral("collectionId"), collectionId},
             {QStringLiteral("appId"), appId}});
    };
}

storage::StorageCommand SteamCollectionsService::moveGame(
    QString profileDirectory, qint64 sourceCollectionId,
    qint64 targetCollectionId, quint32 appId)
{
    return [profileDirectory = std::move(profileDirectory), sourceCollectionId,
            targetCollectionId, appId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto moved = repositories.steamCollections.moveGame(
            sourceCollectionId, targetCollectionId, appId);
        if (!moved.succeeded()) return failed(moved.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("game-moved")},
             {QStringLiteral("sourceCollectionId"), sourceCollectionId},
             {QStringLiteral("targetCollectionId"), targetCollectionId},
             {QStringLiteral("appId"), appId},
             {QStringLiteral("name"), moved.value.game.name}});
    };
}

storage::StorageCommand SteamCollectionsService::copyGame(
    QString profileDirectory, qint64 sourceCollectionId,
    qint64 targetCollectionId, quint32 appId)
{
    return [profileDirectory = std::move(profileDirectory), sourceCollectionId,
            targetCollectionId, appId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto copied = repositories.steamCollections.copyGame(
            sourceCollectionId, targetCollectionId, appId);
        if (!copied.succeeded()) return failed(copied.error);
        return snapshotReply(profileDirectory, repositories, cancelled,
            {{QStringLiteral("type"), QStringLiteral("game-copied")},
             {QStringLiteral("sourceCollectionId"), sourceCollectionId},
             {QStringLiteral("targetCollectionId"), targetCollectionId},
             {QStringLiteral("appId"), appId},
             {QStringLiteral("name"), copied.value.game.name}});
    };
}

storage::StorageCommand SteamCollectionsService::saveArtwork(
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
        const auto existing = repositories.steamCatalog.get(appId);
        if (!existing.succeeded()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(existing.error);
        }
        if (expectedSourceUrl.isEmpty()
            || existing.value.artwork.sourceUrl != expectedSourceUrl) {
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
            removeInstalledIfUnowned(profileDirectory, artwork, installedNewFile,
                                     repositories);
            return failed(domain::AppError::cancelled());
        }

        domain::SteamGameCatalogArtwork cached = existing.value.artwork;
        cached.cachedFilename = artwork.asset.filename;
        cached.mimeType = artwork.asset.mimeType;
        cached.byteSize = artwork.asset.byteSize;
        const auto saved = repositories.steamCatalog.setArtwork(
            appId, expectedSourceUrl, cached);
        if (!saved.succeeded()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            removeInstalledIfUnowned(profileDirectory, artwork, installedNewFile,
                                     repositories);
            return failed(saved.error);
        }
        SteamArtworkStore::discard(profileDirectory, artwork);
        if (existing.value.artwork.cachedFilename != artwork.asset.filename) {
            removeArtworkIfUnowned(profileDirectory,
                                   existing.value.artwork.cachedFilename, repositories);
        }
        return snapshotReply(profileDirectory, repositories, cancelled);
    };
}

} // namespace something::services
