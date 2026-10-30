#include "domain/steam_game.h"
#include "models/steam_games_model.h"
#include "providers/feature_parsers.h"
#include "providers/provider_policy.h"
#include "providers/steam_artwork_image_provider.h"
#include "services/steam_artwork_store.h"
#include "services/steam_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <cmath>
#include <functional>
#include <utility>

using namespace something;

namespace {

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(":/provider-fixtures/") + name);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool openDatabase(QSqlDatabase *database, QString *connectionName)
{
    *connectionName = QStringLiteral("steam-feature-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    database->setDatabaseName(QStringLiteral(":memory:"));
    return database->open() && storage::SchemaMigrator::apply(*database);
}

storage::SteamGame priceGame(quint32 appId, QString name, QString countryCode,
                             QString currencyCode, QString state,
                             bool hasPrice, qint64 regularPriceMinor,
                             qint64 currentPriceMinor, int discountPercent,
                             QString checkedAt)
{
    storage::SteamGame game;
    game.steamAppId = appId;
    game.storeUrl = QStringLiteral("https://store.steampowered.com/app/%1/").arg(appId);
    game.name = std::move(name);
    game.priceCountryCode = std::move(countryCode);
    game.currency = std::move(currencyCode);
    game.priceStatus = std::move(state);
    if (hasPrice) {
        game.regularPriceMinor = regularPriceMinor;
        game.currentPriceMinor = currentPriceMinor;
    }
    game.discountPercent = discountPercent;
    game.lastCheckedAt = std::move(checkedAt);
    game.lastAttemptedAt = game.lastCheckedAt;
    return game;
}

storage::RepositoryResult<storage::SteamGame> addPriceGame(
    storage::SteamRepository &repository, quint32 appId, QString name,
    QString countryCode, QString currencyCode, QString state,
    bool hasPrice, qint64 regularPriceMinor, qint64 currentPriceMinor,
    int discountPercent, QString checkedAt)
{
    return repository.add(priceGame(appId, std::move(name), std::move(countryCode),
        std::move(currencyCode), std::move(state), hasPrice, regularPriceMinor,
        currentPriceMinor, discountPercent, std::move(checkedAt)));
}

storage::RepositoryResult<void> refreshPriceGame(
    storage::SteamRepository &repository, storage::SteamGame game,
    QString attemptedAt, const std::function<bool()> &cancellationRequested = {})
{
    storage::SteamRefreshUpdate update;
    update.gameId = game.id;
    update.attemptedAt = std::move(attemptedAt);
    update.refreshedGame = std::move(game);
    return repository.applyRefreshBatch({update}, cancellationRequested);
}

} // namespace

class SteamFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void storeUrlAndCountryRulesAreStrictAndCanonical();
    void providerFixturesDistinguishPaidFreeUnavailableAndInvalid();
    void steamArtworkPolicyRejectsUnapprovedHosts();
    void priceFormattingUsesMinorUnitsAndCurrencyPrecision();
    void serviceAndRepositoryPreserveCachedPricesOnPerGameFailure();
    void steamPriceHistoryMigrationPreservesLegacyGamesWithoutFabricatingHistory();
    void steamPriceHistoryOrdersEqualTimestampsAndFiltersRanges();
    void steamPriceHistoryCoversRecordingMarketsCalculationsFailuresCancellationAndCascade();
    void artworkStoreStagesBoundedContentAndImageProviderLoadsById();
    void modelExposesMissingFreeAndPaidStatesWithoutZeroFallback();
};

void SteamFeatureTest::storeUrlAndCountryRulesAreStrictAndCanonical()
{
    const auto valid = domain::normalizeSteamStoreUrl(
        QStringLiteral("  http://STORE.SteamPowered.com/app/12345/?ignored=1  "));
    QVERIFY(valid.isValid());
    QCOMPARE(valid.appId, quint32(12345));
    QCOMPARE(valid.canonicalUrl,
             QStringLiteral("https://store.steampowered.com/app/12345/"));
    QVERIFY(domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com/APP/12345/extra")).isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com.evil.test/app/12345/")).isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("https://name@store.steampowered.com/app/12345/")).isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com/app/0/")).isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com/app/4294967296/")).isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com/agecheck/app/12345")).isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com/app/12345/")
        + QString(4100, QLatin1Char('x'))).isValid());

    QVERIFY(domain::isSupportedSteamCountry(QStringLiteral(" tr ")));
    QVERIFY(domain::isSupportedSteamCountry(QStringLiteral("US")));
    QVERIFY(!domain::isSupportedSteamCountry(QStringLiteral("XX")));
    QCOMPARE(domain::supportedSteamCountries().size(), 17);
}

