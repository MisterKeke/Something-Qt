#include "steam_service.h"

#include "../domain/steam_game.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QVariantList>

#include <utility>

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

bool validSteamRecord(const domain::SteamGameDetails &details)
{
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    if (details.appId == 0 || details.name.trimmed().isEmpty()
        || details.name.toUcs4().size() > 300 || details.discountPercent < 0
        || details.discountPercent > 100
        || (!details.artworkUrl.isEmpty()
            && domain::normalizeSteamArtworkUrl(details.artworkUrl) != details.artworkUrl))
        return false;
    if (details.priceState == domain::SteamPriceState::Priced) {
        return currencyPattern.match(details.currency.trimmed().toUpper()).hasMatch()
            && details.regularPriceMinor
            && details.currentPriceMinor && *details.regularPriceMinor >= 0
            && *details.currentPriceMinor >= 0;
    }
    if (details.priceState == domain::SteamPriceState::Free)
        return details.regularPriceMinor == std::optional<qint64>(0)
            && details.currentPriceMinor == std::optional<qint64>(0);
    return (details.priceState == domain::SteamPriceState::Unavailable
            || details.priceState == domain::SteamPriceState::Unsupported)
        && !details.regularPriceMinor && !details.currentPriceMinor;
}

} // namespace

QVariantList SteamService::countryValues()
{
    QVariantList values;
    const QVector<domain::SteamCountry> countries = domain::supportedSteamCountries();
    values.reserve(countries.size());
    for (const domain::SteamCountry &country : countries)
        values.push_back(QVariantMap{{QStringLiteral("code"), country.code},
                                     {QStringLiteral("name"), country.name},
                                     {QStringLiteral("label"),
                                      QStringLiteral("%1 (%2)").arg(country.name, country.code)}});
    return values;
}

QVariantMap SteamService::gameValue(const storage::SteamGame &game,
                                    const QString &profileDirectory)
{
    QVariantMap value{
        {QStringLiteral("id"), game.id},
        {QStringLiteral("steamAppId"), game.steamAppId},
        {QStringLiteral("storeUrl"), game.storeUrl},
        {QStringLiteral("name"), game.name},
        {QStringLiteral("imageSourceUrl"), game.imageSourceUrl},
        {QStringLiteral("priceStatus"), game.priceStatus},
        {QStringLiteral("currency"), game.currency},
        {QStringLiteral("discountPercent"), game.discountPercent},
        {QStringLiteral("priceCountryCode"), game.priceCountryCode},
        {QStringLiteral("lastCheckedAt"), game.lastCheckedAt},
        {QStringLiteral("lastAttemptedAt"), game.lastAttemptedAt},
        {QStringLiteral("lastRefreshError"), game.lastRefreshError},
        {QStringLiteral("createdAt"), game.createdAt},
        {QStringLiteral("updatedAt"), game.updatedAt},
        {QStringLiteral("artworkSource"), game.artwork
             ? SteamArtworkStore::imageSource(profileDirectory, game.artwork->filename)
             : QString()},
    };
    if (game.regularPriceMinor)
        value.insert(QStringLiteral("regularPriceMinor"), *game.regularPriceMinor);
    if (game.currentPriceMinor)
        value.insert(QStringLiteral("currentPriceMinor"), *game.currentPriceMinor);
    return value;
}

storage::StorageCommand SteamService::loadSnapshot(QString profileDirectory)
{
    return [profileDirectory = std::move(profileDirectory)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        SteamArtworkStore::removeAbandonedStagingFiles(profileDirectory);
        const auto settings = repositories.steam.settings();
        if (!settings.succeeded()) return failed(settings.error);
        const auto records = repositories.steam.list();
        if (!records.succeeded()) return failed(records.error);
        if (cancelled()) return failed(domain::AppError::cancelled());
        QVariantList games;
        games.reserve(records.value.size());
        for (const storage::SteamGame &game : records.value)
            games.push_back(gameValue(game, profileDirectory));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("games"), games);
        reply.values.insert(QStringLiteral("countryCode"), settings.value.countryCode);
        reply.values.insert(QStringLiteral("countries"), countryValues());
        return reply;
    };
}

