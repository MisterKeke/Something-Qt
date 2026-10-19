#pragma once

#include "steam_artwork_store.h"
#include "../domain/steam_game.h"
#include "../storage/storage_worker.h"

namespace something::services {

class SteamService final {
public:
    static QVariantList countryValues();
    static storage::StorageCommand loadSnapshot(QString profileDirectory);
    static storage::StorageCommand setCountry(QString countryCode);
    static storage::StorageCommand addGame(QString profileDirectory, QString canonicalUrl,
                                           QString countryCode,
                                           domain::SteamGameDetails details,
                                           QString checkedAt);
    static storage::StorageCommand applyRefresh(QString profileDirectory,
                                                QVector<storage::SteamRefreshUpdate> updates);
    static storage::StorageCommand deleteGame(QString profileDirectory, qint64 gameId);
    static storage::StorageCommand saveArtwork(QString profileDirectory, qint64 gameId,
                                               StagedSteamArtwork artwork);

    static QVariantMap gameValue(const storage::SteamGame &game,
                                 const QString &profileDirectory);
};

} // namespace something::services
