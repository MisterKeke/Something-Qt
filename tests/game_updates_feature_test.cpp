#include "application/application_service_graph.h"
#include "controllers/steam_game_updates_controller.h"
#include "domain/steam_game_updates.h"
#include "providers/feature_parsers.h"
#include "providers/provider_policy.h"
#include "providers/steam_news_provider.h"
#include "services/steam_game_updates_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QDir>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QUrlQuery>
#include <QVector>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

using namespace something;

namespace {

const QString kAddedAt = QStringLiteral("2026-10-30T12:00:00.000Z");
const QString kDiscoveredAt = QStringLiteral("2026-10-30T12:01:00.000Z");

bool openDatabase(QSqlDatabase *database, QString *connectionName)
{
    *connectionName = QStringLiteral("game-updates-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    database->setDatabaseName(QStringLiteral(":memory:"));
    return database->open() && storage::SchemaMigrator::apply(*database);
}

bool openDatabaseAt(QSqlDatabase *database, QString *connectionName,
                    const QString &databasePath)
{
    *connectionName = QStringLiteral("game-updates-file-test-%1")
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

domain::SteamGameUpdateFollow follow(quint32 appId, QString name = {})
{
    if (name.isEmpty()) name = QStringLiteral("Fixture Game %1").arg(appId);
    domain::SteamGameUpdateFollow result;
    result.appId = appId;
    result.canonicalStoreUrl = QStringLiteral(
        "https://store.steampowered.com/app/%1/").arg(appId);
    result.name = std::move(name);
    result.addedAtUtc = kAddedAt;
    return result;
}

domain::SteamNewsArticle article(quint32 appId, QString newsId, QString title,
                                 qint64 publishedAt)
{
    domain::SteamNewsArticle result;
    result.appId = appId;
    result.newsId = std::move(newsId);
    result.title = std::move(title);
    result.publishedAtUnixSeconds = publishedAt;
    result.sourceUrl = QStringLiteral("https://store.steampowered.com/news/app/%1/view/%2")
        .arg(appId).arg(result.newsId);
    result.author = QStringLiteral("Fixture author");
    result.feedLabel = QStringLiteral("Fixture feed");
    result.feedName = QStringLiteral("Fixture feed name");
    result.feedType = 1;
    result.contentText = QStringLiteral("Fixture article text");
    return result;
}

storage::SteamGame priceGame(quint32 appId, QString name = {})
{
    if (name.isEmpty()) name = QStringLiteral("Price fixture %1").arg(appId);
    storage::SteamGame result;
    result.steamAppId = appId;
    result.storeUrl = QStringLiteral("https://store.steampowered.com/app/%1/").arg(appId);
    result.name = std::move(name);
    result.priceStatus = QStringLiteral("priced");
    result.currency = QStringLiteral("USD");
    result.regularPriceMinor = 1999;
    result.currentPriceMinor = 999;
    result.discountPercent = 50;
    result.priceCountryCode = QStringLiteral("US");
    result.lastCheckedAt = kAddedAt;
    result.lastAttemptedAt = kAddedAt;
    return result;
}

QJsonObject newsItem(quint32 appId, QString newsId, QString contents,
                     QString sourceUrl = QStringLiteral(
                         "https://store.steampowered.com/news/fixture"))
{
    return {
        {QStringLiteral("gid"), std::move(newsId)},
        {QStringLiteral("appid"), static_cast<double>(appId)},
        {QStringLiteral("title"), QStringLiteral("Fixture patch notes")},
        {QStringLiteral("date"), 1793361600.0},
        {QStringLiteral("url"), std::move(sourceUrl)},
        {QStringLiteral("author"), QStringLiteral("Fixture author")},
        {QStringLiteral("feedlabel"), QStringLiteral("Patch notes")},
        {QStringLiteral("feedname"), QStringLiteral("Fixture feed")},
        {QStringLiteral("feed_type"), 1},
        {QStringLiteral("is_external_url"), true},
        {QStringLiteral("contents"), std::move(contents)},
    };
}

QByteArray newsPayload(quint32 appId, const QJsonArray &items,
                       std::optional<quint32> responseAppId = std::nullopt,
                       std::optional<int> count = std::nullopt)
{
    const QJsonObject appNews{
        {QStringLiteral("appid"), static_cast<double>(responseAppId.value_or(appId))},
        {QStringLiteral("count"), static_cast<double>(count.value_or(
             static_cast<int>(items.size())))},
        {QStringLiteral("newsitems"), items},
    };
    return QJsonDocument(QJsonObject{{QStringLiteral("appnews"), appNews}})
        .toJson(QJsonDocument::Compact);
}

storage::StorageReply runStorage(storage::StorageCommand command,
                                 storage::StorageRepositories &repositories)
{
    return command(repositories, [] { return false; });
}

qint64 articleCount(QSqlDatabase &database, std::optional<quint32> appId = std::nullopt)
{
    QSqlQuery query(database);
    query.prepare(appId
        ? QStringLiteral("SELECT COUNT(*) FROM steam_game_update_news_items WHERE steam_app_id=?")
        : QStringLiteral("SELECT COUNT(*) FROM steam_game_update_news_items"));
    if (appId) query.addBindValue(*appId);
    if (!query.exec() || !query.next()) return -1;
    return query.value(0).toLongLong();
}

} // namespace

class GameUpdatesFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void steamNewsRequestsUseTheFixedBoundedEndpoint();
    void steamNewsParserKeepsSafeTextAndRejectsMalformedPayloads();
    void followsFailuresAndPriceTrackingRemainIndependent();
    void newsDeduplicationReadStateAndPaginationPersist();
    void cacheRetentionIsBoundedPerGameAndOverall();
    void gameUpdatesMigrationsPreserveExistingPriceRows();
    void offlineStartupLoadsCachedNewsWithoutSteamRequests();
    void offlineArtworkAndRefreshYieldAndCompleteEachGameOnce();
};

void GameUpdatesFeatureTest::steamNewsRequestsUseTheFixedBoundedEndpoint()
{
    constexpr quint32 appId = 991001;
    const auto latest = providers::makeSteamNewsProviderRequest(appId);
    QVERIFY(latest.succeeded());
    QCOMPARE(latest.appId, appId);
    QCOMPARE(static_cast<int>(latest.providerRequest.provider),
             static_cast<int>(providers::ProviderId::SteamNews));
    QCOMPARE(latest.providerRequest.url.scheme(), QStringLiteral("https"));
    QCOMPARE(latest.providerRequest.url.host(), QStringLiteral("api.steampowered.com"));
    QCOMPARE(latest.providerRequest.url.path(),
             QStringLiteral("/ISteamNews/GetNewsForApp/v2/"));
    QVERIFY(providers::isAllowedProviderUrl(providers::ProviderId::SteamNews,
                                             latest.providerRequest.url));
    const QUrlQuery latestQuery(latest.providerRequest.url);
    QCOMPARE(latestQuery.queryItemValue(QStringLiteral("appid")), QString::number(appId));
    QCOMPARE(latestQuery.queryItemValue(QStringLiteral("count")), QStringLiteral("20"));
    QCOMPARE(latestQuery.queryItemValue(QStringLiteral("maxlength")), QStringLiteral("6000"));
    QVERIFY(!latestQuery.hasQueryItem(QStringLiteral("enddate")));
    QVERIFY(latest.providerRequest.bypassCache);

    const qint64 olderCursor = 1700000000;
    const auto older = providers::makeSteamNewsProviderRequest(appId, olderCursor);
    QVERIFY(older.succeeded());
    const QUrlQuery olderQuery(older.providerRequest.url);
    QCOMPARE(olderQuery.queryItemValue(QStringLiteral("enddate")),
             QString::number(olderCursor));
    QVERIFY(!older.providerRequest.bypassCache);
    QVERIFY(!providers::makeSteamNewsProviderRequest(0).succeeded());
    QVERIFY(!providers::makeSteamNewsProviderRequest(
        appId, QDateTime::currentSecsSinceEpoch() + 60).succeeded());

    const providers::ProviderPolicy policy =
        providers::providerPolicy(providers::ProviderId::SteamNews);
    QCOMPARE(policy.timeoutMs, 15000);
    QCOMPARE(policy.maximumResponseBytes, qint64(2 * 1024 * 1024));
    QCOMPARE(policy.maximumRedirects, 1);
    QCOMPARE(policy.maximumConcurrentRequests, 3);
    QVERIFY(!providers::isAllowedProviderUrl(providers::ProviderId::SteamNews,
        QUrl(QStringLiteral("https://api.steampowered.com/ISteamNews/other/"))));
}

void GameUpdatesFeatureTest::steamNewsParserKeepsSafeTextAndRejectsMalformedPayloads()
{
    constexpr quint32 appId = 991002;
    QJsonArray items;
    items.push_back(newsItem(appId, QStringLiteral("10001"),
        QStringLiteral("<p>Patch <b>notes</b></p>"
                       "<a href='javascript:alert(1)' onclick='alert(2)'>Open source</a>"
                       "<script>alert('hidden script')</script>"
                       "<img src='https://evil.test/image' alt='hidden image'>"
                       "<iframe>hidden frame</iframe>")));

    const auto parsed = providers::parseSteamNewsArticles(newsPayload(appId, items), appId);
    QVERIFY(parsed.succeeded());
    QCOMPARE(parsed.value.size(), 1);
    const domain::SteamNewsArticle &result = parsed.value.front();
    QCOMPARE(result.newsId, QStringLiteral("10001"));
    QCOMPARE(result.feedLabel, QStringLiteral("Patch notes"));
    QCOMPARE(result.feedName, QStringLiteral("Fixture feed"));
    QCOMPARE(result.feedType.value(), 1);
    QVERIFY(result.externalUrl);
    QVERIFY(result.contentText.contains(QStringLiteral("Patch notes")));
    QVERIFY(result.contentText.contains(QStringLiteral("Open source")));
    QVERIFY(!result.contentText.contains(QStringLiteral("javascript:")));
    QVERIFY(!result.contentText.contains(QStringLiteral("alert")));
    QVERIFY(!result.contentText.contains(QStringLiteral("hidden image")));
    QVERIFY(!result.contentText.contains(QStringLiteral("hidden frame")));

    QVERIFY(providers::parseSteamNewsArticles(QByteArrayLiteral("{"), appId).error.isError());
    QVERIFY(providers::parseSteamNewsArticles(
        newsPayload(appId, items, appId + 1), appId).error.isError());
    QVERIFY(providers::parseSteamNewsArticles(
        newsPayload(appId, items, appId, 0), appId).error.isError());
    QVERIFY(providers::parseSteamNewsArticles(
        newsPayload(appId, items), appId + 1).error.isError());

    QJsonArray duplicateIds;
    duplicateIds.push_back(newsItem(appId, QStringLiteral("10002"), QStringLiteral("One")));
    duplicateIds.push_back(newsItem(appId, QStringLiteral("10002"), QStringLiteral("Two")));
    QVERIFY(providers::parseSteamNewsArticles(
        newsPayload(appId, duplicateIds), appId).error.isError());

    QJsonArray unsafeUrl;
    unsafeUrl.push_back(newsItem(appId, QStringLiteral("10003"), QStringLiteral("Text"),
        QStringLiteral("javascript:alert(1)")));
    QVERIFY(providers::parseSteamNewsArticles(
        newsPayload(appId, unsafeUrl), appId).error.isError());
    QVERIFY(providers::parseSteamNewsArticles(
        QByteArray(2 * 1024 * 1024 + 1, ' '), appId).error.isError());
}

void GameUpdatesFeatureTest::followsFailuresAndPriceTrackingRemainIndependent()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        constexpr quint32 sharedAppId = 991010;
        constexpr quint32 priceOnlyAppId = 991011;
        constexpr quint32 newsOnlyAppId = 991012;