storage::StorageCommand SteamService::loadPriceHistory(
    qint64 gameId, QString countryCode, QString currencyCode,
    storage::SteamPriceHistoryRange range)
{
    countryCode = countryCode.trimmed().toUpper();
    currencyCode = currencyCode.trimmed().toUpper();
    return [gameId, countryCode = std::move(countryCode),
            currencyCode = std::move(currencyCode), range](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
        if (gameId <= 0 || !domain::isSupportedSteamCountry(countryCode)
            || (!currencyCode.isEmpty() && !currencyPattern.match(currencyCode).hasMatch())) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }

        QString rangeName;
        switch (range) {
        case storage::SteamPriceHistoryRange::Days7:
            rangeName = QStringLiteral("7d");
            break;
        case storage::SteamPriceHistoryRange::Days30:
            rangeName = QStringLiteral("30d");
            break;
        case storage::SteamPriceHistoryRange::Days90:
            rangeName = QStringLiteral("90d");
            break;
        case storage::SteamPriceHistoryRange::Year1:
            rangeName = QStringLiteral("1y");
            break;
        case storage::SteamPriceHistoryRange::All:
            rangeName = QStringLiteral("all");
            break;
        default:
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }

        const auto loaded = repositories.steam.priceHistory(
            gameId, countryCode, currencyCode, range, cancelled);
        if (!loaded.succeeded()) return failed(loaded.error);
        if (cancelled()) return failed(domain::AppError::cancelled());

        const storage::SteamPriceHistoryStatistics &statistics = loaded.value.statistics;
        QVariantMap value{
            {QStringLiteral("gameId"), gameId},
            {QStringLiteral("countryCode"), statistics.countryCode},
            {QStringLiteral("currencyCode"), statistics.currencyCode},
            {QStringLiteral("range"), rangeName},
            {QStringLiteral("observationCount"), statistics.observationCount},
            {QStringLiteral("comparablePriceCount"), statistics.comparablePriceCount},
            {QStringLiteral("hasSufficientHistory"), statistics.hasSufficientHistory},
            {QStringLiteral("firstObservationAtUtc"), statistics.firstObservationAtUtc},
            {QStringLiteral("mostRecentObservationAtUtc"), statistics.mostRecentObservationAtUtc},
            {QStringLiteral("latestObservationState"), statistics.latestObservationState},
            {QStringLiteral("latestObservationAtUtc"), statistics.latestObservationAtUtc},
            {QStringLiteral("latestPriceState"), statistics.latestPriceState},
            {QStringLiteral("latestPriceAtUtc"), statistics.latestPriceAtUtc},
            {QStringLiteral("hasLatestPrice"), statistics.latestPriceMinor.has_value()},
            {QStringLiteral("hasLowestPrice"), statistics.lowestPriceMinor.has_value()},
            {QStringLiteral("hasHighestPrice"), statistics.highestPriceMinor.has_value()},
            {QStringLiteral("hasComparableInterval"), statistics.hasComparableInterval},
            {QStringLiteral("intervalStartAtUtc"), statistics.intervalStartAtUtc},
            {QStringLiteral("intervalEndAtUtc"), statistics.intervalEndAtUtc},
            {QStringLiteral("hasIntervalPercentageChange"),
             statistics.intervalPercentageChange.has_value()},
        };
        const auto formattedPrice = [](qint64 minor, const QString &state,
                                       const QString &currency) {
            if (state == QStringLiteral("free") || minor == 0)
                return QStringLiteral("Free");
            return domain::formatSteamPrice(minor, currency);
        };
        if (statistics.latestPriceMinor) {
            value.insert(QStringLiteral("latestPriceMinor"), *statistics.latestPriceMinor);
            value.insert(QStringLiteral("latestPriceDisplay"),
                         formattedPrice(*statistics.latestPriceMinor,
                                        statistics.latestPriceState,
                                        statistics.currencyCode));
        }
        if (statistics.lowestPriceMinor) {
            value.insert(QStringLiteral("lowestPriceMinor"), *statistics.lowestPriceMinor);
            value.insert(QStringLiteral("lowestPriceDisplay"),
                         formattedPrice(*statistics.lowestPriceMinor,
                                        QStringLiteral("priced"),
                                        statistics.currencyCode));
        }
        if (statistics.highestPriceMinor) {
            value.insert(QStringLiteral("highestPriceMinor"), *statistics.highestPriceMinor);
            value.insert(QStringLiteral("highestPriceDisplay"),
                         formattedPrice(*statistics.highestPriceMinor,
                                        QStringLiteral("priced"),
                                        statistics.currencyCode));
        }
        if (statistics.intervalStartPriceMinor)
            value.insert(QStringLiteral("intervalStartPriceMinor"),
                          *statistics.intervalStartPriceMinor);
        if (statistics.intervalEndPriceMinor)
            value.insert(QStringLiteral("intervalEndPriceMinor"),
                          *statistics.intervalEndPriceMinor);
        if (statistics.intervalAbsoluteChangeMinor)
            value.insert(QStringLiteral("intervalAbsoluteChangeMinor"),
                          *statistics.intervalAbsoluteChangeMinor);
        if (statistics.intervalAbsoluteChangeMinor) {
            const qint64 change = *statistics.intervalAbsoluteChangeMinor;
            const QString magnitude = statistics.currencyCode.isEmpty()
                ? QString::number(qAbs(change))
                : domain::formatSteamPrice(qAbs(change), statistics.currencyCode);
            value.insert(QStringLiteral("intervalAbsoluteChangeDisplay"),
                         (change > 0 ? QStringLiteral("+")
                          : change < 0 ? QStringLiteral("−") : QString()) + magnitude);
        }
        if (statistics.intervalPercentageChange)
            value.insert(QStringLiteral("intervalPercentageChange"),
                          *statistics.intervalPercentageChange);
        if (statistics.intervalPercentageChange) {
            const double change = *statistics.intervalPercentageChange;
            value.insert(QStringLiteral("intervalPercentageChangeDisplay"),
                         QStringLiteral("%1%2%")
                             .arg(change > 0 ? QStringLiteral("+")
                                  : change < 0 ? QStringLiteral("−") : QString(),
                                  QString::number(qAbs(change), 'f', 2)));
        }

        QVariantList points;
        points.reserve(loaded.value.points.size());
        for (const storage::SteamPriceHistoryPoint &point : loaded.value.points) {
            QVariantMap pointValue{
                {QStringLiteral("id"), point.id},
                {QStringLiteral("observedAtUtc"), point.observedAtUtc},
                {QStringLiteral("observedAtUtcMs"), point.observedAtUtcMs},
                {QStringLiteral("priceState"), point.priceState},
                {QStringLiteral("currencyCode"), point.currencyCode},
                {QStringLiteral("hasPrice"), point.priceMinor.has_value()},
            };
            if (point.priceMinor) {
                pointValue.insert(QStringLiteral("priceMinor"), *point.priceMinor);
                pointValue.insert(QStringLiteral("priceDisplay"),
                    formattedPrice(*point.priceMinor, point.priceState,
                                   point.currencyCode));
            }
            points.push_back(std::move(pointValue));
        }

        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("statistics"), value);
        reply.values.insert(QStringLiteral("points"), points);
        return reply;
    };
}

