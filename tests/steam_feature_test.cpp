#include "domain/steam_game.h"
#include "domain/steam_release.h"
#include "models/steam_games_model.h"
#include "providers/feature_parsers.h"
#include "providers/provider_policy.h"
#include "providers/steam_release_provider.h"
#include "providers/steam_artwork_image_provider.h"
#include "services/calendar_service.h"
#include "services/steam_artwork_store.h"
#include "services/steam_release_service.h"
#include "services/steam_service.h"
#include "services/task_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QUrlQuery>
#include <QtTest>

#include <cmath>
#include <functional>
#include <optional>
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

bool openDatabaseAt(QSqlDatabase *database, QString *connectionName,
                    const QString &databasePath)
{
    *connectionName = QStringLiteral("steam-release-feature-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    database->setDatabaseName(databasePath);
    return database->open() && storage::SchemaMigrator::apply(*database);
}

void closeDatabase(QSqlDatabase *database, const QString &connectionName)
{
    database->close();
    *database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

QByteArray steamReleaseResponse(quint32 appId,
                                QString type = QStringLiteral("game"),
                                QString releaseDate = {},
                                std::optional<bool> comingSoon = std::nullopt,
                                bool includeReleaseDate = true,
                                bool success = true)
{
    QJsonObject data{
        {QStringLiteral("type"), std::move(type)},
        {QStringLiteral("name"), QStringLiteral("Fixture Release %1").arg(appId)},
        {QStringLiteral("steam_appid"), static_cast<double>(appId)},
        {QStringLiteral("header_image"), QStringLiteral(
             "https://cdn.akamai.steamstatic.com/steam/apps/1/header.jpg")}};
    if (includeReleaseDate) {
        QJsonObject date;
        if (comingSoon) date.insert(QStringLiteral("coming_soon"), *comingSoon);
        if (!releaseDate.isNull()) date.insert(QStringLiteral("date"), std::move(releaseDate));
        data.insert(QStringLiteral("release_date"), date);
    }
    QJsonObject envelope{{QStringLiteral("success"), success}};
    if (success) envelope.insert(QStringLiteral("data"), data);
    QJsonObject root{{QString::number(appId), envelope}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

domain::SteamReleaseDetails exactRelease(quint32 appId, QString title, QDate date,
                                         domain::SteamReleaseStatus status =
                                             domain::SteamReleaseStatus::ExactDateAnnounced)
{
    domain::SteamReleaseDetails details;
    details.appId = appId;
    details.title = std::move(title);
    details.artworkUrl = QStringLiteral(
        "https://cdn.akamai.steamstatic.com/steam/apps/1/header.jpg");
    details.releaseDate.announcedText = date.toString(QStringLiteral("MMM d, yyyy"));
    details.releaseDate.precision = domain::SteamReleaseDatePrecision::ExactDay;
    details.releaseDate.exactDate = date;
    details.status = status;
    return details;
}

domain::SteamTrackedRelease trackedRelease(const domain::SteamReleaseDetails &details,
                                            QString followedAtUtc)
{
    const domain::SteamStoreUrl storeUrl = domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com/app/%1/").arg(details.appId));
    domain::SteamTrackedRelease release;
    release.appId = details.appId;
    release.canonicalStoreUrl = storeUrl.canonicalUrl;
    release.title = details.title;
    release.artwork.sourceUrl = details.artworkUrl;
    release.releaseDate = details.releaseDate;
    release.status = details.status;
    release.followedAtUtc = followedAtUtc;
    release.lastSuccessfulUpdateAtUtc = followedAtUtc;
    release.lastAttemptedUpdateAtUtc = followedAtUtc;
    release.refreshState = domain::SteamReleaseRefreshState::Succeeded;
    return release;
}

storage::StorageReply executeStorageCommand(storage::StorageCommand command,
                                             storage::StorageRepositories &repositories)
{
    return command(repositories, [] { return false; });
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
    void releaseProviderRequestsCanonicalSteamStoreAppIds();
    void releaseDateParsingRetainsPrecisionAndStatus();
    void releasePersistenceCoversChangesFailuresCancellationAndIndependentDeletion();
    void calendarServiceProjectsOnlyExactReleaseEvents();
    void releaseRadarMigrationPreservesExistingPriceProfiles();
    void unsupportedPriceMigrationPreservesChildrenAndRollsBackFailures();
};

void SteamFeatureTest::unsupportedPriceMigrationPreservesChildrenAndRollsBackFailures()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    qint64 gameId = 0;
    qint64 retiredId = 0;
    const QString artworkFilename = QStringLiteral("996001-")
        + QString(64, QLatin1Char('a')) + QStringLiteral(".png");
    {
        storage::StorageRepositories repositories(database);
        auto game = priceGame(996001, QStringLiteral("Migration game"), QStringLiteral("US"),
            QStringLiteral("USD"), QStringLiteral("priced"), true, 1999, 999, 50,
            QStringLiteral("2026-10-01T12:00:00Z"));
        game.imageSourceUrl = QStringLiteral("https://shared.steamstatic.com/steam/apps/996001/header.jpg");
        const auto saved = repositories.steam.add(game);
        QVERIFY(saved.succeeded());
        gameId = saved.value.id;
        QVERIFY(repositories.steam.setArtwork(gameId,
            {artworkFilename, QStringLiteral("image/png"), 100}).succeeded());
        game.steamAppId = 996002;
        game.storeUrl = QStringLiteral("https://store.steampowered.com/app/996002/");
        const auto retired = repositories.steam.add(game);
        QVERIFY(retired.succeeded());
        retiredId = retired.value.id;
        QVERIFY(repositories.steam.remove(retiredId).succeeded());
    }
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("DROP TABLE automation_execution_claims")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE automation_action_results")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE automation_actions")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE automation_conditions")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE automation_executions")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE automation_rules")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE task_automation_executions")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE task_automations")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE usage_segments")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE usage_sessions")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE usage_tracked_applications")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE usage_tracking_settings")));
    QVERIFY(query.exec(QStringLiteral("DROP TABLE usage_excluded_applications")));
    QVERIFY(query.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version>=19")));
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TRIGGER reject_migration_artwork BEFORE INSERT ON steam_game_images "
        "BEGIN SELECT RAISE(ABORT,'fixture restore failure'); END")));
    QString diagnostic;
    QVERIFY(!storage::SchemaMigrator::apply(database, &diagnostic));
    QVERIFY(query.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 18);
    query.finish();
    {
        storage::StorageRepositories repositories(database);
        const auto unchanged = repositories.steam.get(gameId);
        QVERIFY(unchanged.succeeded());
        QVERIFY(unchanged.value.artwork.has_value());
        QCOMPARE(unchanged.value.artwork->filename, artworkFilename);
    }
    QVERIFY(query.exec(QStringLiteral("DROP TRIGGER reject_migration_artwork")));
    QVERIFY2(storage::SchemaMigrator::apply(database, &diagnostic), qPrintable(diagnostic));
    QVERIFY(query.exec(QStringLiteral("PRAGMA foreign_key_check")));
    QVERIFY(!query.next());
    query.finish();
    {
        storage::StorageRepositories repositories(database);
        const auto preserved = repositories.steam.get(gameId);
        QVERIFY(preserved.succeeded());
        QCOMPARE(preserved.value.currentPriceMinor.value(), qint64(999));
        QVERIFY(preserved.value.artwork.has_value());
        QCOMPARE(preserved.value.artwork->filename, artworkFilename);
        const auto history = repositories.steam.priceHistory(gameId, QStringLiteral("US"),
            QStringLiteral("USD"), storage::SteamPriceHistoryRange::All);
        QVERIFY(history.succeeded());
        QCOMPARE(history.value.statistics.observationCount, qint64(1));
        const auto next = addPriceGame(repositories.steam, 996003, QStringLiteral("Next game"),
            QStringLiteral("US"), QStringLiteral("USD"), QStringLiteral("priced"),
            true, 1999, 999, 50, QStringLiteral("2026-10-02T12:00:00Z"));
        QVERIFY(next.succeeded());
        QVERIFY(next.value.id > retiredId);
        auto unsupported = preserved.value;
        unsupported.priceStatus = QStringLiteral("unsupported");
        unsupported.regularPriceMinor.reset();
        unsupported.currentPriceMinor.reset();
        unsupported.discountPercent = 0;
        QVERIFY(refreshPriceGame(repositories.steam, unsupported,
            QStringLiteral("2026-10-03T12:00:00Z")).succeeded());
        QCOMPARE(repositories.steam.get(gameId).value.priceStatus, QStringLiteral("unsupported"));
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

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

void SteamFeatureTest::releaseProviderRequestsCanonicalSteamStoreAppIds()
{
    const auto request = providers::makeSteamReleaseProviderRequest(
        QStringLiteral("http://STORE.SteamPowered.com/app/64001/example/?ignored=1"));
    QVERIFY(request.succeeded());
    QCOMPARE(request.appId, quint32(64001));
    QCOMPARE(request.canonicalStoreUrl,
             QStringLiteral("https://store.steampowered.com/app/64001/"));
    QCOMPARE(static_cast<int>(request.providerRequest.provider),
             static_cast<int>(providers::ProviderId::SteamStore));
    QCOMPARE(request.providerRequest.url.host(), QStringLiteral("store.steampowered.com"));
    QCOMPARE(request.providerRequest.url.path(), QStringLiteral("/api/appdetails"));
    QVERIFY(providers::isAllowedProviderUrl(providers::ProviderId::SteamStore,
                                             request.providerRequest.url));
    const QUrlQuery query(request.providerRequest.url);
    QCOMPARE(query.queryItemValue(QStringLiteral("appids")), QStringLiteral("64001"));
    QCOMPARE(query.queryItemValue(QStringLiteral("cc")), QStringLiteral("us"));
    QCOMPARE(query.queryItemValue(QStringLiteral("l")), QStringLiteral("english"));

    QVERIFY(!providers::makeSteamReleaseProviderRequest(
        QStringLiteral("https://store.steampowered.com.evil.test/app/64001/")).succeeded());
    QVERIFY(!providers::makeSteamReleaseProviderRequest(
        QStringLiteral("https://store.steampowered.com/app/4294967296/")).succeeded());
}

void SteamFeatureTest::releaseDateParsingRetainsPrecisionAndStatus()
{
    constexpr quint32 appId = 64002;
    const auto parse = [appId](QString date, std::optional<bool> comingSoon = true) {
        return providers::parseSteamReleaseDetails(
            steamReleaseResponse(appId, QStringLiteral("game"), std::move(date), comingSoon),
            appId);
    };

    const auto exact = parse(QStringLiteral("October 19, 2026"));
    QVERIFY(exact.succeeded());
    QCOMPARE(domain::steamReleaseDatePrecisionName(exact.value.releaseDate.precision),
             QStringLiteral("exact_day"));
    QCOMPARE(domain::steamReleaseStatusName(exact.value.status),
             QStringLiteral("exact_date_announced"));
    QVERIFY(exact.value.releaseDate.hasExactDate());
    QCOMPARE(exact.value.releaseDate.exactDate.value(), QDate(2026, 10, 19));

    const auto month = parse(QStringLiteral("October 2026"));
    QVERIFY(month.succeeded());
    QCOMPARE(domain::steamReleaseDatePrecisionName(month.value.releaseDate.precision),
             QStringLiteral("month"));
    QCOMPARE(month.value.releaseDate.windowStart.value(), QDate(2026, 10, 1));
    QCOMPARE(month.value.releaseDate.windowEnd.value(), QDate(2026, 10, 31));
    QVERIFY(!month.value.releaseDate.exactDate.has_value());

    const auto year = parse(QStringLiteral("2027"));
    QVERIFY(year.succeeded());
    QCOMPARE(domain::steamReleaseDatePrecisionName(year.value.releaseDate.precision),
             QStringLiteral("year"));
    QCOMPARE(year.value.releaseDate.windowStart.value(), QDate(2027, 1, 1));
    QCOMPARE(year.value.releaseDate.windowEnd.value(), QDate(2027, 12, 31));

    const auto quarter = parse(QStringLiteral("Q2 2027"));
    QVERIFY(quarter.succeeded());
    QCOMPARE(domain::steamReleaseDatePrecisionName(quarter.value.releaseDate.precision),
             QStringLiteral("quarter"));
    QCOMPARE(quarter.value.releaseDate.windowStart.value(), QDate(2027, 4, 1));
    QCOMPARE(quarter.value.releaseDate.windowEnd.value(), QDate(2027, 6, 30));

    const auto season = parse(QStringLiteral("Spring 2028"));
    QVERIFY(season.succeeded());
    QCOMPARE(domain::steamReleaseDatePrecisionName(season.value.releaseDate.precision),
             QStringLiteral("season"));
    QCOMPARE(season.value.releaseDate.announcedText, QStringLiteral("Spring 2028"));
    QVERIFY(!season.value.releaseDate.exactDate.has_value());
    QVERIFY(!season.value.releaseDate.windowStart.has_value());

    const auto range = parse(QStringLiteral("March 2027 - June 2027"));
    QVERIFY(range.succeeded());
    QCOMPARE(domain::steamReleaseDatePrecisionName(range.value.releaseDate.precision),
             QStringLiteral("range"));
    QCOMPARE(range.value.releaseDate.windowStart.value(), QDate(2027, 3, 1));
    QCOMPARE(range.value.releaseDate.windowEnd.value(), QDate(2027, 6, 30));
    QVERIFY(!range.value.releaseDate.exactDate.has_value());

    const auto unfamiliar = parse(QStringLiteral("Later in 2027"));
    QVERIFY(unfamiliar.succeeded());
    QCOMPARE(domain::steamReleaseDatePrecisionName(unfamiliar.value.releaseDate.precision),
             QStringLiteral("range"));
    QCOMPARE(unfamiliar.value.releaseDate.announcedText, QStringLiteral("Later in 2027"));
    QVERIFY(!unfamiliar.value.releaseDate.windowStart.has_value());
    QVERIFY(!unfamiliar.value.releaseDate.exactDate.has_value());

    const auto comingSoon = parse(QStringLiteral("Coming Soon"));
    QVERIFY(comingSoon.succeeded());
    QCOMPARE(domain::steamReleaseStatusName(comingSoon.value.status),
             QStringLiteral("coming_soon"));
    QCOMPARE(domain::steamReleaseDatePrecisionName(comingSoon.value.releaseDate.precision),
             QStringLiteral("unknown"));
    QVERIFY(!comingSoon.value.releaseDate.exactDate.has_value());

    const auto missing = providers::parseSteamReleaseDetails(
        steamReleaseResponse(appId, QStringLiteral("game"), {}, true, false), appId);
    QVERIFY(missing.succeeded());
    QCOMPARE(domain::steamReleaseStatusName(missing.value.status),
             QStringLiteral("information_unavailable"));
    QCOMPARE(domain::steamReleaseDatePrecisionName(missing.value.releaseDate.precision),
             QStringLiteral("unknown"));

    const auto released = parse(QStringLiteral("May 2, 2020"), false);
    QVERIFY(released.succeeded());
    QCOMPARE(domain::steamReleaseStatusName(released.value.status), QStringLiteral("released"));
    QVERIFY(released.value.releaseDate.hasExactDate());
    QCOMPARE(released.value.releaseDate.exactDate.value(), QDate(2020, 5, 2));

    const auto unsupported = providers::parseSteamReleaseDetails(
        steamReleaseResponse(appId, QStringLiteral("dlc"), QStringLiteral("Coming Soon")), appId);
    QCOMPARE(static_cast<int>(unsupported.error.category()),
             static_cast<int>(domain::ErrorCategory::CapabilityUnavailable));
    const auto notFound = providers::parseSteamReleaseDetails(
        steamReleaseResponse(appId, QStringLiteral("game"), {}, std::nullopt, false, false),
        appId);
    QCOMPARE(static_cast<int>(notFound.error.category()),
             static_cast<int>(domain::ErrorCategory::NotFound));
    QVERIFY(providers::parseSteamReleaseDetails(QByteArrayLiteral("{"), appId).error.isError());
    QVERIFY(providers::parseSteamReleaseDetails(
        steamReleaseResponse(appId + 1, QStringLiteral("game"), QStringLiteral("2027")),
        appId).error.isError());
}

void SteamFeatureTest::releasePersistenceCoversChangesFailuresCancellationAndIndependentDeletion()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString databasePath = profile.filePath(QStringLiteral("profile.sqlite"));
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabaseAt(&database, &connectionName, databasePath));

    constexpr quint32 appId = 77001;
    const QString followedAt = QStringLiteral("2026-10-30T12:00:00.000Z");
    qint64 priceGameId = 0;
    {
        storage::StorageRepositories repositories(database);
        const auto price = addPriceGame(repositories.steam, appId,
            QStringLiteral("Price and Release Radar"), QStringLiteral("US"),
            QStringLiteral("USD"), QStringLiteral("priced"), true, 1999, 999, 50,
            followedAt);
        QVERIFY(price.succeeded());
        priceGameId = price.value.id;

        const domain::SteamReleaseDetails original = exactRelease(
            appId, QStringLiteral("Price and Release Radar"), QDate(2027, 3, 4));
        QVERIFY(domain::isValidSteamReleaseDetails(original));
        const QString canonicalUrl = QStringLiteral(
            "https://store.steampowered.com/app/%1/").arg(appId);
        const auto added = executeStorageCommand(services::SteamReleaseService::addRelease(
            canonicalUrl, original, followedAt), repositories);
        QVERIFY2(!added.error.isError(), qPrintable(added.error.userMessage()));
        QCOMPARE(added.values.value(QStringLiteral("release")).toMap()
                     .value(QStringLiteral("exactDate")).toString(),
                 QStringLiteral("2027-03-04"));

        const auto duplicate = executeStorageCommand(services::SteamReleaseService::addRelease(
            canonicalUrl, original, followedAt), repositories);
        QCOMPARE(static_cast<int>(duplicate.error.category()),
                 static_cast<int>(domain::ErrorCategory::Conflict));
        QVERIFY(repositories.steam.get(priceGameId).succeeded());
        QVERIFY(repositories.steamReleases.get(appId).succeeded());

        storage::SteamReleaseRefreshUpdate changed;
        changed.appId = appId;
        changed.attemptedAtUtc = QStringLiteral("2026-11-01T12:00:00.000Z");
        changed.refreshedDetails = exactRelease(
            appId, QStringLiteral("Price and Release Radar"), QDate(2027, 3, 5));
        const auto changedReply = executeStorageCommand(
            services::SteamReleaseService::applyRefresh(profile.path(), {changed}), repositories);
        QVERIFY(!changedReply.error.isError());
        const auto changedRecord = repositories.steamReleases.get(appId);
        QVERIFY(changedRecord.succeeded());
        QVERIFY(changedRecord.value.dateChangeUnacknowledged);
        QVariantList rows = changedReply.values.value(QStringLiteral("releases")).toList();
        QCOMPARE(rows.size(), 1);
        QVariantMap current = rows.front().toMap();
        QVERIFY(current.value(QStringLiteral("dateChangeUnacknowledged")).toBool());
        QCOMPARE(current.value(QStringLiteral("exactDate")).toString(),
                 QStringLiteral("2027-03-05"));

        const auto acknowledged = executeStorageCommand(
            services::SteamReleaseService::acknowledgeDateChange(appId), repositories);
        QVERIFY(!acknowledged.error.isError());
        storage::SteamReleaseRefreshUpdate unchanged;
        unchanged.appId = appId;
        unchanged.attemptedAtUtc = QStringLiteral("2026-11-02T12:00:00.000Z");
        unchanged.refreshedDetails = *changed.refreshedDetails;
        const auto unchangedReply = executeStorageCommand(
            services::SteamReleaseService::applyRefresh(profile.path(), {unchanged}), repositories);
        QVERIFY(!unchangedReply.error.isError());
        rows = unchangedReply.values.value(QStringLiteral("releases")).toList();
        QCOMPARE(rows.size(), 1);
        current = rows.front().toMap();
        QVERIFY(!current.value(QStringLiteral("dateChangeUnacknowledged")).toBool());
        QCOMPARE(current.value(QStringLiteral("lastSuccessfulUpdateAtUtc")).toString(),
                 unchanged.attemptedAtUtc);

        storage::SteamReleaseRefreshUpdate failed;
        failed.appId = appId;
        failed.attemptedAtUtc = QStringLiteral("2026-11-03T12:00:00.000Z");
        failed.safeError = QStringLiteral("Could not connect to Steam.");
        const auto failedReply = executeStorageCommand(
            services::SteamReleaseService::applyRefresh(profile.path(), {failed}), repositories);
        QVERIFY(!failedReply.error.isError());
        rows = failedReply.values.value(QStringLiteral("releases")).toList();
        QCOMPARE(rows.size(), 1);
        current = rows.front().toMap();
        QCOMPARE(current.value(QStringLiteral("refreshState")).toString(), QStringLiteral("failed"));
        QCOMPARE(current.value(QStringLiteral("refreshError")).toString(), failed.safeError);
        QCOMPARE(current.value(QStringLiteral("exactDate")).toString(),
                 QStringLiteral("2027-03-05"));
        QCOMPARE(current.value(QStringLiteral("lastSuccessfulUpdateAtUtc")).toString(),
                 unchanged.attemptedAtUtc);
        QCOMPARE(current.value(QStringLiteral("lastAttemptedUpdateAtUtc")).toString(),
                 failed.attemptedAtUtc);

        storage::SteamReleaseRefreshUpdate canceledUpdate;
        canceledUpdate.appId = appId;
        canceledUpdate.attemptedAtUtc = QStringLiteral("2026-11-04T12:00:00.000Z");
        canceledUpdate.refreshedDetails = exactRelease(
            appId, QStringLiteral("Price and Release Radar"), QDate(2027, 4, 1));
        int cancellationChecks = 0;
        const auto canceledCommand = services::SteamReleaseService::applyRefresh(
            profile.path(), {canceledUpdate});
        const auto canceledReply = canceledCommand(repositories, [&cancellationChecks] {
            return ++cancellationChecks >= 2;
        });
        QVERIFY(canceledReply.error.isCancelled());
        const auto afterCancel = repositories.steamReleases.get(appId);
        QVERIFY(afterCancel.succeeded());
        QVERIFY(afterCancel.value.releaseDate.hasExactDate());
        QCOMPARE(afterCancel.value.releaseDate.exactDate.value(), QDate(2027, 3, 5));
        QCOMPARE(afterCancel.value.lastAttemptedUpdateAtUtc, failed.attemptedAtUtc);
    }
    closeDatabase(&database, connectionName);

    QString reopenedConnection;
    QVERIFY(openDatabaseAt(&database, &reopenedConnection, databasePath));
    {
        storage::StorageRepositories repositories(database);
        const auto snapshot = executeStorageCommand(
            services::SteamReleaseService::loadSnapshot(profile.path()), repositories);
        QVERIFY(!snapshot.error.isError());
        const QVariantList savedReleases = snapshot.values.value(QStringLiteral("releases")).toList();
        QCOMPARE(savedReleases.size(), 1);
        const QVariantMap saved = savedReleases.front().toMap();
        QCOMPARE(saved.value(QStringLiteral("exactDate")).toString(), QStringLiteral("2027-03-05"));
        QCOMPARE(saved.value(QStringLiteral("refreshState")).toString(), QStringLiteral("failed"));
        QCOMPARE(saved.value(QStringLiteral("refreshError")).toString(),
                 QStringLiteral("Could not connect to Steam."));

        const auto deletedPrice = executeStorageCommand(
            services::SteamService::deleteGame(profile.path(), priceGameId), repositories);
        QVERIFY(!deletedPrice.error.isError());
        QVERIFY(!repositories.steam.get(priceGameId).succeeded());
        QVERIFY(repositories.steamReleases.get(appId).succeeded());

        const auto secondPrice = addPriceGame(repositories.steam, appId,
            QStringLiteral("Price and Release Radar"), QStringLiteral("US"),
            QStringLiteral("USD"), QStringLiteral("priced"), true, 1999, 999, 50,
            followedAt);
        QVERIFY(secondPrice.succeeded());
        const auto deletedRelease = executeStorageCommand(
            services::SteamReleaseService::removeRelease(profile.path(), appId), repositories);
        QVERIFY(!deletedRelease.error.isError());
        QVERIFY(!repositories.steamReleases.get(appId).succeeded());
        QVERIFY(repositories.steam.get(secondPrice.value.id).succeeded());
    }
    closeDatabase(&database, reopenedConnection);
}