        const auto newsOnly = repositories.gameUpdates.add(follow(newsOnlyAppId));
        QVERIFY(newsOnly.succeeded());
        const auto priceRowsBefore = repositories.steam.list();
        QVERIFY(priceRowsBefore.succeeded());
        QVERIFY(priceRowsBefore.value.isEmpty());

        const auto priceOnly = repositories.steam.add(priceGame(priceOnlyAppId));
        QVERIFY(priceOnly.succeeded());
        QCOMPARE(repositories.gameUpdates.get(priceOnlyAppId).error.category(),
                 domain::ErrorCategory::NotFound);

        const auto followed = repositories.gameUpdates.add(follow(sharedAppId));
        QVERIFY(followed.succeeded());
        const auto price = repositories.steam.add(priceGame(sharedAppId));
        QVERIFY(price.succeeded());
        QVERIFY(repositories.gameUpdates.get(sharedAppId).succeeded());
        QVERIFY(repositories.steam.get(price.value.id).succeeded());

        const auto duplicate = repositories.gameUpdates.add(follow(sharedAppId));
        QCOMPARE(duplicate.error.category(), domain::ErrorCategory::Conflict);
        const auto owned = repositories.gameUpdates.setOwnershipStatus(
            sharedAppId, domain::SteamGameUpdateOwnershipStatus::Owned);
        QVERIFY(owned.succeeded());
        QCOMPARE(static_cast<int>(owned.value.ownershipStatus),
                 static_cast<int>(domain::SteamGameUpdateOwnershipStatus::Owned));

