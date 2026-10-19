#include "domain/app_error.h"
#include "domain/favorite_rules.h"
#include "models/provider_items_model.h"
#include "providers/provider_parsers.h"
#include "services/favorites_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

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
    *connectionName = QStringLiteral("favorites-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    database->setDatabaseName(QStringLiteral(":memory:"));
    return database->open() && storage::SchemaMigrator::apply(*database);
}

storage::FavoriteNewsItem newsItem(QString source, QString sourceId, QString itemId,
                                   QString publishedAt)
{
    return {std::move(source), std::move(sourceId), std::move(itemId),
            std::move(publishedAt), {}, QStringLiteral("{\"preview\":\"fixture\"}")};
}

} // namespace

class FavoritesFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void sourceReferencesNormalizeAndExternalLinksAreRestricted();
    void providerFixturesBecomeCanonicalFavoriteRows();
    void olderTelegramPagesStayNewestFirstAndAdvanceTheCursor();
    void favoriteModelsFilterAndNotifyForTargetedMediaChanges();
    void favoriteCreationAndCategoryAssignmentRemainSeparate();
    void favoriteNewsCommitDeduplicatesAndKeepsFailedCheckpoints();
};

void FavoritesFeatureTest::sourceReferencesNormalizeAndExternalLinksAreRestricted()
{
    QCOMPARE(domain::normalizeTelegramChannel(QStringLiteral("  @SaMple_Channel  ")),
             QStringLiteral("sample_channel"));
    QVERIFY(domain::normalizeTelegramChannel(QStringLiteral("@short")).isEmpty());
    QCOMPARE(domain::normalizeYouTubeReference(QStringLiteral("@creator.handle")),
             QStringLiteral("creator.handle"));
    bool isChannelId = false;
    QCOMPARE(domain::normalizeYouTubeReference(
                 QStringLiteral("UC0000000000000000000000"), &isChannelId),
             QStringLiteral("UC0000000000000000000000"));
    QVERIFY(isChannelId);
    QCOMPARE(domain::normalizeFavoriteCategoryColor(QStringLiteral(" #Ab3 ")),
             QStringLiteral("#aabb33"));
    QVERIFY(domain::normalizeFavoriteCategoryColor(QStringLiteral("red")).isEmpty());

    QVERIFY(domain::isAllowedExternalFavoriteUrl(
        QStringLiteral("telegram"), QUrl(QStringLiteral("https://t.me/sample_channel/42"))));
    QVERIFY(domain::isAllowedExternalFavoriteUrl(
        QStringLiteral("youtube"), QUrl(QStringLiteral("https://www.youtube.com/watch?v=abcdefghijk"))));
    QVERIFY(!domain::isAllowedExternalFavoriteUrl(
        QStringLiteral("telegram"), QUrl(QStringLiteral("https://t.me.evil.example/sample_channel/42"))));
    QVERIFY(!domain::isAllowedExternalFavoriteUrl(
        QStringLiteral("telegram"), QUrl(QStringLiteral("https://t.me/sample_channel/not-a-post"))));
    QVERIFY(!domain::isAllowedExternalFavoriteUrl(
        QStringLiteral("telegram"), QUrl(QStringLiteral("https://t.me/sample_channel/42/extra"))));
    QVERIFY(!domain::isAllowedExternalFavoriteUrl(
        QStringLiteral("youtube"), QUrl(QStringLiteral("https://www.youtube.com.evil.example/watch?v=abcdefghijk"))));
    QVERIFY(!domain::isAllowedExternalFavoriteUrl(
        QStringLiteral("youtube"), QUrl(QStringLiteral("http://www.youtube.com/watch?v=abcdefghijk"))));
}

void FavoritesFeatureTest::providerFixturesBecomeCanonicalFavoriteRows()
{
    const auto telegram = providers::parseTelegramPosts(fixture(QStringLiteral("telegram-success.html")));
    QVERIFY(telegram.succeeded());
    QVERIFY(!telegram.items.isEmpty());
    const QVariantMap post = services::FavoritesService::telegramPostToMap(
        QStringLiteral("@Sample_Channel"), telegram.items.first());
    QCOMPARE(post.value(QStringLiteral("sourceId")).toString(), QStringLiteral("sample_channel"));
    QVERIFY(post.value(QStringLiteral("externalUrl")).toString().startsWith(
        QStringLiteral("https://t.me/sample_channel/")));
    QVERIFY(!post.value(QStringLiteral("text")).toString().contains(QLatin1Char('<')));

    const QString channelId = QStringLiteral("UC0000000000000000000000");
    const auto videos = providers::parseYouTubeFeed(
        fixture(QStringLiteral("youtube-feed-success.xml")), channelId);
    QVERIFY(videos.succeeded());
    QCOMPARE(videos.items.size(), 1);
    const QVariantMap video = services::FavoritesService::youTubeVideoToMap(videos.items.first());
    QCOMPARE(video.value(QStringLiteral("sourceId")).toString(), channelId);
    QCOMPARE(video.value(QStringLiteral("itemId")).toString(), QStringLiteral("abcdefghijk"));
    QCOMPARE(video.value(QStringLiteral("externalUrl")).toString(),
             QStringLiteral("https://www.youtube.com/watch?v=abcdefghijk"));
}

