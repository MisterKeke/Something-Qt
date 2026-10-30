#include "application/cancellation_token.h"
#include "application/application_service_graph.h"
#include "controllers/legacy_data_import_controller.h"
#include "storage/repositories.h"
#include "services/legacy_data_import_service.h"
#include "storage/data_import_repository.h"
#include "storage/schema_migrator.h"
#include "providers/wallpaper_image_provider.h"
#include "providers/application_icon_image_provider.h"
#include "providers/setup_icon_image_provider.h"
#include "providers/steam_artwork_image_provider.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSqlQuery>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace something;

namespace {
const QString wallpaperId = QStringLiteral("11111111111111111111111111111111");
const QString timestamp = QStringLiteral("2024-02-03T04:05:06Z");

class Database final {
public:
    Database() : name(QStringLiteral("import-fixture-") + QUuid::createUuid().toString(QUuid::Id128)),
                 db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name)) {}
    ~Database() { db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(name); }
    bool open(const QString &path) { db.setDatabaseName(path); return db.open(); }
    QString name;
    QSqlDatabase db;
};

bool exec(const QSqlDatabase &database, const QString &sql)
{
    QSqlQuery query(database);
    return query.exec(sql);
}

QVariant scalar(const QSqlDatabase &database, const QString &sql)
{
    QSqlQuery query(database);
    return query.exec(sql) && query.next() ? query.value(0) : QVariant{};
}