        const QString successAt = QStringLiteral("2026-10-30T13:00:00.000Z");
        const QString failureAt = QStringLiteral("2026-10-30T14:00:00.000Z");
        QVERIFY(repositories.gameUpdates.recordNewsCheck(
            newsOnlyAppId, successAt, true, {}).succeeded());
        QVERIFY(repositories.gameUpdates.recordNewsCheck(
            sharedAppId, successAt, true, {}).succeeded());
        const QString safeFailure = QStringLiteral("Steam News timed out safely.");
        const auto failed = repositories.gameUpdates.recordNewsCheck(
            sharedAppId, failureAt, false, safeFailure);
        QVERIFY(failed.succeeded());
        QCOMPARE(static_cast<int>(failed.value.refreshState),
                 static_cast<int>(domain::SteamGameUpdateRefreshState::Failed));
        QCOMPARE(failed.value.lastSuccessfulNewsCheckAtUtc, successAt);
        QCOMPARE(failed.value.lastAttemptedNewsCheckAtUtc, failureAt);
        QCOMPARE(failed.value.lastRefreshError, safeFailure);
        const auto unaffected = repositories.gameUpdates.get(newsOnlyAppId);
        QVERIFY(unaffected.succeeded());
        QCOMPARE(static_cast<int>(unaffected.value.refreshState),
                 static_cast<int>(domain::SteamGameUpdateRefreshState::Succeeded));
        QVERIFY(unaffected.value.lastRefreshError.isEmpty());
        const auto priceAfterNewsFailure = repositories.steam.get(price.value.id);
        QVERIFY(priceAfterNewsFailure.succeeded());
        QCOMPARE(priceAfterNewsFailure.value.currentPriceMinor.value(), qint64(999));

