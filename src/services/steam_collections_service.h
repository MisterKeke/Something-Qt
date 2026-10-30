#pragma once

#include "../domain/steam_collections.h"
#include "../domain/steam_game_catalog.h"
#include "steam_artwork_store.h"
#include "../storage/storage_worker.h"

#include <QVector>

namespace something::services {

class SteamCollectionsService final {
public:
    static storage::StorageCommand loadSnapshot(QString profileDirectory);
    static storage::StorageCommand createCollection(
        QString profileDirectory, domain::SteamCollectionInput input);
    static storage::StorageCommand updateCollection(
        QString profileDirectory, qint64 collectionId,
        domain::SteamCollectionInput input);
    static storage::StorageCommand reorderCollections(
        QString profileDirectory, QVector<qint64> orderedIds);
    static storage::StorageCommand deleteCollection(
        QString profileDirectory, qint64 collectionId);
    static storage::StorageCommand addGame(
        QString profileDirectory, qint64 collectionId,
        domain::SteamGameCatalogDetails details);
    static storage::StorageCommand addExistingGame(
        QString profileDirectory, qint64 collectionId, quint32 appId);
    static storage::StorageCommand updateGame(
        QString profileDirectory, qint64 collectionId,
        domain::SteamCollectionGameInput input);
    static storage::StorageCommand removeGame(
        QString profileDirectory, qint64 collectionId, quint32 appId);
    static storage::StorageCommand saveArtwork(
        QString profileDirectory, quint32 appId, QString expectedSourceUrl,
        StagedSteamArtwork artwork);
};

} // namespace something::services