QMap<QString, QByteArray> sourceDigests(const QString &directory)
{
    QMap<QString, QByteArray> values;
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        QFile file(QDir(directory).filePath(QStringLiteral("database.db") + suffix));
        if (file.open(QIODevice::ReadOnly)) values.insert(suffix, QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    for (const QString &folder : {QStringLiteral("user-wallpapers"), QStringLiteral("icons"),
                                   QStringLiteral("setup-icons"), QStringLiteral("steam-game-images")}) {
        QFile file(QDir(directory).filePath(folder + QStringLiteral("/source.png")));
        if (file.open(QIODevice::ReadOnly)) values.insert(folder, QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    return values;
}

bool makeSourceFixture(const QString &directory, int sourceVersion = 24,
                        bool invalidRelationship = false, bool traversalAsset = false)
{
    if (!QDir().mkpath(directory) || !QDir().mkpath(directory + QStringLiteral("-writer"))) return false;
    QByteArray png;
    for (const QString &assetDirectory : {QStringLiteral("user-wallpapers"), QStringLiteral("icons"),
                                            QStringLiteral("setup-icons"), QStringLiteral("steam-game-images")}) {
        const QString assetRoot = QDir(directory).filePath(assetDirectory);
        if (!QDir().mkpath(assetRoot)) return false;
        const QString path = QDir(assetRoot).filePath(QStringLiteral("source.png"));
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        if (!image.save(path, "PNG")) return false;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        png = file.readAll();
    }

    Database writer;
    const QString writerPath = QDir(directory + QStringLiteral("-writer")).filePath(QStringLiteral("database.db"));
    if (!writer.open(writerPath) || !storage::SchemaMigrator::apply(writer.db)
        || !exec(writer.db, QStringLiteral("PRAGMA wal_autocheckpoint=0"))) return false;
    const QStringList changes{
        "DROP TABLE data_imports", "DROP TABLE steam_price_observations",
        "DELETE FROM schema_migrations",
        "ALTER TABLE tasks RENAME TO todos", "ALTER TABLE task_tags RENAME TO todo_tags",
        "ALTER TABLE todo_tags RENAME COLUMN task_id TO todo_id",
        "ALTER TABLE task_subtasks RENAME TO todo_subtasks",
        "ALTER TABLE todo_subtasks RENAME COLUMN task_id TO todo_id",
        "ALTER TABLE currency_favorites RENAME TO favorite_rates",
        "ALTER TABLE favorite_rates RENAME COLUMN base_code TO base",
        "ALTER TABLE favorite_rates RENAME COLUMN quote_code TO quote",
        "ALTER TABLE saved_weather RENAME TO location", "ALTER TABLE wallpapers RENAME TO user_wallpapers",
        "ALTER TABLE preferences RENAME TO app_state",
        "CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT)",
        "CREATE TABLE note_todo_connections(note_id INTEGER,todo_id INTEGER)",
        "CREATE TABLE bookmarks(id INTEGER PRIMARY KEY,url TEXT)",
        "CREATE TABLE voice_settings(id INTEGER PRIMARY KEY,model_id TEXT)",
        "INSERT INTO notes VALUES(1,'excluded note')", "INSERT INTO note_todo_connections VALUES(1,41)",
        "INSERT INTO bookmarks VALUES(1,'https://example.invalid')", "INSERT INTO voice_settings VALUES(1,'excluded')",
        "INSERT INTO todos(id,title,description,is_completed,created_at,updated_at,due_date,priority,difficulty,revision) VALUES(41,'Retained task','Description',0,'2024-02-03T04:05:06Z','2024-02-04T04:05:06Z',NULL,'high','hard',7)",
        "INSERT INTO tags(id,name,name_normalized) VALUES(9,'Work','work')",
        "INSERT INTO todo_tags VALUES(41,9)",
        "INSERT INTO todo_subtasks(id,todo_id,title,is_completed,position,created_at) VALUES(51,41,'First step',1,4,'2024-02-03T04:05:06Z')",
        "INSERT INTO favorite_rates VALUES('EUR','TRY')",
        "INSERT INTO favorite_categories(id,name,name_normalized,source,color,created_at,display_order,updated_at) VALUES(3,'News','telegram:news','telegram','#aabbcc','2024-02-03T04:05:06Z',5,'2024-02-04T04:05:06Z')",
        "INSERT INTO telegram_favorites VALUES('sample_channel','2024-02-03T04:05:06Z',3)",
        "INSERT INTO youtube_favorites VALUES('UCabcdefghijklmnopqrstuv','2024-02-03T04:05:06Z',NULL,'sample')",
        "INSERT INTO favorite_update_checkpoints VALUES('telegram','sample_channel','2024-02-03T04:05:06Z','2024-02-03T04:05:06Z','2024-02-03T04:05:06Z',NULL,'2024-02-03T04:05:06Z')",
        "INSERT INTO favorite_update_seen_items VALUES('telegram','sample_channel','7','2024-02-03T04:05:06Z','2024-02-03T04:05:06Z','2024-02-03T04:05:06Z')",
        "INSERT INTO favorite_news_items VALUES('telegram','sample_channel','7','2024-02-03T04:05:06Z','2024-02-03T04:05:06Z','{\"postId\":\"7\",\"preview\":\"Retained news\",\"images\":[]}','2024-02-03T04:05:06Z')",
        "INSERT INTO favorite_news_state VALUES(1,'2024-02-03T04:05:06Z','[]','2024-02-03T04:05:06Z')",
        "INSERT INTO desktop_apps(id,display_name,executable_path,created_at,updated_at) VALUES(71,'First','C:/Fixture/First.exe','2024-02-03T04:05:06Z','2024-02-04T04:05:06Z')",
        "INSERT INTO desktop_apps(id,display_name,executable_path,created_at,updated_at) VALUES(72,'Second','C:/Fixture/Second.exe','2024-02-03T04:05:06Z','2024-02-04T04:05:06Z')",
        "INSERT INTO setups VALUES(61,'Work','Retained description','2024-02-03T04:05:06Z','2024-02-04T04:05:06Z')",
        "INSERT INTO setup_apps VALUES(61,72,2)", "INSERT INTO setup_apps VALUES(61,71,7)",
        "UPDATE steam_game_settings SET country_code='US',updated_at='2024-02-03T04:05:06Z' WHERE id=1",
        "INSERT INTO steam_games(id,steam_app_id,store_url,name,image_source_url,price_status,currency,regular_price_minor,current_price_minor,discount_percent,price_country_code,last_checked_at,last_attempted_at,last_refresh_error,created_at,updated_at) VALUES(81,12345,'https://store.steampowered.com/app/12345/','Retained game','','priced','USD',4999,3999,20,'US','2024-02-03T04:05:06Z','2024-02-04T04:05:06Z','safe stored failure','2024-02-03T04:05:06Z','2024-02-04T04:05:06Z')",
        "INSERT INTO app_state VALUES('todos.week_start','0')",
        "INSERT INTO app_state VALUES('previous_opened_at','2024-02-03T04:05:06Z')",
        "INSERT INTO app_state VALUES('current_opened_at','2024-02-04T04:05:06Z')",
        "INSERT INTO app_state VALUES('previous_refresh_at','2024-02-03T04:05:06Z')",
        "INSERT INTO app_state VALUES('last_refresh_at','2024-02-04T04:05:06Z')",
        "INSERT INTO app_state VALUES('mcp.enabled','true')",
        "INSERT INTO app_state VALUES('voice.model','excluded')"
    };
    for (const QString &sql : changes) if (!exec(writer.db, sql)) return false;
    {
        QSqlQuery query(writer.db);
        query.prepare(QStringLiteral("INSERT INTO schema_migrations(version,name) VALUES(?,?)"));
        for (int version = 1; version <= sourceVersion; ++version) {
            query.bindValue(0, version); query.bindValue(1, QStringLiteral("fixture migration"));
            if (!query.exec()) return false;
        }
        query.prepare(QStringLiteral("INSERT INTO user_wallpapers VALUES(?,?,?,'image/png',?,?)"));
        query.addBindValue(wallpaperId); query.addBindValue(QStringLiteral("Retained wallpaper"));
        query.addBindValue(traversalAsset ? QStringLiteral("../escape.png") : QStringLiteral("source.png"));
        query.addBindValue(png.size()); query.addBindValue(timestamp);
        if (!query.exec()) return false;
        for (const auto &entry : {QPair<QString,int>{QStringLiteral("desktop_app_icons"),71},
                                  {QStringLiteral("setup_icons"),61}, {QStringLiteral("steam_game_images"),81}}) {
            query.prepare(QStringLiteral("INSERT INTO ") + entry.first + QStringLiteral(" VALUES(?,'source.png','image/png',?,?)"));
            query.addBindValue(entry.second); query.addBindValue(png.size()); query.addBindValue(timestamp);
            if (!query.exec()) return false;
        }
        query.prepare(QStringLiteral("INSERT INTO app_state VALUES('selected_wallpaper',?)"));
        query.addBindValue(QStringLiteral("custom:") + wallpaperId);
        if (!query.exec()) return false;
        const QString forecast = QStringLiteral("{\"current\":{\"temperature_2m\":10},\"daily\":{\"time\":[\"2024-02-03\"],\"weather_code\":[0],\"temperature_2m_max\":[12],\"temperature_2m_min\":[7],\"precipitation_probability_max\":[0]},\"timezone\":\"Europe/Istanbul\"}");
        query.prepare(QStringLiteral("INSERT INTO location VALUES(1,41.0,29.0,?,?,?,?)"));
        query.addBindValue(timestamp); query.addBindValue(timestamp); query.addBindValue(forecast); query.addBindValue(timestamp);
        if (!query.exec()) return false;
    }
    if (invalidRelationship) {
        if (!exec(writer.db, QStringLiteral("PRAGMA foreign_keys=OFF"))
            || !exec(writer.db, QStringLiteral("INSERT INTO todo_tags VALUES(41,999)"))) return false;
    }
    // Freeze a controlled fixture containing committed WAL data, then close
    // its only writer. Import reads this closed copy, never the writer files.
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QString from = writerPath + suffix;
        if (QFileInfo::exists(from) && !QFile::copy(from, QDir(directory).filePath(QStringLiteral("database.db") + suffix))) return false;
    }
    return true;
}

bool prepare(const QString &source, const QString &profile, QVariantMap *plan)
{
    const auto error = services::LegacyDataImportService::prepare(QDir(source).filePath(QStringLiteral("database.db")),
        profile, true, {}, plan);
    return !error.isError() && !error.isCancelled();
}

QStringList installedFiles(const QString &planPath, const QString &profile)
{
    QFile file(planPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonArray assets = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("assets")).toArray();
    QStringList files;
    for (const QJsonValue &value : assets) files.push_back(QDir(profile).filePath(value.toObject().value(QStringLiteral("targetRelative")).toString()));
    return files;
}
}

class DataImportTest final : public QObject {
    Q_OBJECT
private slots:
    void currentSchemasPreserveRetainedModelsAndSourceBytes_data();
    void currentSchemasPreserveRetainedModelsAndSourceBytes();
    void eligibilityAcceptsStartupStateAndExplainsSavedRecords();
    void builtInWallpaperSurvivesImport_data();
    void builtInWallpaperSurvivesImport();
    void controllerImportsStartupBaseline();
    void controllerPreflightDistinguishesStorageFailure();
    void editedTargetIsRejected();
    void unsupportedSchemaAndOrphanRelationshipsAreRejected();
    void interruptionsRecoverBeforeAndAfterCommit();
    void failedTransactionRestoresBaselineAndCanRetry();
    void unsafeAssetsAndMissingSelectionAreReported();
    void cancellationDoesNotCommit();
    void recoveryRetainsChangedFilesAndItsJournal();
    void malformedJournalCannotDeleteOutsideTheProfile();
};

void DataImportTest::eligibilityAcceptsStartupStateAndExplainsSavedRecords()
{
    Database target;
    QVERIFY(target.open(":memory:"));
    QVERIFY(storage::SchemaMigrator::apply(target.db));
    QVERIFY(exec(target.db, "INSERT INTO preferences VALUES('favorite_updates.current_opened_at','startup')"));
    QVERIFY(exec(target.db, "INSERT INTO favorite_news_state VALUES(1,'startup','[ ]','startup')"));
    storage::DataImportRepository repository(target.db);
    storage::DataImportStatus status;
    QVERIFY(!repository.status(&status).isError());
    QVERIFY(status.empty);
    QVERIFY(status.blockingReason.isEmpty());
    QVERIFY(exec(target.db, "UPDATE preferences SET value='0' WHERE key='calendar.game_releases_visible'"));
    QVERIFY(!repository.status(&status).isError());
    QVERIFY(!status.empty);
    QVERIFY(exec(target.db, "UPDATE preferences SET value='1' WHERE key='calendar.game_releases_visible'"));
    QVERIFY(!repository.status(&status).isError());
    QVERIFY(status.empty);
    QVERIFY(exec(target.db, "INSERT INTO tasks(title) VALUES('Keep this Qt task')"));
    QVERIFY(!repository.status(&status).isError());
    QVERIFY(!status.empty);
    QVERIFY(status.blockingReason.contains("tasks"));
    QCOMPARE(scalar(target.db, "SELECT COUNT(*) FROM tasks").toInt(), 1);
    QVERIFY(exec(target.db, "DELETE FROM tasks"));
    QVERIFY(exec(target.db, "INSERT INTO preferences VALUES('appearance.selected_wallpaper','builtin:sandrone')"));
    QVERIFY(!repository.status(&status).isError());
    QVERIFY(status.empty);
    QVERIFY(status.blockingReason.isEmpty());
    QCOMPARE(scalar(target.db, "SELECT value FROM preferences WHERE key='appearance.selected_wallpaper'").toString(), QStringLiteral("builtin:sandrone"));
    QVERIFY(exec(target.db, "UPDATE preferences SET value='custom:missing' WHERE key='appearance.selected_wallpaper'"));
    QVERIFY(!repository.status(&status).isError());
    QVERIFY(!status.empty);
    QVERIFY(status.blockingReason.contains("wallpaper"));
}

void DataImportTest::builtInWallpaperSurvivesImport_data()
{
    QTest::addColumn<QString>("selection");
    QTest::newRow("original") << QStringLiteral("builtin:original");
    QTest::newRow("sandrone") << QStringLiteral("builtin:sandrone");
    QTest::newRow("hu-tao") << QStringLiteral("builtin:hu-tao");
    QTest::newRow("skirk") << QStringLiteral("builtin:skirk");
}

void DataImportTest::builtInWallpaperSurvivesImport()
{
    QFETCH(QString, selection);
    QTemporaryDir temporary;
    const QString source = temporary.filePath("source"), profile = temporary.filePath("profile");
    QVERIFY(makeSourceFixture(source));
    const auto originalDigests = sourceDigests(source);
    QVERIFY(QDir().mkpath(profile));
    Database target;
    QVERIFY(target.open(":memory:"));
    QVERIFY(storage::SchemaMigrator::apply(target.db));
    QSqlQuery choice(target.db);
    choice.prepare("INSERT INTO preferences VALUES('appearance.selected_wallpaper',?)");
    choice.addBindValue(selection);
    QVERIFY(choice.exec());
    choice.finish();
    storage::DataImportRepository repository(target.db);
    storage::DataImportStatus status;
    QVERIFY(!repository.status(&status).isError());
    QVERIFY(status.empty);
    QVariantMap plan;
    QVERIFY(prepare(source, profile, &plan));
    const QString id = plan.value("importId").toString(), path = plan.value("planPath").toString();
    QVERIFY(!services::LegacyDataImportService::installAssets(profile, id, path, {}).isError());
    QVariantMap report;
    QVERIFY(!repository.importPlan(path, id, &report).isError());
    QCOMPARE(scalar(target.db, "SELECT value FROM preferences WHERE key='appearance.selected_wallpaper'").toString(), selection);
    QCOMPARE(scalar(target.db, "SELECT COUNT(*) FROM wallpapers").toInt(), 1);
    QCOMPARE(scalar(target.db, "SELECT COUNT(*) FROM tasks").toInt(), 1);
    const QJsonObject details = QJsonDocument::fromJson(report.value("json").toString().toUtf8()).object();
    QVERIFY(details.value("preservedTargetPreferences").toArray().contains("appearance.selected_wallpaper"));
    QCOMPARE(details.value("skippedCounts").toObject().value("source_wallpaper_selection").toInt(), 1);
    QCOMPARE(scalar(target.db, "SELECT COUNT(*) FROM preferences").toLongLong(),
             details.value("importedCounts").toObject().value("preferences").toInteger() + 1);
    QCOMPARE(sourceDigests(source), originalDigests);
    QVERIFY(!services::LegacyDataImportService::finish(profile, id).isError());
}

void DataImportTest::controllerImportsStartupBaseline()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath("source");
    QVERIFY(makeSourceFixture(source));
    const auto originalDigests = sourceDigests(source);
    application::ApplicationServiceGraph graph("default", temporary.filePath("qt"));
    controllers::LegacyDataImportController controller(&graph);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(controller.ready() && !controller.busy() && controller.canImport(), 10000);
    const auto operation = graph.beginOperation("seed startup baseline");
    QVERIFY(operation.isValid());
    QVERIFY(graph.submitStorage(operation, [](storage::StorageRepositories &repositories, const std::function<bool()> &) {
        storage::StorageReply reply;
        for (const QString &key : {QStringLiteral("previous_opened_at"), QStringLiteral("current_opened_at"),
                                  QStringLiteral("previous_refresh_at"), QStringLiteral("last_refresh_at")}) {
            reply.error = repositories.preferences.set("favorite_updates." + key, timestamp).error;
            if (reply.error.isError()) return reply;
        }
        reply.error = repositories.preferences.setSelectedWallpaper(QStringLiteral("builtin:skirk")).error;
        if (reply.error.isError()) return reply;
        reply.error = repositories.news.saveScanState({timestamp, "[]"}).error;
        return reply;
    }));
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 10000);
    controller.refreshStatus();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QVERIFY2(controller.canImport(), qPrintable(controller.errorMessage() + controller.eligibilityMessage()));
    controller.importProfile(QUrl::fromLocalFile(QDir(source).filePath("database.db")), true);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 15000);
    QVERIFY2(controller.imported(), qPrintable(controller.errorMessage() + controller.eligibilityMessage() + controller.reportJson()));
    QVERIFY(controller.restartRequired());
    QCOMPARE(sourceDigests(source), originalDigests);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 10000);
    Database imported;
    QVERIFY(imported.open(QDir(graph.profileDirectory()).filePath("something.sqlite")));
    QCOMPARE(scalar(imported.db, "SELECT value FROM preferences WHERE key='appearance.selected_wallpaper'").toString(), QStringLiteral("builtin:skirk"));
}