        QVERIFY(repositories.steam.remove(price.value.id).succeeded());
        QVERIFY(repositories.gameUpdates.get(sharedAppId).succeeded());
        QVERIFY(repositories.steam.get(priceOnly.value.id).succeeded());

        const auto secondPrice = repositories.steam.add(priceGame(sharedAppId));
        QVERIFY(secondPrice.succeeded());
        QVERIFY(repositories.gameUpdates.remove(sharedAppId).succeeded());
        QCOMPARE(repositories.gameUpdates.get(sharedAppId).error.category(),
                 domain::ErrorCategory::NotFound);
        QVERIFY(repositories.steam.get(secondPrice.value.id).succeeded());
    }
    closeDatabase(&database, connectionName);
}

void GameUpdatesFeatureTest::newsDeduplicationReadStateAndPaginationPersist()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabaseAt(&database, &connectionName,
                           temporary.filePath(QStringLiteral("news-cache.sqlite"))));
    {
        storage::StorageRepositories repositories(database);
        constexpr quint32 appId = 991020;
        QVERIFY(repositories.gameUpdates.add(follow(appId)).succeeded());

        QVector<domain::SteamNewsArticle> initial{
            article(appId, QStringLiteral("20001"), QStringLiteral("Older notes"), 100),
            article(appId, QStringLiteral("20002"), QStringLiteral("Newer notes"), 200),
        };
        const auto firstSave = repositories.gameUpdates.upsertNewsPage(
            appId, initial, kDiscoveredAt);
        QVERIFY(firstSave.succeeded());
        QCOMPARE(firstSave.value.insertedCount, 2);
        QVERIFY(repositories.gameUpdates.setNewsRead(
            appId, QStringLiteral("20001"), true).succeeded());

        domain::SteamNewsArticle revised = initial.front();
        revised.title = QStringLiteral("Revised notes");
        revised.contentText = QStringLiteral("Updated plain text");
        const QString redownloadedAt = QStringLiteral("2026-10-31T12:00:00.000Z");
        const auto repeatedSave = repositories.gameUpdates.upsertNewsPage(
            appId, {revised}, redownloadedAt);
        QVERIFY(repeatedSave.succeeded());
        QCOMPARE(repeatedSave.value.insertedCount, 0);
        QCOMPARE(repeatedSave.value.refreshedCount, 1);

        const auto newestPage = repositories.gameUpdates.newsPage(
            appId, false, {}, 1);
        QVERIFY(newestPage.succeeded());
        QCOMPARE(newestPage.value.items.size(), 1);
        QCOMPARE(newestPage.value.items.front().article.newsId, QStringLiteral("20002"));
        QVERIFY(!newestPage.value.items.front().isRead);
        QVERIFY(newestPage.value.hasMore);

        const auto olderPage = repositories.gameUpdates.newsPage(
            appId, false, {}, 1, newestPage.value.nextCursor);
        QVERIFY(olderPage.succeeded());
        QCOMPARE(olderPage.value.items.size(), 1);
        QCOMPARE(olderPage.value.items.front().article.newsId, QStringLiteral("20001"));
        QCOMPARE(olderPage.value.items.front().article.title, QStringLiteral("Revised notes"));
        QVERIFY(olderPage.value.items.front().isRead);
        QCOMPARE(olderPage.value.items.front().discoveredAtUtc, kDiscoveredAt);
        QVERIFY(!olderPage.value.hasMore);

        const auto unreadPage = repositories.gameUpdates.newsPage(
            appId, true, {}, storage::SteamGameUpdatesRepository::maximumNewsPageSize);
        QVERIFY(unreadPage.succeeded());
        QCOMPARE(unreadPage.value.items.size(), 1);
        QCOMPARE(unreadPage.value.items.front().article.newsId, QStringLiteral("20002"));
        const auto unreadCounts = repositories.gameUpdates.unreadNewsCounts();
        QVERIFY(unreadCounts.succeeded());
        QCOMPARE(unreadCounts.value.value(appId), 1);

        const auto serviceSnapshot = runStorage(
            services::SteamGameUpdatesService::loadSnapshot(QStringLiteral("profile")),
            repositories);
        QVERIFY(!serviceSnapshot.error.isError());
        const QVariantList serviceGames = serviceSnapshot.values.value(
            QStringLiteral("games")).toList();
        QCOMPARE(serviceGames.size(), 1);
        const QVariantMap game = serviceGames.front().toMap();
        QCOMPARE(game.value(QStringLiteral("unreadNewsCount")).toInt(), 1);

        const auto servicePage = runStorage(services::SteamGameUpdatesService::loadNewsPage(
            appId, false, {}, 10), repositories);
        QVERIFY(!servicePage.error.isError());
        const QVariantList serviceItems = servicePage.values.value(
            QStringLiteral("items")).toList();
        QCOMPARE(serviceItems.size(), 2);
        QVERIFY(serviceItems.at(1).toMap().value(QStringLiteral("isRead")).toBool());
        QCOMPARE(serviceItems.at(1).toMap().value(
                     QStringLiteral("discoveredAtUtc")).toString(), kDiscoveredAt);
    }
    closeDatabase(&database, connectionName);

    QString reopenedConnectionName;
    QSqlDatabase reopenedDatabase;
    QVERIFY(openDatabaseAt(&reopenedDatabase, &reopenedConnectionName,
                           temporary.filePath(QStringLiteral("news-cache.sqlite"))));
    {
        storage::StorageRepositories repositories(reopenedDatabase);
        const auto persisted = repositories.gameUpdates.newsPage(
            991020, false, {}, storage::SteamGameUpdatesRepository::maximumNewsPageSize);
        QVERIFY(persisted.succeeded());
        QCOMPARE(persisted.value.items.size(), 2);
        const auto readArticle = std::find_if(
            persisted.value.items.cbegin(), persisted.value.items.cend(),
            [](const storage::SteamGameUpdateNewsItem &item) {
                return item.article.newsId == QStringLiteral("20001");
            });
        QVERIFY(readArticle != persisted.value.items.cend());
        QVERIFY(readArticle->isRead);
        QCOMPARE(readArticle->discoveredAtUtc, kDiscoveredAt);
    }
    closeDatabase(&reopenedDatabase, reopenedConnectionName);
}

