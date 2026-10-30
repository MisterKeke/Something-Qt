#include "steam_game_updates.h"

#include <QDateTime>
#include <QRegularExpression>

namespace something::domain {
namespace {

bool validUtcInstant(const QString &value)
{
    if (value.isEmpty() || !value.endsWith(QLatin1Char('Z'))) return false;
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    return parsed.isValid() && parsed.timeSpec() == Qt::UTC;
}

bool validArtwork(quint32 appId, const SteamGameUpdateArtworkMetadata &artwork)
{
    if (!artwork.sourceUrl.isEmpty()
        && normalizeSteamArtworkUrl(artwork.sourceUrl) != artwork.sourceUrl) {
        return false;
    }
    if (artwork.cachedFilename.isEmpty())
        return artwork.mimeType.isEmpty() && artwork.byteSize == 0;
    if (appId == 0 || artwork.sourceUrl.isEmpty() || artwork.byteSize <= 0
        || artwork.byteSize > 2LL * 1024 * 1024) {
        return false;
    }

    static const QRegularExpression filenamePattern(
        QStringLiteral("^[1-9][0-9]{0,9}-[a-f0-9]{64}\\.(jpg|png|webp)$"));
    const bool mimeMatchesExtension =
        (artwork.cachedFilename.endsWith(QStringLiteral(".jpg"))
         && artwork.mimeType == QStringLiteral("image/jpeg"))
        || (artwork.cachedFilename.endsWith(QStringLiteral(".png"))
            && artwork.mimeType == QStringLiteral("image/png"))
        || (artwork.cachedFilename.endsWith(QStringLiteral(".webp"))
            && artwork.mimeType == QStringLiteral("image/webp"));
    return mimeMatchesExtension && filenamePattern.match(artwork.cachedFilename).hasMatch()
        && artwork.cachedFilename.startsWith(QString::number(appId) + QLatin1Char('-'));
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
        || !validArtwork(follow.appId, follow.artwork) || !validUtcInstant(follow.addedAtUtc)) {
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
