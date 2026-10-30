#include "steam_release_service.h"

#include "../domain/steam_game.h"

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
                            storage::SteamGameUpdatesRepository &gameUpdates)
{
    if (filename.isEmpty()) return;
    const auto priceOwner = steam.hasArtworkFilename(filename);
    if (!priceOwner.succeeded() || priceOwner.value) return;
    const auto releaseOwner = steamReleases.hasArtworkFilename(filename);
    if (!releaseOwner.succeeded() || releaseOwner.value) return;
    const auto updatesOwner = gameUpdates.hasArtworkFilename(filename);
    if (!updatesOwner.succeeded() || updatesOwner.value) return;
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
                              storage::SteamRepository &steam,
                              storage::SteamReleaseRepository &steamReleases,
                              storage::SteamGameUpdatesRepository &gameUpdates)
{
    if (!installedNewFile) return;
    const auto releaseOwner = steamReleases.hasArtworkFilename(artwork.asset.filename);
    if (!releaseOwner.succeeded() || releaseOwner.value) return;
    const auto priceOwner = steam.hasArtworkFilename(artwork.asset.filename);
    if (!priceOwner.succeeded() || priceOwner.value) return;
    const auto updatesOwner = gameUpdates.hasArtworkFilename(artwork.asset.filename);
    if (!updatesOwner.succeeded() || updatesOwner.value) return;
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

QVariantMap SteamReleaseService::releaseValue(const domain::SteamTrackedRelease &release,
                                              const QString &profileDirectory)
{
    return {
        {QStringLiteral("appId"), release.appId},
        {QStringLiteral("steamAppId"), release.appId},
        {QStringLiteral("storeUrl"), release.canonicalStoreUrl},
        {QStringLiteral("title"), release.title},
        {QStringLiteral("artworkUrl"), release.artwork.sourceUrl},
        {QStringLiteral("artworkFilename"), release.artwork.cachedFilename},
        {QStringLiteral("artworkMimeType"), release.artwork.mimeType},
        {QStringLiteral("artworkByteSize"), release.artwork.byteSize},
        {QStringLiteral("artworkSource"), release.artwork.cachedFilename.isEmpty()
             ? QString() : SteamArtworkStore::imageSource(
                   profileDirectory, release.artwork.cachedFilename)},
        {QStringLiteral("releaseDateText"), release.releaseDate.announcedText},
        {QStringLiteral("datePrecision"),
         domain::steamReleaseDatePrecisionName(release.releaseDate.precision)},
        {QStringLiteral("hasExactDate"), release.releaseDate.hasExactDate()},
        {QStringLiteral("exactDate"), release.releaseDate.exactDate
             ? release.releaseDate.exactDate->toString(Qt::ISODate) : QString()},
        {QStringLiteral("windowStart"), release.releaseDate.windowStart
             ? release.releaseDate.windowStart->toString(Qt::ISODate) : QString()},
        {QStringLiteral("windowEnd"), release.releaseDate.windowEnd
             ? release.releaseDate.windowEnd->toString(Qt::ISODate) : QString()},
        {QStringLiteral("status"), domain::steamReleaseStatusName(release.status)},
        {QStringLiteral("followedAtUtc"), release.followedAtUtc},
        {QStringLiteral("lastSuccessfulUpdateAtUtc"), release.lastSuccessfulUpdateAtUtc},
        {QStringLiteral("lastAttemptedUpdateAtUtc"), release.lastAttemptedUpdateAtUtc},
        {QStringLiteral("refreshState"),
         domain::steamReleaseRefreshStateName(release.refreshState)},
        {QStringLiteral("refreshError"), release.refreshError},
        {QStringLiteral("dateChangeUnacknowledged"), release.dateChangeUnacknowledged},
    };
}

storage::StorageCommand SteamReleaseService::loadSnapshot(QString profileDirectory)
{
    return [profileDirectory = std::move(profileDirectory)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto records = repositories.steamReleases.list();
        if (!records.succeeded()) return failed(records.error);
        if (cancelled()) return failed(domain::AppError::cancelled());
        QVariantList releases;
        releases.reserve(records.value.size());
        for (const domain::SteamTrackedRelease &release : records.value)
            releases.push_back(releaseValue(release, profileDirectory));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("releases"), releases);
        return reply;
    };
}

storage::StorageCommand SteamReleaseService::addRelease(QString storeUrl,
                                                        domain::SteamReleaseDetails details,
                                                        QString followedAtUtc)
{
    return [storeUrl = std::move(storeUrl), details = std::move(details),
            followedAtUtc = std::move(followedAtUtc)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const domain::SteamStoreUrl normalized = domain::normalizeSteamStoreUrl(storeUrl);
        if (!normalized.isValid() || normalized.appId != details.appId
            || !domain::isValidSteamReleaseDetails(details) || followedAtUtc.isEmpty()) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }
        domain::SteamTrackedRelease release;
        release.appId = details.appId;
        release.canonicalStoreUrl = normalized.canonicalUrl;
        release.title = details.title.trimmed();
        release.artwork.sourceUrl = details.artworkUrl;
        release.releaseDate = details.releaseDate;
        release.status = details.status;
        release.followedAtUtc = followedAtUtc;
        release.lastSuccessfulUpdateAtUtc = followedAtUtc;
        release.lastAttemptedUpdateAtUtc = followedAtUtc;
        release.refreshState = domain::SteamReleaseRefreshState::Succeeded;

        const auto inserted = repositories.steamReleases.add(std::move(release), cancelled);
        if (!inserted.succeeded()) return failed(inserted.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("release"),
                            releaseValue(inserted.value, QString()));
        return reply;
    };
}