storage::StorageCommand SteamService::setCountry(QString countryCode)
{
    countryCode = countryCode.trimmed().toUpper();
    return [countryCode = std::move(countryCode)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (!domain::isSupportedSteamCountry(countryCode))
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto saved = repositories.steam.setCountry(countryCode);
        if (!saved.succeeded()) return failed(saved.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("countryCode"), countryCode);
        return reply;
    };
}

storage::StorageCommand SteamService::addGame(QString profileDirectory,
                                              QString canonicalUrl,
                                              QString countryCode,
                                              domain::SteamGameDetails details,
                                              QString checkedAt)
{
    countryCode = countryCode.trimmed().toUpper();
    return [profileDirectory = std::move(profileDirectory),
            canonicalUrl = std::move(canonicalUrl), countryCode = std::move(countryCode),
            details = std::move(details), checkedAt = std::move(checkedAt)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const domain::SteamStoreUrl url = domain::normalizeSteamStoreUrl(canonicalUrl);
        if (!url.isValid() || url.appId != details.appId
            || !domain::isSupportedSteamCountry(countryCode) || !validSteamRecord(details)
            || checkedAt.isEmpty())
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        storage::SteamGame game;
        game.steamAppId = details.appId;
        game.storeUrl = url.canonicalUrl;
        game.name = details.name.trimmed();
        game.imageSourceUrl = details.artworkUrl;
        game.priceStatus = domain::steamPriceStateName(details.priceState);
        game.currency = details.currency.trimmed().toUpper();
        game.regularPriceMinor = details.regularPriceMinor;
        game.currentPriceMinor = details.currentPriceMinor;
        game.discountPercent = details.discountPercent;
        game.priceCountryCode = countryCode;
        game.lastCheckedAt = checkedAt;
        game.lastAttemptedAt = checkedAt;
        const auto inserted = repositories.steam.add(std::move(game), cancelled);
        if (!inserted.succeeded()) return failed(inserted.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("game"), gameValue(inserted.value, profileDirectory));
        return reply;
    };
}