void SteamFeatureTest::calendarServiceProjectsOnlyExactReleaseEvents()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        const QDate releaseDay(2027, 3, 5);
        storage::TaskInput task;
        task.title = QStringLiteral("Existing calendar deadline");
        task.dueDate = releaseDay.toString(Qt::ISODate);
        const auto createdTask = repositories.tasks.create(task);
        QVERIFY(createdTask.succeeded());

        const QString followedAt = QStringLiteral("2026-10-30T12:00:00.000Z");
        const auto add = [&repositories, &followedAt](const domain::SteamReleaseDetails &details) {
            return repositories.steamReleases.add(trackedRelease(details, followedAt));
        };
        QVERIFY(add(exactRelease(88001, QStringLiteral("Exact announcement"), releaseDay))
                    .succeeded());
        QVERIFY(add(exactRelease(88002, QStringLiteral("Already released"), releaseDay,
                                 domain::SteamReleaseStatus::Released)).succeeded());

        domain::SteamReleaseDetails approximate;
        approximate.appId = 88003;
        approximate.title = QStringLiteral("Approximate announcement");
        approximate.artworkUrl = QStringLiteral(
            "https://cdn.akamai.steamstatic.com/steam/apps/1/header.jpg");
        approximate.releaseDate.announcedText = QStringLiteral("March 2027");
        approximate.releaseDate.precision = domain::SteamReleaseDatePrecision::Month;
        approximate.releaseDate.windowStart = QDate(2027, 3, 1);
        approximate.releaseDate.windowEnd = QDate(2027, 3, 31);
        approximate.status = domain::SteamReleaseStatus::ApproximateDateAnnounced;
        QVERIFY(domain::isValidSteamReleaseDetails(approximate));
        QVERIFY(add(approximate).succeeded());

        domain::SteamReleaseDetails unknown;
        unknown.appId = 88004;
        unknown.title = QStringLiteral("Unknown announcement");
        unknown.artworkUrl = QStringLiteral(
            "https://cdn.akamai.steamstatic.com/steam/apps/1/header.jpg");
        unknown.releaseDate.announcedText = QStringLiteral("Coming Soon");
        unknown.status = domain::SteamReleaseStatus::ComingSoon;
        QVERIFY(domain::isValidSteamReleaseDetails(unknown));
        QVERIFY(add(unknown).succeeded());

        const auto dayReply = executeStorageCommand(
            services::CalendarService::dateWithSchedules(releaseDay, true), repositories);
        QVERIFY2(!dayReply.error.isError(), qPrintable(dayReply.error.userMessage()));
        const QVariantList taskRows = dayReply.values.value(QStringLiteral("tasks")).toList();
        QCOMPARE(taskRows.size(), 1);
        QCOMPARE(taskRows.front().toMap().value(QStringLiteral("id")).toLongLong(),
                 createdTask.value.id);
        QCOMPARE(taskRows.front().toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Existing calendar deadline"));

        const QVariantList releaseRows = dayReply.values.value(QStringLiteral("releases")).toList();
        QCOMPARE(releaseRows.size(), 2);
        for (const QVariant &row : releaseRows) {
            const QVariantMap release = row.toMap();
            QCOMPARE(release.value(QStringLiteral("datePrecision")).toString(),
                     QStringLiteral("exact_day"));
            QCOMPARE(release.value(QStringLiteral("exactDate")).toString(),
                     releaseDay.toString(Qt::ISODate));
        }

        const auto yearReply = executeStorageCommand(
            services::CalendarService::year(2027, true), repositories);
        QVERIFY(!yearReply.error.isError());
        QCOMPARE(yearReply.values.value(QStringLiteral("releases")).toList().size(), 2);

        const auto initialVisibility = repositories.preferences.gameReleasesVisibleInCalendar();
        QVERIFY(initialVisibility.succeeded());
        QVERIFY(initialVisibility.value);
        QVERIFY(repositories.preferences.setGameReleasesVisibleInCalendar(false).succeeded());
        QVERIFY(!repositories.preferences.gameReleasesVisibleInCalendar().value);
        const auto hiddenReply = executeStorageCommand(
            services::CalendarService::dateWithSchedules(releaseDay, false), repositories);
        QVERIFY(!hiddenReply.error.isError());
        QVERIFY(hiddenReply.values.value(QStringLiteral("releases")).toList().isEmpty());
        QCOMPARE(hiddenReply.values.value(QStringLiteral("tasks")).toList().size(), 1);
        QVERIFY(repositories.preferences.setGameReleasesVisibleInCalendar(true).succeeded());
    }
    closeDatabase(&database, connectionName);
}

