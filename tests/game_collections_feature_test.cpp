#include "application/application_service_graph.h"
#include "controllers/steam_collections_controller.h"
#include "controllers/steam_games_controller.h"
#include "controllers/steam_release_controller.h"
#include "domain/steam_collections.h"
#include "domain/steam_game.h"
#include "domain/steam_release.h"
#include "domain/steam_game_updates.h"
#include "services/steam_collections_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QTimer>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <utility>

using namespace something;

namespace {

const QString kTimestamp = QStringLiteral("2026-10-30T12:00:00.000Z");

QString storeUrl(quint32 appId)
{
    return QStringLiteral("https://store.steampowered.com/app/%1/").arg(appId);
}

domain::SteamGameCatalogDetails catalogDetails(quint32 appId, QString name)
{
    return {appId, storeUrl(appId), std::move(name), {}};
}

domain::SteamCollectionInput collectionInput(QString name)
{
    return {std::move(name), {}, {}, {}};
}

storage::SteamGame priceGame(quint32 appId, QString name)
{
    storage::SteamGame result;
    result.steamAppId = appId;
    result.storeUrl = storeUrl(appId);
    result.name = std::move(name);
    result.priceStatus = QStringLiteral("priced");
    result.currency = QStringLiteral("USD");
    result.regularPriceMinor = 1999;
    result.currentPriceMinor = 999;
    result.discountPercent = 50;
    result.priceCountryCode = QStringLiteral("US");
    result.lastCheckedAt = kTimestamp;
    result.lastAttemptedAt = kTimestamp;
    return result;
}

domain::SteamTrackedRelease releaseFollow(quint32 appId, QString name)
{
    domain::SteamTrackedRelease result;
    result.appId = appId;
    result.canonicalStoreUrl = storeUrl(appId);
    result.title = std::move(name);
    result.followedAtUtc = kTimestamp;
    result.status = domain::SteamReleaseStatus::InformationUnavailable;
    return result;
}

domain::SteamGameUpdateFollow updatesFollow(quint32 appId, QString name)
{
    domain::SteamGameUpdateFollow result;
    result.appId = appId;
    result.canonicalStoreUrl = storeUrl(appId);
    result.name = std::move(name);
    result.addedAtUtc = kTimestamp;
    return result;
}

bool openDatabase(QSqlDatabase *database, QString *connectionName)
{
    *connectionName = QStringLiteral("game-collections-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    database->setDatabaseName(QStringLiteral(":memory:"));
    return database->open() && storage::SchemaMigrator::apply(*database);
}

bool openDatabaseAt(QSqlDatabase *database, QString *connectionName,
                    const QString &databasePath)
{
    *connectionName = QStringLiteral("game-collections-file-test-%1")
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

storage::StorageReply runStorage(storage::StorageCommand command,
                                 storage::StorageRepositories &repositories)
{
    return command(repositories, [] { return false; });
}

} // namespace

class GameCollectionsFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void storeUrlNormalizationRejectsNonstandardPorts();
    void migrationSeedsTheCatalogWithoutChangingExistingSteamRows();
    void collectionCrudEnforcesNormalizedUniqueNames();
    void membershipConstraintsAndPersonalMetadataPersist();
    void offlineCatalogAndFeatureIndependence();
    void startupRetriesMissingArtworkInBoundedOfflineTurns();
    void priceAndReleaseOfflineBatchesKeepSavedData();
};

void GameCollectionsFeatureTest::storeUrlNormalizationRejectsNonstandardPorts()
{
    const auto httpsDefaultPort = domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com:443/app/992000/"));
    QVERIFY(httpsDefaultPort.isValid());
    QCOMPARE(httpsDefaultPort.canonicalUrl,
             QStringLiteral("https://store.steampowered.com/app/992000/"));

    const auto httpDefaultPort = domain::normalizeSteamStoreUrl(
        QStringLiteral("http://store.steampowered.com:80/app/992000/"));
    QVERIFY(httpDefaultPort.isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("https://store.steampowered.com:444/app/992000/")).isValid());
    QVERIFY(!domain::normalizeSteamStoreUrl(
        QStringLiteral("http://store.steampowered.com:443/app/992000/")).isValid());
}