void DataImportTest::controllerPreflightDistinguishesStorageFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph("default", temporary.path());
    controllers::LegacyDataImportController controller(&graph);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(controller.ready() && !controller.busy() && controller.canImport(), 10000);
    const auto operation = graph.beginOperation("simulate unavailable import status table");
    QVERIFY(operation.isValid());
    QVERIFY(graph.submitStorage(operation, [](storage::StorageRepositories &, const std::function<bool()> &) {
        storage::StorageReply reply;
        // Only this disposable graph owns a something-* connection. Resolve
        // it on its storage thread; no connection handle crosses threads.
        for (const QString &name : QSqlDatabase::connectionNames()) {
            if (!name.startsWith("something-")) continue;
            QSqlQuery query(QSqlDatabase::database(name));
            if (!query.exec("DROP TABLE steam_game_settings")) reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        }
        reply.error = domain::AppError(domain::ErrorCategory::Storage);
        return reply;
    }));
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 10000);
    controller.importProfile(QUrl::fromLocalFile(temporary.filePath("not-read.db")), true);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QVERIFY(controller.errorMessage().contains("Could not check the Qt profile"));
    QVERIFY(!controller.errorMessage().contains("requires an empty"));
    QVERIFY(!controller.imported());
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 10000);
}

void DataImportTest::currentSchemasPreserveRetainedModelsAndSourceBytes_data()
{
    QTest::addColumn<int>("version");
    QTest::newRow("schema24") << 24;
    QTest::newRow("schema25") << 25;
}

