#pragma once

#include "app_error.h"

#include <QString>
#include <QtGlobal>

namespace something::domain {

// Common Steam identity and Store metadata. Feature membership and its
// feature-specific state live outside this catalog value.
struct SteamGameCatalogDetails final {
    quint32 appId{0};
    QString canonicalStoreUrl;
    QString name;
    QString artworkUrl;
};

struct SteamGameCatalogArtwork final {
    QString sourceUrl;
    QString cachedFilename;
    QString mimeType;
    qint64 byteSize{0};
};

// Persisted by App ID, independently of price, release, update, or collection
// membership. Feature rows refer to this identity without owning its artwork.
struct SteamGameCatalogEntry final {
    quint32 appId{0};
    QString canonicalStoreUrl;
    QString name;
    SteamGameCatalogArtwork artwork;
    QString createdAt;
    QString updatedAt;
};

AppError normalizeSteamGameCatalogDetails(SteamGameCatalogDetails &details);
bool isValidSteamGameCatalogDetails(const SteamGameCatalogDetails &details);
bool isValidSteamGameCatalogArtwork(quint32 appId,
                                   const SteamGameCatalogArtwork &artwork);
bool isValidSteamGameCatalogEntry(const SteamGameCatalogEntry &entry);

} // namespace something::domain