void GameCollectionsFeatureTest::migrationSeedsTheCatalogWithoutChangingExistingSteamRows()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));

    constexpr quint32 priceAppId = 992001;
    constexpr quint32 releaseAppId = 992002;
    constexpr quint32 updatesAppId = 992003;
    qint64 priceRowId = 0;
    {
        storage::StorageRepositories repositories(database);
        const auto price = repositories.steam.add(
            priceGame(priceAppId, QStringLiteral("Legacy price entry")));
        QVERIFY(price.succeeded());
        priceRowId = price.value.id;

        const auto release = repositories.steamReleases.add(
            releaseFollow(releaseAppId, QStringLiteral("Legacy release entry")));
        QVERIFY(release.succeeded());
        QVERIFY(repositories.gameUpdates.add(
            updatesFollow(updatesAppId, QStringLiteral("Legacy updates entry"))).succeeded());
    }

    // Recreate a schema-version-17 profile: leave feature rows intact, but remove
    // the tables introduced by migration 18 and its ledger entry.
    {
        QSqlQuery legacy(database);
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collection_games")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collections")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_catalog")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_segments")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_sessions")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracked_applications")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracking_settings")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_excluded_applications")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automation_executions")));
        QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automations")));
        QVERIFY(legacy.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version>=18")));
        QVERIFY(legacy.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")));
        QVERIFY(legacy.next());
        QCOMPARE(legacy.value(0).toInt(), 17);
        legacy.finish();
    }

    QString diagnostic;
    QVERIFY2(storage::SchemaMigrator::apply(database, &diagnostic), qPrintable(diagnostic));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 21);
    {
        storage::StorageRepositories repositories(database);
        const auto price = repositories.steam.get(priceRowId);
        QVERIFY(price.succeeded());
        QCOMPARE(price.value.name, QStringLiteral("Legacy price entry"));
        QVERIFY(repositories.steamReleases.get(releaseAppId).succeeded());
        QVERIFY(repositories.gameUpdates.get(updatesAppId).succeeded());

        const auto priceCatalog = repositories.steamCatalog.get(priceAppId);
        const auto releaseCatalog = repositories.steamCatalog.get(releaseAppId);
        const auto updatesCatalog = repositories.steamCatalog.get(updatesAppId);
        QVERIFY(priceCatalog.succeeded());
        QVERIFY(releaseCatalog.succeeded());
        QVERIFY(updatesCatalog.succeeded());
        QCOMPARE(priceCatalog.value.name, QStringLiteral("Legacy price entry"));
        QCOMPARE(releaseCatalog.value.name, QStringLiteral("Legacy release entry"));
        QCOMPARE(updatesCatalog.value.name, QStringLiteral("Legacy updates entry"));
        QCOMPARE(priceCatalog.value.canonicalStoreUrl, storeUrl(priceAppId));
    }

    closeDatabase(&database, connectionName);
}