void DataImportTest::currentSchemasPreserveRetainedModelsAndSourceBytes()
{
    QFETCH(int, version);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    const QString profile = temporary.filePath(QStringLiteral("profile"));
    QVERIFY(makeSourceFixture(source, version));
    QVERIFY(QDir().mkpath(profile));
    const auto digests = sourceDigests(source);
    QVERIFY(digests.contains(QStringLiteral("-wal")));
    Database target;
    QVERIFY(target.open(QDir(profile).filePath(QStringLiteral("something.sqlite"))));
    QVERIFY(storage::SchemaMigrator::apply(target.db));
    storage::DataImportRepository repository(target.db);
    QVariantMap plan;
    QVERIFY(prepare(source, profile, &plan));
    const QString id = plan.value(QStringLiteral("importId")).toString();
    const QString planPath = plan.value(QStringLiteral("planPath")).toString();
    const QStringList assets = installedFiles(planPath, profile);
    QCOMPARE(assets.size(), 4);
    QVERIFY(!services::LegacyDataImportService::installAssets(profile, id, planPath, {}).isError());
    QVariantMap report;
    QVERIFY(!repository.importPlan(planPath, id, &report).isError());
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT revision FROM tasks WHERE id=41")).toLongLong(), qint64(7));
    QVERIFY(scalar(target.db, QStringLiteral("SELECT due_date FROM tasks WHERE id=41")).isNull());
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT position FROM task_subtasks WHERE id=51")).toInt(), 4);
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT app_id FROM setup_apps WHERE setup_id=61 ORDER BY position LIMIT 1")).toLongLong(), qint64(72));
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT updated_at FROM favorite_categories WHERE id=3")).toString(), QStringLiteral("2024-02-04T04:05:06Z"));
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT current_price_minor FROM steam_games WHERE id=81")).toLongLong(), qint64(3999));
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT country_code FROM steam_game_settings")).toString(), QStringLiteral("US"));
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT value FROM preferences WHERE key='tasks.week_start'")).toString(), QStringLiteral("0"));
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT COUNT(*) FROM favorite_news_items")).toInt(), 1);
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name IN ('notes','bookmarks','voice_settings','note_todo_connections')")).toInt(), 0);
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT COUNT(*) FROM preferences WHERE key LIKE 'mcp.%' OR key LIKE 'voice.%'")).toInt(), 0);
    QVERIFY(QJsonDocument::fromJson(report.value(QStringLiteral("json")).toString().toUtf8()).object().value(QStringLiteral("committed")).toBool());
    const QString completedReport = report.value(QStringLiteral("json")).toString();
    QCOMPARE(scalar(target.db, "SELECT report_json FROM data_imports").toString(), completedReport);
    QVERIFY(!services::LegacyDataImportService::saveReport(profile, id, completedReport).isError());
    QVERIFY(QFileInfo::exists(QDir(profile).filePath(QStringLiteral("import-reports/") + id + QStringLiteral(".json"))));
    providers::WallpaperImageProvider wallpaperImages(profile);
    providers::ApplicationIconImageProvider applicationImages(profile);
    providers::SetupIconImageProvider setupImages(profile);
    providers::SteamArtworkImageProvider steamImages(profile);
    QVERIFY(!wallpaperImages.requestImage(wallpaperId, nullptr, QSize(64,64)).isNull());
    QVERIFY(!applicationImages.requestImage(scalar(target.db, "SELECT filename FROM desktop_app_icons").toString(), nullptr, QSize(64,64)).isNull());
    QVERIFY(!setupImages.requestImage(scalar(target.db, "SELECT filename FROM setup_icons").toString(), nullptr, QSize(64,64)).isNull());
    QVERIFY(!steamImages.requestImage(scalar(target.db, "SELECT filename FROM steam_game_images").toString(), nullptr, QSize(64,64)).isNull());
    QCOMPARE(repository.importPlan(planPath, id, &report).category(), domain::ErrorCategory::Conflict);
    QVERIFY(!services::LegacyDataImportService::finish(profile, id).isError());
    for (const QString &file : assets) QVERIFY(QFileInfo::exists(file));
    QCOMPARE(sourceDigests(source), digests);
}

