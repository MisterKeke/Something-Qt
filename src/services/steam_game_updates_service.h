#pragma once

#include "steam_artwork_store.h"
#include "../domain/steam_game_updates.h"
#include "../storage/storage_worker.h"

#include <optional>

namespace something::services {

class SteamGameUpdatesService final {
public:
    static storage::StorageCommand loadSnapshot(QString profileDirectory);
    static storage::StorageCommand addFollow(QString profileDirectory,
                                             domain::SteamGameUpdateFollow follow);
    static storage::StorageCommand setOwnershipStatus(
        QString profileDirectory, quint32 appId,
        domain::SteamGameUpdateOwnershipStatus status);
    static storage::StorageCommand recordNewsCheck(
        QString profileDirectory, quint32 appId, QString attemptedAtUtc,
        bool succeeded, QString safeError = {});
    static storage::StorageCommand loadNewsPage(
        std::optional<quint32> appId, bool unreadOnly, QString titleQuery, int pageSize,
        storage::SteamGameUpdateNewsCursor before = {});
    static storage::StorageCommand oldestNewsPublishedAt(quint32 appId);
    static storage::StorageCommand saveNewsPage(
        quint32 appId, QVector<domain::SteamNewsArticle> articles,
        QString discoveredAtUtc);
    static storage::StorageCommand setNewsRead(
        quint32 appId, QString newsId, bool isRead);
    static storage::StorageCommand setAllNewsRead(quint32 appId, bool isRead);
    static storage::StorageCommand removeFollow(QString profileDirectory, quint32 appId);
    static storage::StorageCommand saveArtwork(
        QString profileDirectory, quint32 appId, QString expectedSourceUrl,
        StagedSteamArtwork artwork);

    static QVariantMap followValue(const domain::SteamGameUpdateFollow &follow,
                                   const QString &profileDirectory,
                                   int unreadNewsCount = 0);
};

} // namespace something::services