void GameCollectionsFeatureTest::collectionCrudEnforcesNormalizedUniqueNames()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        const auto first = repositories.steamCollections.create(
            {QStringLiteral("  Échos  "), QStringLiteral("  My library  "),
             QStringLiteral("#abc"), QStringLiteral("🎮")});
        QVERIFY(first.succeeded());
        QCOMPARE(first.value.name, QStringLiteral("Échos"));
        QCOMPARE(first.value.description, QStringLiteral("My library"));
        QCOMPARE(first.value.color, QStringLiteral("#aabbcc"));

        // This spelling is decomposed and case-folded to the first collection's key.
        const auto duplicate = repositories.steamCollections.create(
            collectionInput(QStringLiteral("e\u0301CHOS")));
        QCOMPARE(duplicate.error.category(), domain::ErrorCategory::Conflict);

        const auto second = repositories.steamCollections.create(
            collectionInput(QStringLiteral("Other")));
        QVERIFY(second.succeeded());
        const auto renamed = repositories.steamCollections.update(
            first.value.id,
            {QStringLiteral("新しいゲーム"), QStringLiteral("Revised"),
             QStringLiteral("#123456"), QStringLiteral("🕹️")});
        QVERIFY(renamed.succeeded());
        QCOMPARE(renamed.value.id, first.value.id);
        QCOMPARE(renamed.value.name, QStringLiteral("新しいゲーム"));
        QCOMPARE(renamed.value.description, QStringLiteral("Revised"));

        const auto renameConflict = repositories.steamCollections.update(
            first.value.id, collectionInput(QStringLiteral("other")));
        QCOMPARE(renameConflict.error.category(), domain::ErrorCategory::Conflict);

        const auto maximumName = repositories.steamCollections.create(
            collectionInput(QString(80, QLatin1Char('x'))));
        QVERIFY(maximumName.succeeded());
        const auto overlongName = repositories.steamCollections.create(
            collectionInput(QString(81, QLatin1Char('x'))));
        QCOMPARE(overlongName.error.category(), domain::ErrorCategory::Validation);

        QVERIFY(repositories.steamCollections.reorder(
            {second.value.id, first.value.id, maximumName.value.id}).succeeded());
        const auto ordered = repositories.steamCollections.list();
        QVERIFY(ordered.succeeded());
        QCOMPARE(ordered.value.size(), 3);
        QCOMPARE(ordered.value.at(0).id, second.value.id);
        QCOMPARE(ordered.value.at(1).id, first.value.id);

        const auto removed = repositories.steamCollections.remove(maximumName.value.id);
        QVERIFY(removed.succeeded());
        QCOMPARE(removed.value.removedMemberships, 0);
        QCOMPARE(repositories.steamCollections.get(maximumName.value.id).error.category(),
                 domain::ErrorCategory::NotFound);
    }
    closeDatabase(&database, connectionName);
}