void SteamFeatureTest::providerFixturesDistinguishPaidFreeUnavailableAndInvalid()
{
    const auto paid = providers::parseSteamGameDetails(
        fixture(QStringLiteral("steam-game-priced.json")), 12345);
    QVERIFY(paid.succeeded());
    QCOMPARE(paid.value.name, QStringLiteral("Fixture Game"));
    QCOMPARE(paid.value.priceState, domain::SteamPriceState::Priced);
    QCOMPARE(paid.value.currency, QStringLiteral("USD"));
    QCOMPARE(paid.value.regularPriceMinor.value(), qint64(1999));
    QCOMPARE(paid.value.currentPriceMinor.value(), qint64(999));
    QCOMPARE(paid.value.discountPercent, 50);
    QVERIFY(paid.value.artworkUrl.startsWith(QStringLiteral("https://")));

    const auto discountMissing = providers::parseSteamGameDetails(
        fixture(QStringLiteral("steam-game-discount-missing.json")), 12345);
    QVERIFY(discountMissing.succeeded());
    QCOMPARE(discountMissing.value.discountPercent, 0);
    QCOMPARE(domain::effectiveSteamDiscount(
        *discountMissing.value.regularPriceMinor,
        *discountMissing.value.currentPriceMinor,
        discountMissing.value.discountPercent), 25);

    const auto free = providers::parseSteamGameDetails(
        fixture(QStringLiteral("steam-game-free.json")), 12345);
    QVERIFY(free.succeeded());
    QCOMPARE(free.value.priceState, domain::SteamPriceState::Free);
    QCOMPARE(free.value.currentPriceMinor.value(), qint64(0));

    const auto unavailable = providers::parseSteamGameDetails(
        fixture(QStringLiteral("steam-game-unavailable.json")), 12345);
    QVERIFY(unavailable.succeeded());
    QCOMPARE(unavailable.value.priceState, domain::SteamPriceState::Unavailable);
    QVERIFY(!unavailable.value.currentPriceMinor.has_value());
    QVERIFY(providers::parseSteamGameDetails(
        fixture(QStringLiteral("steam-game-mismatch.json")), 12345).error.isError());
    QVERIFY(providers::parseSteamGameDetails(
        fixture(QStringLiteral("steam-game-malformed.json")), 12345).error.isError());
    QVERIFY(providers::parseSteamGameDetails(QByteArrayLiteral("{"), 12345).error.isError());
    QVERIFY(providers::parseSteamGameDetails(QByteArray(2 * 1024 * 1024 + 1, ' '),
                                             12345).error.isError());
}