void DataImportTest::editedTargetIsRejected()
{
    QTemporaryDir temporary;
    const QString source = temporary.filePath(QStringLiteral("source"));
    const QString profile = temporary.filePath(QStringLiteral("profile"));
    QVERIFY(makeSourceFixture(source)); QVERIFY(QDir().mkpath(profile));
    Database target; QVERIFY(target.open(QStringLiteral(":memory:"))); QVERIFY(storage::SchemaMigrator::apply(target.db));
    QVERIFY(exec(target.db, QStringLiteral("INSERT INTO tasks(title) VALUES('Qt edit')")));
    storage::DataImportRepository repository(target.db);
    QVariantMap plan; QVERIFY(prepare(source, profile, &plan));
    QVariantMap report;
    QCOMPARE(repository.importPlan(plan.value("planPath").toString(), plan.value("importId").toString(), &report).category(), domain::ErrorCategory::Conflict);
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT COUNT(*) FROM tasks")).toInt(), 1);
    QCOMPARE(scalar(target.db, QStringLiteral("SELECT COUNT(*) FROM data_imports")).toInt(), 0);
    QVERIFY(!services::LegacyDataImportService::rollback(profile, plan.value("importId").toString()).isError());
}

void DataImportTest::unsupportedSchemaAndOrphanRelationshipsAreRejected()
{
    for (const bool orphan : {false, true}) {
        QTemporaryDir temporary;
        const QString source = temporary.filePath(QStringLiteral("source"));
        const QString profile = temporary.filePath(QStringLiteral("profile"));
        QVERIFY(makeSourceFixture(source, orphan ? 24 : 23, orphan));
        QVERIFY(QDir().mkpath(profile));
        const auto before = sourceDigests(source);
        QVariantMap plan;
        QCOMPARE(services::LegacyDataImportService::prepare(QDir(source).filePath("database.db"), profile, false, {}, &plan).category(), domain::ErrorCategory::Validation);
        QCOMPARE(services::LegacyDataImportService::prepare(QDir(source).filePath("database.db"), profile, true, {}, &plan).category(), domain::ErrorCategory::Validation);
        QVERIFY(!plan.value("reportJson").toString().isEmpty());
        QCOMPARE(sourceDigests(source), before);
    }
}