void GameUpdatesFeatureTest::cacheRetentionIsBoundedPerGameAndOverall()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    {
        storage::StorageRepositories repositories(database);
        constexpr int gameCount = 13;
        constexpr int articlesPerGame = 250;
        constexpr quint32 firstAppId = 991100;
        QVariantMap lastUnreadCounts;

        for (int gameIndex = 0; gameIndex < gameCount; ++gameIndex) {
            const quint32 appId = firstAppId + static_cast<quint32>(gameIndex);
            QVERIFY(repositories.gameUpdates.add(follow(appId)).succeeded());
            const int itemCount = gameIndex == 0 ? articlesPerGame + 1 : articlesPerGame;
            for (int offset = 0; offset < itemCount; offset += 100) {
                QVector<domain::SteamNewsArticle> batch;
                const int end = qMin(itemCount, offset + 100);
                batch.reserve(end - offset);
                for (int index = offset; index < end; ++index) {
                    const int sequence = index + 1;
                    batch.push_back(article(
                        appId,
                        QString::number(gameIndex * 1000 + sequence),
                        QStringLiteral("Retention article %1").arg(sequence),
                        1000000 + static_cast<qint64>(gameIndex * 1000 + sequence)));
                }
                const auto saved = runStorage(services::SteamGameUpdatesService::saveNewsPage(
                    appId, batch, kDiscoveredAt), repositories);
                QVERIFY(!saved.error.isError());
                lastUnreadCounts = saved.values.value(QStringLiteral("unreadNewsCounts")).toMap();
            }
            if (gameIndex == 0) {
                QCOMPARE(articleCount(database, appId), qint64(250));
            }
        }

        QCOMPARE(articleCount(database), qint64(
            storage::SteamGameUpdatesRepository::maximumArticlesOverall));
        QCOMPARE(articleCount(database, firstAppId), qint64(0));
        QCOMPARE(articleCount(database, firstAppId + 12), qint64(250));
        QCOMPARE(lastUnreadCounts.value(QString::number(firstAppId), 0).toInt(), 0);
        QCOMPARE(lastUnreadCounts.value(QString::number(firstAppId + 12)).toInt(), 250);
        int totalUnread = 0;
        for (const QVariant &count : lastUnreadCounts) totalUnread += count.toInt();
        QCOMPARE(totalUnread, storage::SteamGameUpdatesRepository::maximumArticlesOverall);
    }
    closeDatabase(&database, connectionName);
}