void SteamFeatureTest::steamArtworkPolicyRejectsUnapprovedHosts()
{
    const auto provider = providers::ProviderId::SteamArtwork;
    QCOMPARE(providers::providerPolicy(provider).maximumConcurrentRequests, 4);
    QCOMPARE(providers::providerPolicy(provider).maximumResponseBytes,
             services::SteamArtworkStore::maximumArtworkBytes);
    QVERIFY(providers::isAllowedProviderUrl(provider,
        QUrl(QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/1/header.jpg"))));
    QVERIFY(providers::isAllowedProviderUrl(provider,
        QUrl(QStringLiteral("https://steamcdn-a.akamaihd.net/steam/apps/1/header.jpg"))));
    QVERIFY(!providers::isAllowedProviderUrl(provider,
        QUrl(QStringLiteral("https://steamstatic.com.evil.test/image.jpg"))));
    QVERIFY(!providers::isAllowedProviderUrl(provider,
        QUrl(QStringLiteral("http://cdn.akamai.steamstatic.com/steam/apps/1/header.jpg"))));
    QVERIFY(domain::normalizeSteamArtworkUrl(
        QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/123/header.jpg")).size() > 0);
    QVERIFY(domain::normalizeSteamArtworkUrl(
        QStringLiteral("https://evil.test/steam/apps/123/header.jpg")).isEmpty());
}

void SteamFeatureTest::priceFormattingUsesMinorUnitsAndCurrencyPrecision()
{
    const QLocale locale(QLocale::English, QLocale::UnitedStates);
    QCOMPARE(domain::formatSteamPrice(1999, QStringLiteral("USD"), locale),
             QStringLiteral("19.99 USD"));
    QCOMPARE(domain::formatSteamPrice(1234, QStringLiteral("JPY"), locale),
             QStringLiteral("1,234 JPY"));
    QCOMPARE(domain::formatSteamPrice(12345, QStringLiteral("KWD"), locale),
             QStringLiteral("12.345 KWD"));
    QCOMPARE(domain::effectiveSteamDiscount(1999, 999, 0), 50);
    QCOMPARE(domain::effectiveSteamDiscount(100, 100, 25), 0);
    QVERIFY(domain::formatSteamPrice(-1, QStringLiteral("USD"), locale).isEmpty());
    QVERIFY(domain::formatSteamPrice(100, QStringLiteral("US"), locale).isEmpty());
}

void SteamFeatureTest::serviceAndRepositoryPreserveCachedPricesOnPerGameFailure()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        const auto run = [&repositories](storage::StorageCommand command) {
            return command(repositories, [] { return false; });
        };
        auto snapshot = run(services::SteamService::loadSnapshot(profile.path()));
        QVERIFY(!snapshot.error.isError());
        QCOMPARE(snapshot.values.value(QStringLiteral("countryCode")).toString(),
                 QStringLiteral("TR"));
        QCOMPARE(snapshot.values.value(QStringLiteral("games")).toList().size(), 0);
        QVERIFY(!run(services::SteamService::setCountry(QStringLiteral("US"))).error.isError());
        QVERIFY(run(services::SteamService::setCountry(QStringLiteral("XX"))).error.isError());

        const auto parsed = providers::parseSteamGameDetails(
            fixture(QStringLiteral("steam-game-priced.json")), 12345);
        QVERIFY(parsed.succeeded());
        const auto firstReply = run(services::SteamService::addGame(
            profile.path(), QStringLiteral("https://store.steampowered.com/app/12345/"),
            QStringLiteral("US"), parsed.value, QStringLiteral("2026-10-19T10:00:00Z")));
        QVERIFY(!firstReply.error.isError());
        const QVariantMap first = firstReply.values.value(QStringLiteral("game")).toMap();
        const qint64 firstId = first.value(QStringLiteral("id")).toLongLong();
        QCOMPARE(first.value(QStringLiteral("currentPriceMinor")).toLongLong(), qint64(999));

        const auto duplicate = run(services::SteamService::addGame(
            profile.path(), QStringLiteral("https://store.steampowered.com/app/12345/"),
            QStringLiteral("US"), parsed.value, QStringLiteral("2026-10-19T10:01:00Z")));
        QCOMPARE(duplicate.error.category(), domain::ErrorCategory::Conflict);

        domain::SteamGameDetails secondDetails = parsed.value;
        secondDetails.appId = 67890;
        secondDetails.name = QStringLiteral("Second Fixture Game");
        const auto secondReply = run(services::SteamService::addGame(
            profile.path(), QStringLiteral("https://store.steampowered.com/app/67890/"),
            QStringLiteral("US"), secondDetails, QStringLiteral("2026-10-19T10:00:00Z")));
        QVERIFY(!secondReply.error.isError());
        const qint64 secondId = secondReply.values.value(QStringLiteral("game")).toMap()
            .value(QStringLiteral("id")).toLongLong();

        auto existing = repositories.steam.get(firstId);
        QVERIFY(existing.succeeded());
        storage::SteamGame successful = existing.value;
        successful.priceStatus = QStringLiteral("free");
        successful.currency.clear();
        successful.regularPriceMinor = 0;
        successful.currentPriceMinor = 0;
        successful.discountPercent = 0;
        successful.priceCountryCode = QStringLiteral("US");
        storage::SteamRefreshUpdate updated;
        updated.gameId = firstId;
        updated.attemptedAt = QStringLiteral("2026-10-19T11:00:00Z");
        updated.refreshedGame = successful;
        storage::SteamRefreshUpdate failed;
        failed.gameId = secondId;
        failed.attemptedAt = QStringLiteral("2026-10-19T11:00:01Z");
        failed.safeError = QStringLiteral("Steam could not provide this game's details right now.");
        QVERIFY(repositories.steam.applyRefreshBatch({updated, failed}).succeeded());

        const auto cachedAfterFailure = repositories.steam.get(secondId);
        QVERIFY(cachedAfterFailure.succeeded());
        QCOMPARE(cachedAfterFailure.value.currentPriceMinor.value(), qint64(999));
        QCOMPARE(cachedAfterFailure.value.lastRefreshError, failed.safeError);
        QCOMPARE(cachedAfterFailure.value.lastAttemptedAt, failed.attemptedAt);
        QCOMPARE(repositories.steam.get(firstId).value.priceStatus, QStringLiteral("free"));

        snapshot = run(services::SteamService::loadSnapshot(profile.path()));
        QVERIFY(!snapshot.error.isError());
        QCOMPARE(snapshot.values.value(QStringLiteral("games")).toList().size(), 2);
        QCOMPARE(snapshot.values.value(QStringLiteral("countryCode")).toString(),
                 QStringLiteral("US"));
        QVERIFY(!run(services::SteamService::deleteGame(profile.path(), firstId)).error.isError());
        QVERIFY(!repositories.steam.get(firstId).succeeded());
        QVERIFY(repositories.steam.get(secondId).succeeded());
    }
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SteamFeatureTest::steamPriceHistoryMigrationPreservesLegacyGamesWithoutFabricatingHistory()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));

    QSqlQuery legacy(database);
    QVERIFY(legacy.exec(QStringLiteral(
        "INSERT INTO steam_games(id,steam_app_id,store_url,name,price_status,currency,"
        "regular_price_minor,current_price_minor,discount_percent,price_country_code,"
        "last_checked_at,last_attempted_at) VALUES(51,555001,"
        "'https://store.steampowered.com/app/555001/','Legacy tracked game','priced','USD',"
        "1999,999,50,'US','2025-03-04T12:00:00Z','2025-03-04T12:00:00Z')")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_price_observations")));
    QVERIFY(legacy.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version=12")));
    legacy.finish();

    QString diagnostic;
    QVERIFY2(storage::SchemaMigrator::apply(database, &diagnostic), qPrintable(diagnostic));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 12);

    {
        storage::StorageRepositories repositories(database);
        const auto preserved = repositories.steam.get(51);
        QVERIFY(preserved.succeeded());
        QCOMPARE(preserved.value.name, QStringLiteral("Legacy tracked game"));
        QCOMPARE(preserved.value.currentPriceMinor.value(), qint64(999));

        const auto history = repositories.steam.priceHistory(
            51, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(history.succeeded());
        QCOMPARE(history.value.statistics.observationCount, qint64(0));
        QVERIFY(history.value.points.isEmpty());
    }

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SteamFeatureTest::steamPriceHistoryOrdersEqualTimestampsAndFiltersRanges()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        const auto timestampDaysAgo = [&nowUtc](int days) {
            return nowUtc.addDays(-days).toString(Qt::ISODateWithMs);
        };
        const QString oldestAt = timestampDaysAgo(40);
        const QString repeatedAt = timestampDaysAgo(6);
        const QString newestAt = timestampDaysAgo(2);
        const auto added = addPriceGame(repositories.steam, 32123,
            QStringLiteral("Range and order fixture"), QStringLiteral("US"),
            QStringLiteral("USD"), QStringLiteral("priced"), true, 5000, 4000, 20,
            oldestAt);
        QVERIFY(added.succeeded());

        auto current = repositories.steam.get(added.value.id);
        QVERIFY(current.succeeded());
        current.value.currentPriceMinor = 3000;
        current.value.discountPercent = 40;
        QVERIFY(refreshPriceGame(repositories.steam, current.value, repeatedAt).succeeded());

        current = repositories.steam.get(added.value.id);
        QVERIFY(current.succeeded());
        current.value.currentPriceMinor = 2500;
        current.value.discountPercent = 50;
        QVERIFY(refreshPriceGame(repositories.steam, current.value, repeatedAt).succeeded());

        current = repositories.steam.get(added.value.id);
        QVERIFY(current.succeeded());
        current.value.currentPriceMinor = 2000;
        current.value.discountPercent = 60;
        QVERIFY(refreshPriceGame(repositories.steam, current.value, newestAt).succeeded());

        const auto allHistory = repositories.steam.priceHistory(
            added.value.id, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(allHistory.succeeded());
        QCOMPARE(allHistory.value.points.size(), 4);
        QVERIFY(allHistory.value.points.at(0).observedAtUtc
                < allHistory.value.points.at(1).observedAtUtc);
        QCOMPARE(allHistory.value.points.at(1).observedAtUtc,
                 allHistory.value.points.at(2).observedAtUtc);
        QVERIFY(allHistory.value.points.at(1).id < allHistory.value.points.at(2).id);
        QVERIFY(allHistory.value.points.at(2).observedAtUtc
                < allHistory.value.points.at(3).observedAtUtc);

        const auto week = repositories.steam.priceHistory(
            added.value.id, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::Days7);
        const auto month = repositories.steam.priceHistory(
            added.value.id, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::Days30);
        const auto quarter = repositories.steam.priceHistory(
            added.value.id, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::Days90);
        const auto year = repositories.steam.priceHistory(
            added.value.id, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::Year1);
        QVERIFY(week.succeeded());
        QVERIFY(month.succeeded());
        QVERIFY(quarter.succeeded());
        QVERIFY(year.succeeded());
        QCOMPARE(week.value.points.size(), 3);
        QCOMPARE(month.value.points.size(), 3);
        QCOMPARE(quarter.value.points.size(), 4);
        QCOMPARE(year.value.points.size(), 4);
        QCOMPARE(week.value.statistics.intervalStartPriceMinor.value(), qint64(3000));
        QCOMPARE(week.value.statistics.intervalEndPriceMinor.value(), qint64(2000));
        QCOMPARE(week.value.statistics.intervalAbsoluteChangeMinor.value(), qint64(-1000));
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SteamFeatureTest::steamPriceHistoryCoversRecordingMarketsCalculationsFailuresCancellationAndCascade()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        const auto run = [&repositories](storage::StorageCommand command) {
            return command(repositories, [] { return false; });
        };

        QVERIFY(!run(services::SteamService::setCountry(QStringLiteral("US")))
                     .error.isError());
        const auto added = addPriceGame(repositories.steam, 12345,
            QStringLiteral("History Fixture"), QStringLiteral("US"), QStringLiteral("USD"),
            QStringLiteral("priced"), true, 1999, 999, 50,
            QStringLiteral("2026-10-01T10:00:00Z"));
        QVERIFY(added.succeeded());
        const qint64 gameId = added.value.id;

        auto current = repositories.steam.get(gameId);
        QVERIFY(current.succeeded());
        QVERIFY(refreshPriceGame(repositories.steam, current.value,
            QStringLiteral("2026-10-01T12:00:00Z")).succeeded());

        current = repositories.steam.get(gameId);
        QVERIFY(current.succeeded());
        storage::SteamGame changedPrice = current.value;
        changedPrice.currentPriceMinor = 799;
        changedPrice.discountPercent = 60;
        QVERIFY(refreshPriceGame(repositories.steam, changedPrice,
            QStringLiteral("2026-10-01T14:00:00Z")).succeeded());

        current = repositories.steam.get(gameId);
        QVERIFY(current.succeeded());
        QVERIFY(refreshPriceGame(repositories.steam, current.value,
            QStringLiteral("2026-10-01T15:00:00Z")).succeeded());
        current = repositories.steam.get(gameId);
        QVERIFY(current.succeeded());
        QVERIFY(refreshPriceGame(repositories.steam, current.value,
            QStringLiteral("2026-10-02T10:00:00Z")).succeeded());

        auto usHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(usHistory.succeeded());
        QCOMPARE(usHistory.value.statistics.observationCount, qint64(3));
        QCOMPARE(usHistory.value.statistics.comparablePriceCount, qint64(3));
        QCOMPARE(usHistory.value.statistics.lowestPriceMinor.value(), qint64(799));
        QCOMPARE(usHistory.value.statistics.highestPriceMinor.value(), qint64(999));
        QCOMPARE(usHistory.value.statistics.latestPriceMinor.value(), qint64(799));
        QCOMPARE(usHistory.value.statistics.latestObservationState, QStringLiteral("priced"));
        QVERIFY(usHistory.value.statistics.firstObservationAtUtc.startsWith(
            QStringLiteral("2026-10-01T10:00:00")));
        QVERIFY(usHistory.value.statistics.mostRecentObservationAtUtc.startsWith(
            QStringLiteral("2026-10-02T10:00:00")));
        QCOMPARE(usHistory.value.statistics.intervalAbsoluteChangeMinor.value(), qint64(-200));
        QVERIFY(usHistory.value.statistics.intervalPercentageChange.has_value());
        QVERIFY(std::abs(*usHistory.value.statistics.intervalPercentageChange
                         + 20.02002002) < 0.001);
        QVERIFY(usHistory.value.statistics.hasSufficientHistory);
        QCOMPARE(usHistory.value.points.size(), 3);
        QCOMPARE(usHistory.value.points.at(0).priceMinor.value(), qint64(999));
        QCOMPARE(usHistory.value.points.at(1).priceMinor.value(), qint64(799));
        QCOMPARE(usHistory.value.points.at(2).priceMinor.value(), qint64(799));
        QVERIFY(usHistory.value.points.at(0).observedAtUtc
                < usHistory.value.points.at(1).observedAtUtc);
        QVERIFY(usHistory.value.points.at(1).observedAtUtc
                < usHistory.value.points.at(2).observedAtUtc);

        QVERIFY(!run(services::SteamService::setCountry(QStringLiteral("TR")))
                     .error.isError());
        current = repositories.steam.get(gameId);
        QVERIFY(current.succeeded());
        storage::SteamGame turkishMarket = current.value;
        turkishMarket.priceCountryCode = QStringLiteral("TR");
        turkishMarket.currency = QStringLiteral("TRY");
        turkishMarket.regularPriceMinor = 30000;
        turkishMarket.currentPriceMinor = 24000;
        turkishMarket.discountPercent = 20;
        QVERIFY(refreshPriceGame(repositories.steam, turkishMarket,
            QStringLiteral("2026-10-03T10:00:00Z")).succeeded());

        current = repositories.steam.get(gameId);
        QVERIFY(current.succeeded());
        storage::SteamGame alternateCurrency = current.value;
        alternateCurrency.currency = QStringLiteral("EUR");
        alternateCurrency.regularPriceMinor = 2499;
        alternateCurrency.currentPriceMinor = 1999;
        QVERIFY(refreshPriceGame(repositories.steam, alternateCurrency,
            QStringLiteral("2026-10-04T10:00:00Z")).succeeded());

        QCOMPARE(repositories.steam.settings().value.countryCode, QStringLiteral("TR"));
        usHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(usHistory.succeeded());
        QCOMPARE(usHistory.value.statistics.observationCount, qint64(3));
        const auto tryHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("TR"), QStringLiteral("TRY"),
            storage::SteamPriceHistoryRange::All);
        const auto euroHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("TR"), QStringLiteral("EUR"),
            storage::SteamPriceHistoryRange::All);
        const auto mixedCountryHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("US"), QStringLiteral("TRY"),
            storage::SteamPriceHistoryRange::All);
        const auto mixedCurrencyHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("TR"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(tryHistory.succeeded());
        QVERIFY(euroHistory.succeeded());
        QVERIFY(mixedCountryHistory.succeeded());
        QVERIFY(mixedCurrencyHistory.succeeded());
        QCOMPARE(tryHistory.value.statistics.observationCount, qint64(1));
        QCOMPARE(euroHistory.value.statistics.observationCount, qint64(1));
        QCOMPARE(mixedCountryHistory.value.statistics.observationCount, qint64(0));
        QCOMPARE(mixedCurrencyHistory.value.statistics.observationCount, qint64(0));

        const auto markets = repositories.steam.priceHistoryMarkets(gameId);
        QVERIFY(markets.succeeded());
        QCOMPARE(markets.value.size(), 3);
        const auto hasMarket = [](const auto &available, const QString &country,
                                  const QString &currency) {
            for (const auto &market : available) {
                if (market.countryCode == country && market.currencyCode == currency)
                    return true;
            }
            return false;
        };
        QVERIFY(hasMarket(markets.value, QStringLiteral("US"), QStringLiteral("USD")));
        QVERIFY(hasMarket(markets.value, QStringLiteral("TR"), QStringLiteral("TRY")));
        QVERIFY(hasMarket(markets.value, QStringLiteral("TR"), QStringLiteral("EUR")));

        storage::SteamRefreshUpdate failedRefresh;
        failedRefresh.gameId = gameId;
        failedRefresh.attemptedAt = QStringLiteral("2026-10-05T10:00:00Z");
        failedRefresh.safeError = QStringLiteral("Steam request failed.");
        const auto failedReply = run(services::SteamService::applyRefresh(
            profile.path(), {failedRefresh}));
        QVERIFY(!failedReply.error.isError());
        const auto afterFailure = repositories.steam.get(gameId);
        QVERIFY(afterFailure.succeeded());
        QCOMPARE(afterFailure.value.currentPriceMinor.value(), qint64(1999));
        QCOMPARE(afterFailure.value.lastRefreshError, failedRefresh.safeError);
        const auto afterFailureHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("TR"), QStringLiteral("EUR"),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(afterFailureHistory.succeeded());
        QCOMPARE(afterFailureHistory.value.statistics.observationCount, qint64(1));

        storage::SteamGame canceledChange = afterFailure.value;
        canceledChange.currentPriceMinor = 1700;
        int cancellationChecks = 0;
        const std::function<bool()> cancelAfterWrite = [&cancellationChecks] {
            return ++cancellationChecks >= 2;
        };
        storage::SteamRefreshUpdate canceledRefresh;
        canceledRefresh.gameId = gameId;
        canceledRefresh.attemptedAt = QStringLiteral("2026-10-06T10:00:00Z");
        canceledRefresh.refreshedGame = canceledChange;
        const auto canceledReply = services::SteamService::applyRefresh(
            profile.path(), {canceledRefresh})(repositories, cancelAfterWrite);
        QCOMPARE(canceledReply.error.category(), domain::ErrorCategory::Cancelled);
        QCOMPARE(cancellationChecks, 2);
        const auto afterCancellation = repositories.steam.get(gameId);
        QVERIFY(afterCancellation.succeeded());
        QCOMPARE(afterCancellation.value.currentPriceMinor.value(), qint64(1999));
        QCOMPARE(afterCancellation.value.lastAttemptedAt, failedRefresh.attemptedAt);
        const auto afterCancellationHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("TR"), QStringLiteral("EUR"),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(afterCancellationHistory.succeeded());
        QCOMPARE(afterCancellationHistory.value.statistics.observationCount, qint64(1));

        const auto freeAdded = addPriceGame(repositories.steam, 67890,
            QStringLiteral("Free and missing states"), QStringLiteral("TR"), QString(),
            QStringLiteral("free"), true, 0, 0, 0,
            QStringLiteral("2026-10-03T09:00:00Z"));
        QVERIFY(freeAdded.succeeded());
        auto freeHistory = repositories.steam.priceHistory(
            freeAdded.value.id, QStringLiteral("TR"), QString(),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(freeHistory.succeeded());
        QCOMPARE(freeHistory.value.statistics.observationCount, qint64(1));
        QCOMPARE(freeHistory.value.statistics.comparablePriceCount, qint64(1));
        QCOMPARE(freeHistory.value.points.front().priceMinor.value(), qint64(0));

        auto freeCurrent = repositories.steam.get(freeAdded.value.id);
        QVERIFY(freeCurrent.succeeded());
        QVERIFY(refreshPriceGame(repositories.steam, freeCurrent.value,
            QStringLiteral("2026-10-03T18:00:00Z")).succeeded());
        freeCurrent = repositories.steam.get(freeAdded.value.id);
        QVERIFY(freeCurrent.succeeded());
        storage::SteamGame unavailable = freeCurrent.value;
        unavailable.priceStatus = QStringLiteral("unavailable");
        unavailable.regularPriceMinor.reset();
        unavailable.currentPriceMinor.reset();
        QVERIFY(refreshPriceGame(repositories.steam, unavailable,
            QStringLiteral("2026-10-04T09:00:00Z")).succeeded());
        freeCurrent = repositories.steam.get(freeAdded.value.id);
        QVERIFY(freeCurrent.succeeded());
        storage::SteamGame unsupported = freeCurrent.value;
        unsupported.priceStatus = QStringLiteral("unsupported");
        QVERIFY(refreshPriceGame(repositories.steam, unsupported,
            QStringLiteral("2026-10-05T09:00:00Z")).succeeded());

        freeHistory = repositories.steam.priceHistory(
            freeAdded.value.id, QStringLiteral("TR"), QString(),
            storage::SteamPriceHistoryRange::All);
        QVERIFY(freeHistory.succeeded());
        QCOMPARE(freeHistory.value.statistics.observationCount, qint64(3));
        QCOMPARE(freeHistory.value.statistics.comparablePriceCount, qint64(1));
        QCOMPARE(freeHistory.value.statistics.latestObservationState, QStringLiteral("unsupported"));
        QCOMPARE(freeHistory.value.statistics.latestPriceMinor.value(), qint64(0));
        QCOMPARE(freeHistory.value.statistics.lowestPriceMinor.value(), qint64(0));
        QCOMPARE(freeHistory.value.statistics.highestPriceMinor.value(), qint64(0));
        QCOMPARE(freeHistory.value.points.at(0).priceState, QStringLiteral("free"));
        QVERIFY(freeHistory.value.points.at(0).priceMinor.has_value());
        QCOMPARE(freeHistory.value.points.at(1).priceState, QStringLiteral("unavailable"));
        QVERIFY(!freeHistory.value.points.at(1).priceMinor.has_value());
        QCOMPARE(freeHistory.value.points.at(2).priceState, QStringLiteral("unsupported"));
        QVERIFY(!freeHistory.value.points.at(2).priceMinor.has_value());

        const auto offlineReply = run(services::SteamService::loadPriceHistory(
            gameId, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All, true));
        QVERIFY(!offlineReply.error.isError());
        QCOMPARE(offlineReply.values.value(QStringLiteral("points")).toList().size(), 3);
        const QVariantList offlineMarkets = offlineReply.values
            .value(QStringLiteral("markets")).toList();
        QCOMPARE(offlineMarkets.size(), 3);
        bool serviceHasUsUsd = false;
        for (const QVariant &marketValue : offlineMarkets) {
            const QVariantMap market = marketValue.toMap();
            if (market.value(QStringLiteral("countryCode")).toString() == QStringLiteral("US")
                && market.value(QStringLiteral("currencyCode")).toString()
                    == QStringLiteral("USD")) {
                serviceHasUsUsd = true;
            }
        }
        QVERIFY(serviceHasUsUsd);
        const auto canceledHistoryReply = services::SteamService::loadPriceHistory(
            gameId, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All, true)(repositories, [] { return true; });
        QCOMPARE(canceledHistoryReply.error.category(), domain::ErrorCategory::Cancelled);

        QVERIFY(!run(services::SteamService::deleteGame(profile.path(), gameId))
                     .error.isError());
        QVERIFY(!repositories.steam.get(gameId).succeeded());
        QSqlQuery cascadedCount(database);
        cascadedCount.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM steam_price_observations WHERE steam_game_id=?"));
        cascadedCount.addBindValue(gameId);
        QVERIFY(cascadedCount.exec());
        QVERIFY(cascadedCount.next());
        QCOMPARE(cascadedCount.value(0).toLongLong(), qint64(0));
        cascadedCount.finish();
        const auto deletedHistory = repositories.steam.priceHistory(
            gameId, QStringLiteral("US"), QStringLiteral("USD"),
            storage::SteamPriceHistoryRange::All);
        QCOMPARE(deletedHistory.error.category(), domain::ErrorCategory::NotFound);
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SteamFeatureTest::artworkStoreStagesBoundedContentAndImageProviderLoadsById()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QByteArray image = QByteArray::fromBase64(
        fixture(QStringLiteral("steam-artwork-1px.png.b64")).trimmed());
    QVERIFY(!image.isEmpty());
    services::StagedSteamArtwork staged;
    const application::CancellationToken noCancellation;
    QVERIFY(!services::SteamArtworkStore::stageResponse(
        profile.path(), 12345, image, noCancellation, &staged).isError());
    QVERIFY(services::SteamArtworkStore::stageResponse(
        profile.path(), 12345, QByteArray(2 * 1024 * 1024 + 1, 'x'),
        noCancellation, &staged).isError());
    QVERIFY(staged.asset.filename.startsWith(QStringLiteral("12345-")));
    QVERIFY(staged.asset.byteSize <= services::SteamArtworkStore::maximumArtworkBytes);
    QVERIFY(!staged.stagingFilename.isEmpty());
    QVERIFY(services::SteamArtworkStore::install(profile.path(), staged));
    QCOMPARE(services::SteamArtworkStore::imageSource(profile.path(), staged.asset.filename),
             QStringLiteral("image://steam-artwork/") + staged.asset.filename);

    providers::SteamArtworkImageProvider provider(profile.path());
    QSize actual;
    const QImage decoded = provider.requestImage(staged.asset.filename, &actual, QSize(64, 64));
    QVERIFY(!decoded.isNull());
    QVERIFY(actual.width() <= 64 && actual.height() <= 64);
    QVERIFY(provider.requestImage(QStringLiteral("../outside.png"), &actual, QSize(64, 64)).isNull());
    provider.invalidate(staged.asset.filename);
    QVERIFY(services::SteamArtworkStore::stageDeletion(
        profile.path(), staged.asset.filename, &staged.stagingFilename));
    services::SteamArtworkStore::finishDeletion(staged.stagingFilename);
    QVERIFY(services::SteamArtworkStore::imageSource(profile.path(), staged.asset.filename).isEmpty());
}

void SteamFeatureTest::modelExposesMissingFreeAndPaidStatesWithoutZeroFallback()
{
    models::SteamGamesModel model;
    QSignalSpy countSpy(&model, &models::SteamGamesModel::countChanged);
    model.append({{QStringLiteral("id"), 1}, {QStringLiteral("steamAppId"), 12345},
                  {QStringLiteral("name"), QStringLiteral("Paid")},
                  {QStringLiteral("priceStatus"), QStringLiteral("priced")},
                  {QStringLiteral("currency"), QStringLiteral("USD")},
                  {QStringLiteral("currentPriceMinor"), 0},
                  {QStringLiteral("regularPriceMinor"), 999},
                  {QStringLiteral("discountPercent"), 0}});
    QCOMPARE(model.data(model.index(0), models::SteamGamesModel::PriceDisplayRole).toString(),
             QStringLiteral("0.00 USD"));
    QCOMPARE(model.data(model.index(0), models::SteamGamesModel::PriceStateLabelRole).toString(),
             QStringLiteral("Paid price"));
    QCOMPARE(model.data(model.index(0), models::SteamGamesModel::DiscountPercentRole).toInt(), 100);

    model.append({{QStringLiteral("id"), 2}, {QStringLiteral("steamAppId"), 54321},
                  {QStringLiteral("name"), QStringLiteral("Free")},
                  {QStringLiteral("priceStatus"), QStringLiteral("free")},
                  {QStringLiteral("currency"), QString()},
                  {QStringLiteral("currentPriceMinor"), 0},
                  {QStringLiteral("regularPriceMinor"), 0}});
    model.append({{QStringLiteral("id"), 3}, {QStringLiteral("steamAppId"), 98765},
                  {QStringLiteral("name"), QStringLiteral("Missing")},
                  {QStringLiteral("priceStatus"), QStringLiteral("unavailable")}});
    model.append({{QStringLiteral("id"), 4}, {QStringLiteral("steamAppId"), 98766},
                  {QStringLiteral("name"), QStringLiteral("Missing amount")},
                  {QStringLiteral("priceStatus"), QStringLiteral("priced")},
                  {QStringLiteral("currency"), QStringLiteral("USD")}});
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(countSpy.size(), 4);
    QCOMPARE(model.data(model.index(model.rowForId(2)), models::SteamGamesModel::PriceDisplayRole).toString(),
             QStringLiteral("Free"));
    QVERIFY(model.data(model.index(model.rowForId(3)), models::SteamGamesModel::PriceDisplayRole)
                .toString().isEmpty());
    QCOMPARE(model.data(model.index(model.rowForId(3)), models::SteamGamesModel::PriceStateLabelRole)
                 .toString(), QStringLiteral("Unavailable"));
    QVERIFY(model.data(model.index(model.rowForId(4)), models::SteamGamesModel::PriceDisplayRole)
                .toString().isEmpty());
    QCOMPARE(model.data(model.index(model.rowForId(4)), models::SteamGamesModel::PriceStateLabelRole)
                 .toString(), QStringLiteral("Price missing"));
}

QTEST_MAIN(SteamFeatureTest)
#include "steam_feature_test.moc"