void DataImportTest::interruptionsRecoverBeforeAndAfterCommit()
{
    for (const bool commit : {false, true}) {
        QTemporaryDir temporary;
        const QString source = temporary.filePath("source"), profile = temporary.filePath("profile");
        QVERIFY(makeSourceFixture(source)); QVERIFY(QDir().mkpath(profile));
        Database target; QVERIFY(target.open(":memory:")); QVERIFY(storage::SchemaMigrator::apply(target.db));
        storage::DataImportRepository repository(target.db);
        QVariantMap plan; QVERIFY(prepare(source, profile, &plan));
        const QString id = plan.value("importId").toString(), path = plan.value("planPath").toString();
        const auto assets = installedFiles(path, profile);
        QVERIFY(!services::LegacyDataImportService::installAssets(profile, id, path, {}).isError());
        if (commit) { QVariantMap report; QVERIFY(!repository.importPlan(path, id, &report).isError()); }
        bool marker = false; QVERIFY(!repository.hasCompletion(id, &marker).isError()); QCOMPARE(marker, commit);
        QVERIFY(!services::LegacyDataImportService::recover(profile, id, marker).isError());
        QVERIFY(!QFileInfo::exists(QDir(profile).filePath(".legacy-import-journal.json")));
        for (const QString &asset : assets) QCOMPARE(QFileInfo::exists(asset), commit);
        QVERIFY(!services::LegacyDataImportService::recover(profile, id, marker).isError());
    }
}

