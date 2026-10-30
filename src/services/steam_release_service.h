#pragma once

#include "steam_artwork_store.h"
#include "../domain/steam_release.h"
#include "../storage/storage_worker.h"

#include <QDate>

namespace something::services {

class SteamReleaseService final {
public:
    static storage::StorageCommand loadSnapshot(QString profileDirectory);
    static storage::StorageCommand addRelease(QString storeUrl,
                                              domain::SteamReleaseDetails details,
                                              QString followedAtUtc);
    static storage::StorageCommand applyRefresh(
        QString profileDirectory, QVector<storage::SteamReleaseRefreshUpdate> updates);
    static storage::StorageCommand removeRelease(QString profileDirectory, quint32 appId);
    static storage::StorageCommand acknowledgeDateChange(quint32 appId);
    static storage::StorageCommand saveArtwork(QString profileDirectory, quint32 appId,
                                               QString expectedSourceUrl,
                                               StagedSteamArtwork artwork);
    static storage::StorageCommand calendarEvents(QDate startDate, QDate endDate);

    static QVariantMap releaseValue(const domain::SteamTrackedRelease &release,
                                    const QString &profileDirectory);
};

} // namespace something::services
