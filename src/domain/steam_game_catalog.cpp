#include "steam_game_catalog.h"

#include "steam_game.h"

#include <QRegularExpression>

namespace something::domain {
namespace {

bool mimeMatchesFilename(const SteamGameCatalogArtwork &artwork)
{
    return (artwork.cachedFilename.endsWith(QStringLiteral(".jpg"))
            && artwork.mimeType == QStringLiteral("image/jpeg"))
        || (artwork.cachedFilename.endsWith(QStringLiteral(".png"))
            && artwork.mimeType == QStringLiteral("image/png"))
        || (artwork.cachedFilename.endsWith(QStringLiteral(".webp"))
            && artwork.mimeType == QStringLiteral("image/webp"));
}

} // namespace

AppError normalizeSteamGameCatalogDetails(SteamGameCatalogDetails &details)
{
    const SteamStoreUrl store = normalizeSteamStoreUrl(details.canonicalStoreUrl);
    details.name = details.name.trimmed().normalized(QString::NormalizationForm_C);
    if (details.appId == 0 || !store.isValid() || store.appId != details.appId
        || details.name.isEmpty() || details.name.toUcs4().size() > 300) {
        return AppError(ErrorCategory::Validation);
    }
    details.canonicalStoreUrl = store.canonicalUrl;
    if (!details.artworkUrl.isEmpty()) {
        const QString normalizedArtwork = normalizeSteamArtworkUrl(details.artworkUrl);
        if (normalizedArtwork.isEmpty()) return AppError(ErrorCategory::Validation);
        details.artworkUrl = normalizedArtwork;
    } else {
        details.artworkUrl = QStringLiteral("");
    }
    return AppError::none();
}

bool isValidSteamGameCatalogDetails(const SteamGameCatalogDetails &details)
{
    SteamGameCatalogDetails normalized = details;
    if (normalizeSteamGameCatalogDetails(normalized).isError()) return false;
    return normalized.canonicalStoreUrl == details.canonicalStoreUrl
        && normalized.name == details.name && normalized.artworkUrl == details.artworkUrl;
}

bool isValidSteamGameCatalogArtwork(quint32 appId,
                                   const SteamGameCatalogArtwork &artwork)
{
    if (!artwork.sourceUrl.isEmpty()
        && normalizeSteamArtworkUrl(artwork.sourceUrl) != artwork.sourceUrl) {
        return false;
    }
    if (artwork.cachedFilename.isEmpty())
        return artwork.mimeType.isEmpty() && artwork.byteSize == 0;

    static const QRegularExpression filenamePattern(
        QStringLiteral("^[1-9][0-9]{0,9}-[a-f0-9]{64}\\.(jpg|png|webp)$"));
    return appId != 0 && !artwork.sourceUrl.isEmpty() && artwork.byteSize > 0
        && artwork.byteSize <= 2LL * 1024 * 1024
        && filenamePattern.match(artwork.cachedFilename).hasMatch()
        && artwork.cachedFilename.startsWith(QString::number(appId) + QLatin1Char('-'))
        && mimeMatchesFilename(artwork);
}

bool isValidSteamGameCatalogEntry(const SteamGameCatalogEntry &entry)
{
    if (entry.createdAt.trimmed().isEmpty() || entry.updatedAt.trimmed().isEmpty())
        return false;
    const SteamGameCatalogDetails details{
        entry.appId, entry.canonicalStoreUrl, entry.name, entry.artwork.sourceUrl};
    return isValidSteamGameCatalogDetails(details)
        && isValidSteamGameCatalogArtwork(entry.appId, entry.artwork);
}

} // namespace something::domain
