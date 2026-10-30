#include "steam_game_updates.h"

#include <QDateTime>
namespace something::domain {
namespace {

bool validUtcInstant(const QString &value)
{
    if (value.isEmpty() || !value.endsWith(QLatin1Char('Z'))) return false;
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    return parsed.isValid() && parsed.timeSpec() == Qt::UTC;
}

} // namespace

QString steamGameUpdateOwnershipStatusName(SteamGameUpdateOwnershipStatus status)
{
    switch (status) {
    case SteamGameUpdateOwnershipStatus::Owned: return QStringLiteral("owned");
    case SteamGameUpdateOwnershipStatus::NotOwned: return QStringLiteral("not_owned");
    }
    return QStringLiteral("not_owned");
}

QString steamGameUpdateRefreshStateName(SteamGameUpdateRefreshState state)
{
    switch (state) {
    case SteamGameUpdateRefreshState::NeverAttempted:
        return QStringLiteral("never_attempted");
    case SteamGameUpdateRefreshState::Succeeded: return QStringLiteral("succeeded");
    case SteamGameUpdateRefreshState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("never_attempted");
}

std::optional<SteamGameUpdateOwnershipStatus>
steamGameUpdateOwnershipStatusFromName(const QString &name)
{
    if (name == QStringLiteral("owned")) return SteamGameUpdateOwnershipStatus::Owned;
    if (name == QStringLiteral("not_owned")) return SteamGameUpdateOwnershipStatus::NotOwned;
    return std::nullopt;
}

std::optional<SteamGameUpdateRefreshState>
steamGameUpdateRefreshStateFromName(const QString &name)
{
    if (name == QStringLiteral("never_attempted"))
        return SteamGameUpdateRefreshState::NeverAttempted;
    if (name == QStringLiteral("succeeded")) return SteamGameUpdateRefreshState::Succeeded;
    if (name == QStringLiteral("failed")) return SteamGameUpdateRefreshState::Failed;
    return std::nullopt;
}

bool isValidSteamGameUpdateFollow(const SteamGameUpdateFollow &follow)
{
    const SteamStoreUrl normalized = normalizeSteamStoreUrl(follow.canonicalStoreUrl);
    if (follow.appId == 0 || !normalized.isValid() || normalized.appId != follow.appId
        || normalized.canonicalUrl != follow.canonicalStoreUrl
        || follow.name.trimmed().isEmpty() || follow.name.trimmed().toUcs4().size() > 300
        || (follow.ownershipStatus != SteamGameUpdateOwnershipStatus::Owned
            && follow.ownershipStatus != SteamGameUpdateOwnershipStatus::NotOwned)
        || !isValidSteamGameCatalogArtwork(follow.appId, follow.artwork)
        || !validUtcInstant(follow.addedAtUtc)) {
        return false;
    }

    switch (follow.refreshState) {
    case SteamGameUpdateRefreshState::NeverAttempted:
        return follow.lastAttemptedNewsCheckAtUtc.isEmpty()
            && follow.lastSuccessfulNewsCheckAtUtc.isEmpty()
            && follow.lastRefreshError.isEmpty();
    case SteamGameUpdateRefreshState::Succeeded:
        return validUtcInstant(follow.lastAttemptedNewsCheckAtUtc)
            && validUtcInstant(follow.lastSuccessfulNewsCheckAtUtc)
            && follow.lastRefreshError.isEmpty();
    case SteamGameUpdateRefreshState::Failed:
        return validUtcInstant(follow.lastAttemptedNewsCheckAtUtc)
            && !follow.lastRefreshError.trimmed().isEmpty()
            && follow.lastRefreshError.size() <= 500
            && (follow.lastSuccessfulNewsCheckAtUtc.isEmpty()
                || validUtcInstant(follow.lastSuccessfulNewsCheckAtUtc));
    }
    return false;
}

} // namespace something::domain