void GameUpdatesFeatureTest::gameUpdatesMigrationsPreserveExistingPriceRows()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    constexpr quint32 appId = 991200;
    qint64 priceId = 0;
    {
        storage::StorageRepositories repositories(database);
        const auto price = repositories.steam.add(priceGame(appId, QStringLiteral("Legacy price row")));
        QVERIFY(price.succeeded());
        priceId = price.value.id;
    }

    QSqlQuery legacy(database);
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collection_games")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_collections")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_catalog")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_update_news_items")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE steam_game_update_follows")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_segments")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_sessions")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracked_applications")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_tracking_settings")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE usage_excluded_applications")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automation_executions")));
    QVERIFY(legacy.exec(QStringLiteral("DROP TABLE task_automations")));
    QVERIFY(legacy.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version>=16")));
    QVERIFY(legacy.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")));
    QVERIFY(legacy.next());
    QCOMPARE(legacy.value(0).toInt(), 15);
    legacy.finish();

    QString diagnostic;
    QVERIFY2(storage::SchemaMigrator::apply(database, &diagnostic), qPrintable(diagnostic));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 21);
    {
        storage::StorageRepositories repositories(database);
        const auto preservedPrice = repositories.steam.get(priceId);
        QVERIFY(preservedPrice.succeeded());
        QCOMPARE(preservedPrice.value.name, QStringLiteral("Legacy price row"));
        const auto noNewsFollows = repositories.gameUpdates.list();
        QVERIFY(noNewsFollows.succeeded());
        QVERIFY(noNewsFollows.value.isEmpty());

        const auto followInsert = repositories.gameUpdates.add(follow(appId));
        QVERIFY(followInsert.succeeded());
        QVERIFY(repositories.steam.get(priceId).succeeded());
    }

    QSqlQuery tables(database);
    QVERIFY(tables.exec(QStringLiteral(
        "SELECT name FROM sqlite_master WHERE type='table' AND name IN "
        "('steam_game_update_follows','steam_game_update_news_items')")));
    QStringList names;
    while (tables.next()) names.push_back(tables.value(0).toString());
    QCOMPARE(names.size(), 2);
    tables.finish();

    QSqlQuery foreignKeys(database);
    QVERIFY(foreignKeys.exec(QStringLiteral(
        "PRAGMA foreign_key_list(steam_game_update_news_items)")));
    bool followsAreTheOnlyParent = false;
    while (foreignKeys.next()) {
        QCOMPARE(foreignKeys.value(2).toString(),
                 QStringLiteral("steam_game_update_follows"));
        followsAreTheOnlyParent = true;
    }
    QVERIFY(followsAreTheOnlyParent);
    foreignKeys.finish();
    closeDatabase(&database, connectionName);
}