void DataImportTest::failedTransactionRestoresBaselineAndCanRetry()
{
    QTemporaryDir temporary;
    const QString source = temporary.filePath("source"), profile = temporary.filePath("profile");
    QVERIFY(makeSourceFixture(source)); QVERIFY(QDir().mkpath(profile));
    Database target; QVERIFY(target.open(":memory:")); QVERIFY(storage::SchemaMigrator::apply(target.db));
    QVERIFY(exec(target.db, "INSERT INTO preferences VALUES('favorite_updates.current_opened_at','baseline')"));
    QVERIFY(exec(target.db, "INSERT INTO preferences VALUES('appearance.selected_wallpaper','builtin:sandrone')"));
    QVERIFY(exec(target.db, "CREATE TRIGGER reject_import BEFORE INSERT ON tasks BEGIN SELECT RAISE(ABORT,'fixture failure'); END"));
    storage::DataImportRepository repository(target.db);
    QVariantMap plan; QVERIFY(prepare(source, profile, &plan));
    const QString id = plan.value("importId").toString(), path = plan.value("planPath").toString();
    const auto assets = installedFiles(path, profile);
    QVERIFY(!services::LegacyDataImportService::installAssets(profile, id, path, {}).isError());
    QVariantMap report; QVERIFY(repository.importPlan(path, id, &report).isError());
    QVERIFY(!QJsonDocument::fromJson(report.value("json").toString().toUtf8()).object().value("committed").toBool());
    QVERIFY(!services::LegacyDataImportService::saveReport(profile, id, report.value("json").toString()).isError());
    QCOMPARE(scalar(target.db, "SELECT COUNT(*) FROM tasks").toInt(), 0);
    QCOMPARE(scalar(target.db, "SELECT value FROM preferences WHERE key='favorite_updates.current_opened_at'").toString(), QStringLiteral("baseline"));
    QCOMPARE(scalar(target.db, "SELECT value FROM preferences WHERE key='appearance.selected_wallpaper'").toString(), QStringLiteral("builtin:sandrone"));
    QCOMPARE(scalar(target.db, "SELECT COUNT(*) FROM data_imports").toInt(), 0);
    QVERIFY(!services::LegacyDataImportService::recover(profile, id, false).isError());
    for (const QString &asset : assets) QVERIFY(!QFileInfo::exists(asset));
    storage::DataImportStatus status; QVERIFY(!repository.status(&status).isError()); QVERIFY(status.empty);
}