storage::StorageCommand SteamService::applyRefresh(
    QString profileDirectory, QVector<storage::SteamRefreshUpdate> updates)
{
    return [profileDirectory = std::move(profileDirectory), updates = std::move(updates)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        const auto applied = repositories.steam.applyRefreshBatch(updates, cancelled);
        if (!applied.succeeded()) return failed(applied.error);
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto records = repositories.steam.list();
        if (!records.succeeded()) return failed(records.error);
        QVariantList games;
        games.reserve(records.value.size());
        for (const storage::SteamGame &game : records.value)
            games.push_back(gameValue(game, profileDirectory));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("games"), games);
        return reply;
    };
}

storage::StorageCommand SteamService::deleteGame(QString profileDirectory, qint64 gameId)
{
    return [profileDirectory = std::move(profileDirectory), gameId](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto existing = repositories.steam.get(gameId);
        if (!existing.succeeded()) return failed(existing.error);
        const auto removed = repositories.steam.remove(gameId);
        if (!removed.succeeded()) return failed(removed.error);
        if (existing.value.artwork) {
            QString originalPath;
            QString ignoredStage;
            if (SteamArtworkStore::stageDeletion(profileDirectory,
                    existing.value.artwork->filename, &ignoredStage, &originalPath)) {
                if (!ignoredStage.isEmpty()) SteamArtworkStore::finishDeletion(ignoredStage);
            }
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("deletedId"), gameId);
        return reply;
    };
}

storage::StorageCommand SteamService::saveArtwork(QString profileDirectory, qint64 gameId,
                                                  StagedSteamArtwork artwork)
{
    return [profileDirectory = std::move(profileDirectory), gameId,
            artwork = std::move(artwork)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(domain::AppError::cancelled());
        }
        const auto existing = repositories.steam.get(gameId);
        if (!existing.succeeded()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(existing.error);
        }
        bool installedNewFile = false;
        if (!SteamArtworkStore::install(profileDirectory, artwork, &installedNewFile)) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            return failed(domain::AppError(domain::ErrorCategory::Storage));
        }
        if (cancelled()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            SteamArtworkStore::removeInstalledIfNew(profileDirectory, artwork, installedNewFile);
            return failed(domain::AppError::cancelled());
        }
        const auto stored = repositories.steam.setArtwork(gameId, artwork.asset);
        if (!stored.succeeded()) {
            SteamArtworkStore::discard(profileDirectory, artwork);
            SteamArtworkStore::removeInstalledIfNew(profileDirectory, artwork, installedNewFile);
            return failed(stored.error);
        }
        SteamArtworkStore::discard(profileDirectory, artwork);
        if (existing.value.artwork
            && existing.value.artwork->filename != artwork.asset.filename) {
            QString originalPath;
            QString stagedPath;
            if (SteamArtworkStore::stageDeletion(profileDirectory,
                    existing.value.artwork->filename, &stagedPath, &originalPath)
                && !stagedPath.isEmpty()) SteamArtworkStore::finishDeletion(stagedPath);
        }
        const auto refreshed = repositories.steam.get(gameId);
        if (!refreshed.succeeded()) return failed(refreshed.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("game"), gameValue(refreshed.value, profileDirectory));
        return reply;
    };
}

} // namespace something::services