void GameCollectionsFeatureTest::membershipConstraintsAndPersonalMetadataPersist()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString databasePath = temporary.filePath(QStringLiteral("collections.sqlite"));
    constexpr quint32 appId = 992100;
    qint64 firstCollectionId = 0;
    qint64 secondCollectionId = 0;

    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabaseAt(&database, &connectionName, databasePath));
    {
        storage::StorageRepositories repositories(database);
        const auto first = repositories.steamCollections.create(
            collectionInput(QStringLiteral("First collection")));
        const auto second = repositories.steamCollections.create(
            collectionInput(QStringLiteral("Second collection")));
        QVERIFY(first.succeeded());
        QVERIFY(second.succeeded());
        firstCollectionId = first.value.id;
        secondCollectionId = second.value.id;

        domain::SteamCollectionGameInput firstMembership;
        firstMembership.steamAppId = appId;
        firstMembership.status = domain::SteamCollectionGameStatus::Playing;
        firstMembership.personalRating = 4;
        firstMembership.personalComment = QStringLiteral("First note");
        const auto addedFirst = repositories.steamCollections.addGame(
            firstCollectionId, catalogDetails(appId, QStringLiteral("Persistent game")),
            firstMembership);
        QVERIFY(addedFirst.succeeded());

        const auto duplicate = repositories.steamCollections.addGame(
            firstCollectionId, catalogDetails(appId, QStringLiteral("Persistent game")),
            firstMembership);
        QCOMPARE(duplicate.error.category(), domain::ErrorCategory::Conflict);

        domain::SteamCollectionGameInput secondMembership;
        secondMembership.steamAppId = appId;
        secondMembership.status = domain::SteamCollectionGameStatus::Backlog;
        secondMembership.personalRating = 2;
        secondMembership.personalComment = QStringLiteral("Separate note");
        QVERIFY(repositories.steamCollections.addGame(
            secondCollectionId, catalogDetails(appId, QStringLiteral("Persistent game")),
            secondMembership).succeeded());

        const auto invalidMembership = repositories.steamCollections.addGame(
            secondCollectionId, catalogDetails(appId, QStringLiteral("Persistent game")),
            domain::SteamCollectionGameInput{});
        QCOMPARE(invalidMembership.error.category(), domain::ErrorCategory::Validation);

        firstMembership.status = domain::SteamCollectionGameStatus::Completed;
        firstMembership.personalRating = 5;
        firstMembership.personalComment = QStringLiteral("Finished — 100% 🎮");
        const auto updated = repositories.steamCollections.updateGame(
            firstCollectionId, firstMembership);
        QVERIFY(updated.succeeded());
        QCOMPARE(updated.value.personalComment, firstMembership.personalComment);
    }
    closeDatabase(&database, connectionName);

    QString reopenedConnection;
    QSqlDatabase reopened;
    QVERIFY(openDatabaseAt(&reopened, &reopenedConnection, databasePath));
    {
        storage::StorageRepositories repositories(reopened);
        const auto allMemberships = repositories.steamCollections.allGames();
        QVERIFY(allMemberships.succeeded());
        QCOMPARE(allMemberships.value.size(), 2);

        const auto firstGames = repositories.steamCollections.games(firstCollectionId);
        const auto secondGames = repositories.steamCollections.games(secondCollectionId);
        QVERIFY(firstGames.succeeded());
        QVERIFY(secondGames.succeeded());
        QCOMPARE(firstGames.value.size(), 1);
        QCOMPARE(secondGames.value.size(), 1);
        QVERIFY(firstGames.value.front().status.has_value());
        QVERIFY(firstGames.value.front().personalRating.has_value());
        QVERIFY(secondGames.value.front().status.has_value());
        QVERIFY(secondGames.value.front().personalRating.has_value());
        QCOMPARE(static_cast<int>(firstGames.value.front().status.value()),
                 static_cast<int>(domain::SteamCollectionGameStatus::Completed));
        QCOMPARE(firstGames.value.front().personalRating.value(), 5);
        QCOMPARE(firstGames.value.front().personalComment,
                 QStringLiteral("Finished — 100% 🎮"));
        QCOMPARE(static_cast<int>(secondGames.value.front().status.value()),
                 static_cast<int>(domain::SteamCollectionGameStatus::Backlog));
        QCOMPARE(secondGames.value.front().personalRating.value(), 2);
        QCOMPARE(secondGames.value.front().personalComment, QStringLiteral("Separate note"));

        QVERIFY(repositories.steamCollections.removeGame(firstCollectionId, appId).succeeded());
        QCOMPARE(repositories.steamCollections.games(firstCollectionId).value.size(), 0);
        QVERIFY(repositories.steamCatalog.get(appId).succeeded());
        QVERIFY(repositories.steamCollections.removeGame(secondCollectionId, appId).succeeded());
        QCOMPARE(repositories.steamCollections.allGames().value.size(), 0);
        QVERIFY(repositories.steamCatalog.get(appId).succeeded());
        QVERIFY(repositories.steamCatalog.remove(appId).succeeded());
    }
    closeDatabase(&reopened, reopenedConnection);
}

