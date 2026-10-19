#include "domain/steam_game.h"
#include "models/steam_games_model.h"
#include "providers/feature_parsers.h"
#include "providers/provider_policy.h"
#include "providers/steam_artwork_image_provider.h"
#include "services/steam_artwork_store.h"
#include "services/steam_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QFile>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

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

} // namespace

class SteamFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void storeUrlAndCountryRulesAreStrictAndCanonical();
    void providerFixturesDistinguishPaidFreeUnavailableAndInvalid();
    void steamArtworkPolicyRejectsUnapprovedHosts();
    void priceFormattingUsesMinorUnitsAndCurrencyPrecision();
    void serviceAndRepositoryPreserveCachedPricesOnPerGameFailure();
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