void FavoritesFeatureTest::olderTelegramPagesStayNewestFirstAndAdvanceTheCursor()
{
    const auto first = providers::parseTelegramPosts(fixture(QStringLiteral("telegram-success.html")));
    const auto older = providers::parseTelegramPosts(QByteArrayLiteral(
        "<html><body>"
        "<div class='tgme_widget_message_wrap' data-post='sample_channel/39'></div>"
        "<div class='tgme_widget_message_wrap' data-post='sample_channel/40'></div>"
        "<div class='tgme_widget_message_wrap' data-post='sample_channel/41'></div>"
        "</body></html>"));
    QVERIFY(first.succeeded());
    QVERIFY(older.succeeded());
    const auto rows = [](const auto &posts) {
        QVariantList values;
        for (const auto &post : posts)
            values.push_back(services::FavoritesService::telegramPostToMap(
                QStringLiteral("sample_channel"), post));
        return values;
    };
    models::ProviderItemsModel model;
    model.replace(rows(first.items));
    const auto ids = [&model] {
        QStringList values;
        for (int row = 0; row < model.count(); ++row)
            values.push_back(model.itemAt(row).value(QStringLiteral("itemId")).toString());
        return values;
    };
    QCOMPARE(domain::oldestTelegramPostId(ids()), quint64(41));
    model.appendUnique(rows(older.items));
    QCOMPARE(ids(), QStringList({QStringLiteral("sample_channel/42"),
        QStringLiteral("sample_channel/41"), QStringLiteral("sample_channel/40"),
        QStringLiteral("sample_channel/39")}));
    QCOMPARE(domain::oldestTelegramPostId(ids()), quint64(39));
    model.appendUnique(rows(older.items));
    QCOMPARE(model.count(), 4);
    QCOMPARE(domain::telegramPostsUrl(QStringLiteral("sample_channel"),
        domain::oldestTelegramPostId(ids())).query(), QStringLiteral("before=39"));
    QCOMPARE(domain::oldestTelegramPostId({QStringLiteral("100"), QStringLiteral("9"),
        QStringLiteral("invalid"), QStringLiteral("0"), QStringLiteral("-1"),
        QStringLiteral("18446744073709551616")}), quint64(9));
    QCOMPARE(domain::oldestTelegramPostId({QStringLiteral("invalid"), QStringLiteral("0")}), quint64(0));
}

void FavoritesFeatureTest::favoriteModelsFilterAndNotifyForTargetedMediaChanges()
{
    models::FavoriteChannelsModel favorites;
    favorites.replace({
        QVariantMap{{QStringLiteral("sourceId"), QStringLiteral("first")},
                    {QStringLiteral("label"), QStringLiteral("First")},
                    {QStringLiteral("categoryId"), 7}},
        QVariantMap{{QStringLiteral("sourceId"), QStringLiteral("second")},
                    {QStringLiteral("label"), QStringLiteral("Second")}},
    });
    QCOMPARE(favorites.count(), 2);
    favorites.setCategoryFilter(QStringLiteral("7"));
    QCOMPARE(favorites.count(), 1);
    favorites.setCategoryFilter(QStringLiteral("uncategorized"));
    QCOMPARE(favorites.count(), 1);
    favorites.setCategoryFilter(QStringLiteral("all"));

    models::ProviderItemsModel posts;
    posts.replace({QVariantMap{{QStringLiteral("itemId"), QStringLiteral("channel/42")},
                               {QStringLiteral("imageSources"), QVariantList{QString()}},
                               {QStringLiteral("title"), QStringLiteral("post")}}});
    QSignalSpy changed(&posts, &QAbstractItemModel::dataChanged);
    posts.updateImageSource(QStringLiteral("channel/42"), 0, QStringLiteral("image://provider-media/1"));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(posts.data(posts.index(0), models::ProviderItemsModel::ImageSourcesRole)
                 .toList().first().toString(), QStringLiteral("image://provider-media/1"));
    posts.appendUnique({QVariantMap{{QStringLiteral("itemId"), QStringLiteral("channel/42")}},
                        QVariantMap{{QStringLiteral("itemId"), QStringLiteral("channel/43")}}});
    QCOMPARE(posts.count(), 2);
    posts.prependUnique({QVariantMap{{QStringLiteral("itemId"), QStringLiteral("channel/40")}},
                         QVariantMap{{QStringLiteral("itemId"), QStringLiteral("channel/42")}},
                         QVariantMap{{QStringLiteral("itemId"), QStringLiteral("channel/41")}}});
    QCOMPARE(posts.count(), 4);
    QCOMPARE(posts.itemAt(0).value(QStringLiteral("itemId")).toString(), QStringLiteral("channel/40"));
    QCOMPARE(posts.itemAt(1).value(QStringLiteral("itemId")).toString(), QStringLiteral("channel/41"));
    QCOMPARE(posts.itemAt(2).value(QStringLiteral("itemId")).toString(), QStringLiteral("channel/42"));
}