void GameCollectionsFeatureTest::offlineCatalogAndFeatureIndependence()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        constexpr quint32 appId = 992200;
        const auto price = repositories.steam.add(
            priceGame(appId, QStringLiteral("Shared Steam game")));
        const auto release = repositories.steamReleases.add(
            releaseFollow(appId, QStringLiteral("Shared Steam game")));
        const auto updates = repositories.gameUpdates.add(
            updatesFollow(appId, QStringLiteral("Shared Steam game")));
        QVERIFY(price.succeeded());
        QVERIFY(release.succeeded());
        QVERIFY(updates.succeeded());

        const auto collection = repositories.steamCollections.create(
            collectionInput(QStringLiteral("Offline picks")));
        QVERIFY(collection.succeeded());
        const storage::StorageReply snapshot = runStorage(
            services::SteamCollectionsService::loadSnapshot(QStringLiteral("profile")),
            repositories);
        QVERIFY(!snapshot.error.isError());
        const QVariantList catalog = snapshot.values.value(QStringLiteral("catalog")).toList();
        QCOMPARE(catalog.size(), 1);
        QCOMPARE(catalog.front().toMap().value(QStringLiteral("appId")).toUInt(), appId);

        const storage::StorageReply added = runStorage(
            services::SteamCollectionsService::addExistingGame(
                QStringLiteral("profile"), collection.value.id, appId), repositories);
        QVERIFY(!added.error.isError());
        QCOMPARE(added.values.value(QStringLiteral("mutation")).toMap()
                     .value(QStringLiteral("type")).toString(),
                 QStringLiteral("game-added"));
        const storage::StorageReply duplicate = runStorage(
            services::SteamCollectionsService::addExistingGame(
                QStringLiteral("profile"), collection.value.id, appId), repositories);
        QCOMPARE(duplicate.error.category(), domain::ErrorCategory::Conflict);

        QCOMPARE(repositories.steamCollections.allGames().value.size(), 1);
        QCOMPARE(repositories.steam.list().value.size(), 1);
        QCOMPARE(repositories.steamReleases.list().value.size(), 1);
        QCOMPARE(repositories.gameUpdates.list().value.size(), 1);
        QCOMPARE(repositories.steamCatalog.remove(appId).error.category(),
                 domain::ErrorCategory::Conflict);

        const storage::StorageReply deleted = runStorage(
            services::SteamCollectionsService::deleteCollection(
                QStringLiteral("profile"), collection.value.id), repositories);
        QVERIFY(!deleted.error.isError());
        QCOMPARE(repositories.steamCollections.allGames().value.size(), 0);
        QVERIFY(repositories.steamCatalog.get(appId).succeeded());
        QVERIFY(repositories.steam.get(price.value.id).succeeded());
        QVERIFY(repositories.steamReleases.get(appId).succeeded());
        QVERIFY(repositories.gameUpdates.get(appId).succeeded());

        QVERIFY(repositories.steam.remove(price.value.id).succeeded());
        QVERIFY(repositories.steamReleases.remove(appId).succeeded());
        QVERIFY(repositories.gameUpdates.remove(appId).succeeded());
        QVERIFY(repositories.steamCatalog.remove(appId).succeeded());
    }
    closeDatabase(&database, connectionName);
}