void SteamFeatureTest::releaseRadarMigrationPreservesExistingPriceProfiles()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    constexpr quint32 appId = 99001;
    const QString followedAt = QStringLiteral("2026-10-30T12:00:00.000Z");
    qint64 priceGameId = 0;
    {
        storage::StorageRepositories repositories(database);
        const auto price = addPriceGame(repositories.steam, appId,
            QStringLiteral("Legacy price-tracked game"), QStringLiteral("US"),
            QStringLiteral("USD"), QStringLiteral("priced"), true, 2999, 2499, 17,
            followedAt);
        QVERIFY(price.succeeded());
        priceGameId = price.value.id;
    }

    {
        QSqlQuery legacy(database);
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collection_games")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collections")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_catalog")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_update_news_items")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_update_follows")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_release_follows")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_segments")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_sessions")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracked_applications")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracking_settings")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_excluded_applications")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_execution_claims")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_action_results")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_actions")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_conditions")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_executions")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_rules")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automation_executions")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automations")));
        QVERIFY(legacy.exec(QStringLiteral(
            "DELETE FROM preferences WHERE key='calendar.game_releases_visible'")));
        QVERIFY(legacy.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version>=13")));
    }

    QString diagnostic;
    QVERIFY2(storage::SchemaMigrator::apply(database, &diagnostic), qPrintable(diagnostic));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 23);
    {
        storage::StorageRepositories repositories(database);
        const auto preserved = repositories.steam.get(priceGameId);
        QVERIFY(preserved.succeeded());
        QCOMPARE(preserved.value.name, QStringLiteral("Legacy price-tracked game"));
        QCOMPARE(preserved.value.currentPriceMinor.value(), qint64(2499));

        const auto radarRows = repositories.steamReleases.list();
        QVERIFY(radarRows.succeeded());
        QVERIFY(radarRows.value.isEmpty());
        const auto visibility = repositories.preferences.gameReleasesVisibleInCalendar();
        QVERIFY(visibility.succeeded());
        QVERIFY(visibility.value);
        QSqlQuery seededPreference(database);
        QVERIFY(seededPreference.exec(QStringLiteral(
            "SELECT value FROM preferences WHERE key='calendar.game_releases_visible'")));
        QVERIFY(seededPreference.next());
        QCOMPARE(seededPreference.value(0).toString(), QStringLiteral("1"));
        seededPreference.finish();
        QSqlQuery migratedVersion(database);
        QVERIFY(migratedVersion.exec(QStringLiteral(
            "SELECT MAX(version) FROM schema_migrations")));
        QVERIFY(migratedVersion.next());
    QCOMPARE(migratedVersion.value(0).toInt(), storage::SchemaMigrator::latestVersion());
    }
    closeDatabase(&database, connectionName);
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
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collection_games")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collections")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_catalog")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_price_observations")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_release_follows")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_update_news_items")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_update_follows")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_segments")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_sessions")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracked_applications")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracking_settings")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_excluded_applications")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_execution_claims")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_action_results")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_actions")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_conditions")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_executions")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE automation_rules")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automation_executions")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automations")));
    QVERIFY(legacy.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version>=12")));
    legacy.finish();

    QString diagnostic;
    QVERIFY2(storage::SchemaMigrator::apply(database, &diagnostic), qPrintable(diagnostic));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 23);

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
