#pragma once

#include "steam_game.h"

#include <QString>
#include <QtGlobal>

namespace something::domain {

// This is a local label selected by the user. It is not a Steam purchase claim.
enum class SteamGameUpdateOwnershipStatus { Owned, NotOwned };

enum class SteamGameUpdateRefreshState { NeverAttempted, Succeeded, Failed };

struct SteamGameUpdateArtworkMetadata final {
    QString sourceUrl;
    QString cachedFilename;
    QString mimeType;
    qint64 byteSize{0};
};

// Presence in the independent follow table means news updates are followed.
// ownershipStatus only controls the user's local Owned/Following label.
struct SteamGameUpdateFollow final {
    quint32 appId{0};
    QString canonicalStoreUrl;
    QString name;
    SteamGameUpdateArtworkMetadata artwork;
    SteamGameUpdateOwnershipStatus ownershipStatus{
        SteamGameUpdateOwnershipStatus::NotOwned};
    QString addedAtUtc;
    QString lastSuccessfulNewsCheckAtUtc;
    QString lastAttemptedNewsCheckAtUtc;
    SteamGameUpdateRefreshState refreshState{
        SteamGameUpdateRefreshState::NeverAttempted};
    QString lastRefreshError;
};

QString steamGameUpdateOwnershipStatusName(SteamGameUpdateOwnershipStatus status);
QString steamGameUpdateRefreshStateName(SteamGameUpdateRefreshState state);
std::optional<SteamGameUpdateOwnershipStatus>
steamGameUpdateOwnershipStatusFromName(const QString &name);
std::optional<SteamGameUpdateRefreshState>
steamGameUpdateRefreshStateFromName(const QString &name);
bool isValidSteamGameUpdateFollow(const SteamGameUpdateFollow &follow);

} // namespace something::domain
