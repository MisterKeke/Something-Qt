#pragma once

#include "steam_artwork_store.h"
#include "../domain/steam_game_updates.h"
#include "../storage/storage_worker.h"

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
    static storage::StorageCommand removeFollow(QString profileDirectory, quint32 appId);
    static storage::StorageCommand saveArtwork(
        QString profileDirectory, quint32 appId, QString expectedSourceUrl,
        StagedSteamArtwork artwork);

    static QVariantMap followValue(const domain::SteamGameUpdateFollow &follow,
                                   const QString &profileDirectory);
};

} // namespace something::services