void FavoritesFeatureTest::favoriteCreationAndCategoryAssignmentRemainSeparate()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    storage::FavoritesRepository favorites(database);

    const auto category = favorites.createCategory(QStringLiteral("telegram"),
                                                  QStringLiteral("News"),
                                                  QStringLiteral("#abc"));
    QVERIFY(category.succeeded());
    QVERIFY(favorites.addFavorite(QStringLiteral("telegram"),
                                  QStringLiteral("sample_channel"),
                                  QStringLiteral("sample_channel")).succeeded());
    const auto beforeAssignment = favorites.favorites(QStringLiteral("telegram"));
    QVERIFY(beforeAssignment.succeeded());
    QCOMPARE(beforeAssignment.value.size(), 1);
    QVERIFY(!beforeAssignment.value.first().categoryId.has_value());

    const auto duplicate = favorites.addFavorite(QStringLiteral("telegram"),
                                                 QStringLiteral("sample_channel"),
                                                 QStringLiteral("sample_channel"));
    QVERIFY(duplicate.error.category() == domain::ErrorCategory::Conflict);
    QVERIFY(favorites.assignFavoriteCategory(QStringLiteral("telegram"),
                                             QStringLiteral("missing_channel"),
                                             category.value.id).error.category()
            == domain::ErrorCategory::NotFound);
    QVERIFY(favorites.assignFavoriteCategory(QStringLiteral("telegram"),
                                             QStringLiteral("sample_channel"),
                                             category.value.id).succeeded());
    const auto assigned = favorites.favorites(QStringLiteral("telegram"));
    QVERIFY(assigned.succeeded());
    QCOMPARE(assigned.value.first().categoryId.value(), category.value.id);
    QVERIFY(favorites.addFavorite(QStringLiteral("youtube"),
                                  QStringLiteral("UC0000000000000000000001"),
                                  QStringLiteral("channel")).succeeded());
    QVERIFY(favorites.assignFavoriteCategory(QStringLiteral("youtube"),
                                             QStringLiteral("UC0000000000000000000001"),
                                             category.value.id).error.category()
            == domain::ErrorCategory::Validation);
    QVERIFY(favorites.assignFavoriteCategory(QStringLiteral("telegram"),
                                             QStringLiteral("sample_channel"),
                                             std::nullopt).succeeded());
    const auto cleared = favorites.favorites(QStringLiteral("telegram"));
    QVERIFY(cleared.succeeded());
    QVERIFY(!cleared.value.first().categoryId.has_value());
    QVERIFY(favorites.addFavorite(QStringLiteral("telegram"),
                                  QStringLiteral("alpha_channel"),
                                  QStringLiteral("alpha_channel")).succeeded());
    QSqlQuery order(database);
    QVERIFY(order.prepare(QStringLiteral("UPDATE telegram_favorites SET added_at=? WHERE username=?")));
    order.addBindValue(QStringLiteral("2026-10-18T10:00:00Z"));
    order.addBindValue(QStringLiteral("sample_channel"));
    QVERIFY(order.exec());
    order.finish();
    order.bindValue(0, QStringLiteral("2026-10-19T10:00:00Z"));
    order.bindValue(1, QStringLiteral("alpha_channel"));
    QVERIFY(order.exec());
    const auto oldestFirst = favorites.favorites(QStringLiteral("telegram"));
    QVERIFY(oldestFirst.succeeded());
    QCOMPARE(oldestFirst.value.first().sourceId, QStringLiteral("sample_channel"));

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void FavoritesFeatureTest::favoriteNewsCommitDeduplicatesAndKeepsFailedCheckpoints()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, &connectionName));
    storage::FavoriteNewsRepository news(database);

    storage::FavoriteNewsSourceScan telegram;
    telegram.source = QStringLiteral("telegram");
    telegram.sourceId = QStringLiteral("sample_channel");
    telegram.checkedThrough = QStringLiteral("2026-10-18T12:00:00.000Z");
    telegram.updateWindowStart = QStringLiteral("2026-10-18T12:00:00.000Z");
    telegram.succeeded = true;
    telegram.items = {
        newsItem(QStringLiteral("telegram"), QStringLiteral("sample_channel"),
                 QStringLiteral("sample_channel/old"), QStringLiteral("2026-10-18T11:00:00.000Z")),
        newsItem(QStringLiteral("telegram"), QStringLiteral("sample_channel"),
                 QStringLiteral("sample_channel/42"), QStringLiteral("2026-10-19T10:30:00.000Z")),
    };

    storage::FavoriteNewsSourceScan youtubeFailure;
    youtubeFailure.source = QStringLiteral("youtube");
    youtubeFailure.sourceId = QStringLiteral("UC0000000000000000000000");
    youtubeFailure.checkedThrough = QStringLiteral("2026-10-18T12:00:00.000Z");
    youtubeFailure.error = QStringLiteral("YouTube data could not be loaded.");

    storage::FavoriteNewsSourceScan youtubeWarmup;
    youtubeWarmup.source = QStringLiteral("youtube");
    youtubeWarmup.sourceId = QStringLiteral("UC0000000000000000000001");
    youtubeWarmup.checkedThrough = QStringLiteral("2026-10-19T10:00:00.000Z");
    youtubeWarmup.updateWindowStart = QStringLiteral("2026-10-18T12:00:00.000Z");
    youtubeWarmup.succeeded = true;
    youtubeWarmup.items = {
        newsItem(QStringLiteral("youtube"), youtubeWarmup.sourceId,
                 QStringLiteral("abcdefghijk"), QStringLiteral("2026-10-18T20:00:00.000Z")),
    };

    storage::FavoriteNewsScanCommit scan;
    scan.scanStartedAt = QStringLiteral("2026-10-19T11:00:00.000Z");
    scan.errorsJson = QStringLiteral("[{\"source\":\"youtube\"}]");
    scan.sources = {telegram, youtubeFailure, youtubeWarmup};
    const auto first = news.commitScan(scan);
    QVERIFY(first.succeeded());
    QCOMPARE(first.value.size(), 2);
    QCOMPARE(news.newsForScan(scan.scanStartedAt, 20).value.size(), 2);
    const auto telegramCheckpoint = news.checkpoint(QStringLiteral("telegram"),
                                                    QStringLiteral("sample_channel"));
    QVERIFY(telegramCheckpoint.succeeded() && telegramCheckpoint.value.has_value());
    QCOMPARE(telegramCheckpoint.value->checkedThrough, scan.scanStartedAt);
    const auto youtubeCheckpoint = news.checkpoint(QStringLiteral("youtube"),
                                                    youtubeFailure.sourceId);
    QVERIFY(youtubeCheckpoint.succeeded() && youtubeCheckpoint.value.has_value());
    QCOMPARE(youtubeCheckpoint.value->checkedThrough, youtubeFailure.checkedThrough);
    QCOMPARE(youtubeCheckpoint.value->lastError, youtubeFailure.error);
    QCOMPARE(news.recentNews(20).value.size(), 2);

    telegram.sourceHasSeenItems = true;
    telegram.checkedThrough = scan.scanStartedAt;
    storage::FavoriteNewsScanCommit repeated;
    repeated.scanStartedAt = QStringLiteral("2026-10-19T12:00:00.000Z");
    telegram.updateWindowStart = scan.scanStartedAt;
    repeated.sources = {telegram};
    const auto duplicate = news.commitScan(repeated);
    QVERIFY(duplicate.succeeded());
    QVERIFY(duplicate.value.isEmpty());
    QCOMPARE(news.recentNews(20).value.size(), 2);
    QCOMPARE(news.newsForScan(repeated.scanStartedAt, 20).value.size(), 0);

    storage::FavoriteNewsScanCommit canceled;
    canceled.scanStartedAt = QStringLiteral("2026-10-19T13:00:00.000Z");
    canceled.sources = {telegram};
    const auto rolledBack = news.commitScan(canceled, [] { return true; });
    QVERIFY(rolledBack.error.isCancelled());
    QCOMPARE(news.recentNews(20).value.size(), 2);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

QTEST_GUILESS_MAIN(FavoritesFeatureTest)
#include "favorites_feature_test.moc"