void GameCollectionsFeatureTest::startupRetriesMissingArtworkInBoundedOfflineTurns()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString profileId = QStringLiteral("collections-artwork");
    const QString profileDirectory = QDir(temporary.path()).filePath(
        QStringLiteral("profiles/") + profileId);
    QVERIFY(QDir().mkpath(profileDirectory));
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabaseAt(&database, &connectionName,
                          QDir(profileDirectory).filePath(QStringLiteral("something.sqlite"))));
    constexpr int gameCount = 96;
    {
        storage::StorageRepositories repositories(database);
        const auto collection = repositories.steamCollections.create(
            collectionInput(QStringLiteral("Saved collection")));
        QVERIFY(collection.succeeded());
        for (int index = 0; index < gameCount; ++index) {
            auto details = catalogDetails(993000 + index, QStringLiteral("Saved game %1").arg(index));
            details.artworkUrl = QStringLiteral("https://shared.steamstatic.com/store_item_assets/steam/apps/%1/header.jpg")
                .arg(details.appId);
            QVERIFY(repositories.steamCatalog.upsert(details).succeeded());
            if (index == 0) {
                const QString missingFilename = QString::number(details.appId)
                    + QLatin1Char('-') + QString(64, QLatin1Char('0')) + QStringLiteral(".png");
                QVERIFY(repositories.steamCatalog.setArtwork(details.appId, details.artworkUrl,
                    {details.artworkUrl, missingFilename, QStringLiteral("image/png"), 100}).succeeded());
            }
            QVERIFY(repositories.steamCollections.addGame(
                collection.value.id, details, {details.appId}).succeeded());
        }
    }
    closeDatabase(&database, connectionName);

    application::ApplicationServiceGraph graph(profileId, temporary.path());
    // Reject admission synchronously, before any request reaches the network.
    connect(&graph.capabilities(), &application::CapabilityRegistry::capabilitiesChanged,
            &graph, [&graph] {
        if (graph.capabilities().isAvailable(QStringLiteral("external-network")))
            graph.reportCapabilityUnavailable(QStringLiteral("external-network"),
                domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
    });
    int totalRequests = 0;
    int requestsInTurn = 0;
    int maximumRequestsInTurn = 0;
    bool resetScheduled = false;
    connect(&graph, &application::ApplicationServiceGraph::httpsFinished, &graph,
            [&](quint64, const QByteArray &, domain::AppError error) {
        QCOMPARE(error.category(), domain::ErrorCategory::CapabilityUnavailable);
        ++totalRequests;
        maximumRequestsInTurn = qMax(maximumRequestsInTurn, ++requestsInTurn);
        if (!resetScheduled) {
            resetScheduled = true;
            QTimer::singleShot(0, &graph, [&] {
                requestsInTurn = 0;
                resetScheduled = false;
            });
        }
    });
    controllers::SteamCollectionsController controller(&graph);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(controller.snapshotLoaded(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(totalRequests, gameCount, 10000);
    QCOMPARE(controller.allGamesCount(), gameCount);
    QVERIFY(maximumRequestsInTurn <= 32);
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 10000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 10000);
}

void GameCollectionsFeatureTest::priceAndReleaseOfflineBatchesKeepSavedData()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString profileId = QStringLiteral("steam-offline-batches");
    const QString profileDirectory = QDir(temporary.path()).filePath(
        QStringLiteral("profiles/") + profileId);
    QVERIFY(QDir().mkpath(profileDirectory));
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabaseAt(&database, &connectionName,
                          QDir(profileDirectory).filePath(QStringLiteral("something.sqlite"))));
    constexpr int gameCount = 12;
    {
        storage::StorageRepositories repositories(database);
        for (int index = 0; index < gameCount; ++index) {
            const quint32 appId = 995000 + index;
            const QString name = QStringLiteral("Saved Steam game %1").arg(index);
            QVERIFY(repositories.steam.add(priceGame(appId, name)).succeeded());
            QVERIFY(repositories.steamReleases.add(releaseFollow(appId, name)).succeeded());
        }
    }
    closeDatabase(&database, connectionName);
    application::ApplicationServiceGraph graph(profileId, temporary.path());
    connect(&graph.capabilities(), &application::CapabilityRegistry::capabilitiesChanged,
            &graph, [&graph] {
        if (graph.capabilities().isAvailable(QStringLiteral("external-network")))
            graph.reportCapabilityUnavailable(QStringLiteral("external-network"),
                domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
    });
    controllers::SteamGamesController prices(&graph);
    controllers::SteamReleaseController releases(&graph);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(prices.snapshotLoaded() && releases.snapshotLoaded(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!prices.refreshBusy(), 10000);
    prices.refreshPrices();
    releases.refreshReleases();
    QTRY_VERIFY_WITH_TIMEOUT(!prices.refreshBusy() && !releases.refreshBusy(), 10000);
    QCOMPARE(prices.games()->count(), gameCount);
    QCOMPARE(releases.releases()->count(), gameCount);
    for (int row = 0; row < gameCount; ++row) {
        QCOMPARE(prices.games()->rowAt(row).value(QStringLiteral("currentPriceMinor")).toLongLong(), qint64(999));
        const QVariantMap release = releases.releases()->allRows().at(row);
        QCOMPARE(release.value(QStringLiteral("refreshState")).toString(), QStringLiteral("failed"));
        QVERIFY(!release.value(QStringLiteral("lastAttemptedUpdateAtUtc")).toString().isEmpty());
    }
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 10000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 10000);
}

QTEST_GUILESS_MAIN(GameCollectionsFeatureTest)
#include "game_collections_feature_test.moc"