void GameUpdatesFeatureTest::offlineStartupLoadsCachedNewsWithoutSteamRequests()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString profileId = QStringLiteral("game-updates-offline");
    const QString profileDirectory = QDir(temporary.path()).filePath(
        QStringLiteral("profiles/") + profileId);
    QVERIFY(QDir().mkpath(profileDirectory));
    const QString databasePath = QDir(profileDirectory).filePath(QStringLiteral("something.sqlite"));

    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabaseAt(&database, &connectionName, databasePath));
    {
        storage::StorageRepositories repositories(database);
        constexpr quint32 appId = 991300;
        QVERIFY(repositories.gameUpdates.add(follow(appId, QStringLiteral("Cached offline game")))
                    .succeeded());
        domain::SteamNewsArticle cachedArticle = article(
            appId, QStringLiteral("30001"), QStringLiteral("Cached offline patch"), 500);
        cachedArticle.sourceUrl = QStringLiteral("http://127.0.0.1/unsafe-local-article");
        const QVector<domain::SteamNewsArticle> cached{cachedArticle};
        QVERIFY(repositories.gameUpdates.upsertNewsPage(
            appId, cached, kDiscoveredAt).succeeded());
    }
    closeDatabase(&database, connectionName);

    application::ApplicationServiceGraph graph(profileId, temporary.path());
    controllers::SteamGameUpdatesController controller(&graph);
    QSignalSpy httpsFinished(&graph, &application::ApplicationServiceGraph::httpsFinished);
    QVERIFY(httpsFinished.isValid());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.snapshotLoaded(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.newsLoading(), 10000);

    QCOMPARE(controller.games().size(), 1);
    QCOMPARE(controller.newsItems().size(), 1);
    QCOMPARE(controller.newsItems().front().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Cached offline patch"));
    const QVariantMap cachedItem = controller.newsItems().front().toMap();
    QVERIFY(!cachedItem.value(QStringLiteral("originalLinkAllowed")).toBool());
    controller.openArticle(991300, QStringLiteral("30001"));
    QCOMPARE(controller.errorMessage(),
             QStringLiteral("The original Steam news article could not be opened."));
    QCOMPARE(httpsFinished.count(), 0);

    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 10000);
}

void GameUpdatesFeatureTest::offlineArtworkAndRefreshYieldAndCompleteEachGameOnce()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString profileId = QStringLiteral("game-updates-offline-batch");
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
        for (int index = 0; index < gameCount; ++index) {
            auto game = follow(994000 + index, QStringLiteral("Offline game %1").arg(index));
            game.artwork.sourceUrl = QStringLiteral("https://shared.steamstatic.com/store_item_assets/steam/apps/%1/header.jpg")
                .arg(game.appId);
            QVERIFY(repositories.gameUpdates.add(game).succeeded());
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
    controllers::SteamGameUpdatesController controller(&graph);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(controller.snapshotLoaded(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(totalRequests, gameCount, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.newsLoading(), 10000);
    QVERIFY(maximumRequestsInTurn <= 32);

    maximumRequestsInTurn = requestsInTurn = 0;
    controller.refreshAll();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.refreshBusy(), 10000);
    QCOMPARE(totalRequests, gameCount * 2);
    QVERIFY(maximumRequestsInTurn <= 3);
    QCOMPARE(controller.refreshStatus(), QStringLiteral("Updated 0 of 96 games; 96 failed."));
    QCOMPARE(controller.games().size(), gameCount);
    for (const QVariant &value : controller.games()) {
        const QVariantMap game = value.toMap();
        QCOMPARE(game.value(QStringLiteral("refreshState")).toString(), QStringLiteral("failed"));
        QVERIFY(!game.value(QStringLiteral("lastAttemptedNewsCheckAtUtc")).toString().isEmpty());
    }
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 10000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 10000);
}

QTEST_GUILESS_MAIN(GameUpdatesFeatureTest)
#include "game_updates_feature_test.moc"