storage::StorageCommand SteamReleaseService::applyRefresh(
    QString profileDirectory, QVector<storage::SteamReleaseRefreshUpdate> updates)
{
    return [profileDirectory = std::move(profileDirectory), updates = std::move(updates)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        const auto previous = repositories.steamReleases.list();
        if (!previous.succeeded()) return failed(previous.error);
        const auto applied = repositories.steamReleases.applyRefreshBatch(updates, cancelled);
        if (!applied.succeeded()) return failed(applied.error);

        // A changed artwork URL clears the cached release image in the same
        // transaction. Remove its old file only after checking every Steam
        // feature that can reference shared artwork.
        for (const storage::SteamReleaseRefreshUpdate &update : updates) {
            if (!update.refreshedDetails) continue;
            for (const domain::SteamTrackedRelease &oldRelease : previous.value) {
                if (oldRelease.appId == update.appId
                    && oldRelease.artwork.sourceUrl != update.refreshedDetails->artworkUrl
                    && !oldRelease.artwork.cachedFilename.isEmpty()) {
                    removeArtworkIfUnowned(profileDirectory,
                                           oldRelease.artwork.cachedFilename,
                                           repositories.steam, repositories.steamReleases,
                                           repositories.gameUpdates);
                    break;
                }
            }
        }
        // Once the refresh transaction commits, return its snapshot so the UI
        // reconciles with durable data even if cancellation arrives afterward.
        const auto records = repositories.steamReleases.list();
        if (!records.succeeded()) return failed(records.error);
        QVariantList releases;
        releases.reserve(records.value.size());
        for (const domain::SteamTrackedRelease &release : records.value)
            releases.push_back(releaseValue(release, profileDirectory));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("releases"), releases);
        return reply;
    };
}

storage::StorageCommand SteamReleaseService::removeRelease(QString profileDirectory,
                                                           quint32 appId)
{
    return [profileDirectory = std::move(profileDirectory), appId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto existing = repositories.steamReleases.get(appId);
        if (!existing.succeeded()) return failed(existing.error);
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto removed = repositories.steamReleases.remove(appId);
        if (!removed.succeeded()) return failed(removed.error);
        removeArtworkIfUnowned(profileDirectory, existing.value.artwork.cachedFilename,
                               repositories.steam, repositories.steamReleases,
                               repositories.gameUpdates);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("deletedAppId"), appId);
        return reply;
    };
}

storage::StorageCommand SteamReleaseService::acknowledgeDateChange(quint32 appId)
{
    return [appId](storage::StorageRepositories &repositories,
                   const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto acknowledged = repositories.steamReleases.acknowledgeDateChange(appId);
        if (!acknowledged.succeeded()) return failed(acknowledged.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("appId"), appId);
        return reply;
    };
}

storage::StorageCommand SteamReleaseService::saveArtwork(
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
        const auto existing = repositories.steamReleases.get(appId);
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
            removeInstalledIfUnowned(profileDirectory, artwork, installedNewFile,
                                     repositories.steam, repositories.steamReleases,
                                     repositories.gameUpdates);
            return failed(domain::AppError::cancelled());
        }

        domain::SteamReleaseArtworkMetadata cached = existing.value.artwork;
        cached.cachedFilename = artwork.asset.filename;
        cached.mimeType = artwork.asset.mimeType;
        cached.byteSize = artwork.asset.byteSize;
        const auto saved = repositories.steamReleases.setArtwork(
            appId, expectedSourceUrl, cached);
        if (!saved.succeeded()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            removeInstalledIfUnowned(profileDirectory, artwork, installedNewFile,
                                     repositories.steam, repositories.steamReleases,
                                     repositories.gameUpdates);
            return failed(saved.error);
        }
        SteamArtworkStore::discard(profileDirectory, artwork);
        if (existing.value.artwork.cachedFilename != artwork.asset.filename) {
            removeArtworkIfUnowned(profileDirectory, existing.value.artwork.cachedFilename,
                                   repositories.steam, repositories.steamReleases,
                                   repositories.gameUpdates);
        }
        const auto refreshed = repositories.steamReleases.get(appId);
        if (!refreshed.succeeded()) return failed(refreshed.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("release"),
                            releaseValue(refreshed.value, profileDirectory));
        return reply;
    };
}

storage::StorageCommand SteamReleaseService::calendarEvents(QDate startDate, QDate endDate)
{
    return [startDate, endDate](storage::StorageRepositories &repositories,
                                const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto events = repositories.steamReleases.exactDateEvents(
            startDate, endDate, cancelled);
        if (!events.succeeded()) return failed(events.error);
        QVariantList rows;
        rows.reserve(events.value.size());
        for (const domain::SteamTrackedRelease &release : events.value) {
            if (!release.releaseDate.hasExactDate()) {
                continue;
            }
            rows.push_back(QVariantMap{
                {QStringLiteral("appId"), release.appId},
                {QStringLiteral("title"), release.title},
                {QStringLiteral("storeUrl"), release.canonicalStoreUrl},
                {QStringLiteral("exactDate"), release.releaseDate.exactDate->toString(Qt::ISODate)},
                {QStringLiteral("releaseDateText"), release.releaseDate.announcedText},
                {QStringLiteral("datePrecision"),
                 domain::steamReleaseDatePrecisionName(release.releaseDate.precision)},
                {QStringLiteral("status"), domain::steamReleaseStatusName(release.status)}});
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("releases"), rows);
        return reply;
    };
}

} // namespace something::services