void DataImportTest::unsafeAssetsAndMissingSelectionAreReported()
{
    QTemporaryDir temporary;
    const QString source = temporary.filePath("source"), profile = temporary.filePath("profile");
    QVERIFY(makeSourceFixture(source, 24, false, true)); QVERIFY(QDir().mkpath(profile));
    QVERIFY(QFile::remove(QDir(source).filePath("icons/source.png")));
    QVariantMap plan; QVERIFY(prepare(source, profile, &plan));
    const QJsonObject report = QJsonDocument::fromJson(plan.value("reportJson").toString().toUtf8()).object();
    QCOMPARE(report.value("assetFailures").toArray().size(), 1);
    QCOMPARE(report.value("missingAssets").toArray().size(), 1);
    QCOMPARE(report.value("skippedCounts").toObject().value("selected_wallpaper").toInt(), 1);
    QVERIFY(!report.value("browserStorageImported").toBool());
    QVERIFY(!services::LegacyDataImportService::rollback(profile, plan.value("importId").toString()).isError());
}

void DataImportTest::recoveryRetainsChangedFilesAndItsJournal()
{
    QTemporaryDir temporary;
    const QString source = temporary.filePath("source"), profile = temporary.filePath("profile");
    QVERIFY(makeSourceFixture(source)); QVERIFY(QDir().mkpath(profile));
    QVariantMap plan; QVERIFY(prepare(source, profile, &plan));
    const QString id = plan.value("importId").toString(), path = plan.value("planPath").toString();
    const auto assets = installedFiles(path, profile);
    QVERIFY(!assets.isEmpty());
    QVERIFY(!services::LegacyDataImportService::installAssets(profile, id, path, {}).isError());
    QFile changed(assets.first()); QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(changed.write("changed"), qint64(7)); changed.close();
    QCOMPARE(services::LegacyDataImportService::recover(profile, id, false).category(), domain::ErrorCategory::Storage);
    QVERIFY(QFileInfo::exists(assets.first()));
    QVERIFY(QFileInfo::exists(QDir(profile).filePath(".legacy-import-journal.json")));
}

void DataImportTest::malformedJournalCannotDeleteOutsideTheProfile()
{
    QTemporaryDir temporary;
    const QString profile = temporary.filePath("profile");
    QVERIFY(QDir().mkpath(profile));
    QFile outside(temporary.filePath("outside.png"));
    QVERIFY(outside.open(QIODevice::WriteOnly)); QCOMPARE(outside.write("keep"), qint64(4)); outside.close();
    const QString id(32, QLatin1Char('a'));
    const QJsonObject asset{{"kind","wallpaper"}, {"stageRelative",QStringLiteral(".legacy-import-staging/") + id + "/assets/" + id + ".png"},
                            {"targetRelative","../outside.png"}, {"byteSize","4"}, {"sha256",QString(64, QLatin1Char('a'))}};
    QFile journal(QDir(profile).filePath(".legacy-import-journal.json"));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    const QByteArray body = QJsonDocument(QJsonObject{{"importId",id},{"assets",QJsonArray{asset}}}).toJson();
    QCOMPARE(journal.write(body), qint64(body.size())); journal.close();
    QCOMPARE(services::LegacyDataImportService::recover(profile, id, false).category(), domain::ErrorCategory::Validation);
    QVERIFY(outside.open(QIODevice::ReadOnly)); QCOMPARE(outside.readAll(), QByteArrayLiteral("keep"));
    QVERIFY(journal.exists());
}

void DataImportTest::cancellationDoesNotCommit()
{
    QTemporaryDir temporary;
    const QString source = temporary.filePath("source"), profile = temporary.filePath("profile");
    QVERIFY(makeSourceFixture(source)); QVERIFY(QDir().mkpath(profile));
    application::CancellationSource cancellation; cancellation.requestCancellation();
    QVariantMap plan;
    QVERIFY(services::LegacyDataImportService::prepare(QDir(source).filePath("database.db"), profile, true, cancellation.token(), &plan).isCancelled());
    Database target; QVERIFY(target.open(":memory:")); QVERIFY(storage::SchemaMigrator::apply(target.db));
    storage::DataImportRepository repository(target.db);
    QVERIFY(prepare(source, profile, &plan));
    QVariantMap report;
    QVERIFY(repository.importPlan(plan.value("planPath").toString(), plan.value("importId").toString(), &report, [] { return true; }).isCancelled());
    QCOMPARE(scalar(target.db, "SELECT COUNT(*) FROM data_imports").toInt(), 0);
    QVERIFY(!services::LegacyDataImportService::rollback(profile, plan.value("importId").toString()).isError());
}

QTEST_GUILESS_MAIN(DataImportTest)
#include "data_import_test.moc"
