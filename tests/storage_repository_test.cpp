#include "storage/repositories.h"
#include "storage/schema_migrator.h"
#include "services/preferences_service.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDate>
#include <QThread>
#include <QUuid>
#include <QtTest>

#include <limits>

using namespace something;

class StorageRepositoryTest final : public QObject {
    Q_OBJECT

private slots:
    void migrationsAreTransactionalIndependentAndRepeatable();
    void failedMigrationLeavesNoPartialVersion();
    void migratorRejectsUntrackedAndUnknownHistories();
    void taskAggregateUpdateIsAtomicAndRevisionChecked();
    void taskSearchFiltersPagesAndOmittedMetadata();
    void categoryDeleteMovesAssignmentsWithinOneSource();
    void appOpenUpdatesFavoriteWindowsAndPrunesAtomically();
    void setupMembershipPreservesOrderAndEnforcesReferences();
    void desktopApplicationPathsAreCaseInsensitive();
    void invalidWeekStartIsRejectedAndDefaultsToMonday();
    void preferencesServiceCommandsValidateAndRoundTripWeekStart();
};

static QStringList tableNames(const QSqlDatabase &database)
{
    QSqlQuery query(database);
    query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"));
    QStringList names;
    while (query.next()) names.push_back(query.value(0).toString());
    return names;
}

static bool openTestDatabase(QSqlDatabase *database, QString *connectionName)
{
    *connectionName = QStringLiteral("storage-test-%1")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    database->setDatabaseName(QStringLiteral(":memory:"));
    return database->open() && storage::SchemaMigrator::apply(*database);
}

void StorageRepositoryTest::migrationsAreTransactionalIndependentAndRepeatable()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 8);
    QVERIFY(storage::SchemaMigrator::apply(database));

    QSqlQuery history(database);
    QVERIFY(history.exec(QStringLiteral("SELECT COUNT(*),MIN(version),MAX(version) FROM schema_migrations")));
    QVERIFY(history.next());
    QCOMPARE(history.value(0).toInt(), 8);
    QCOMPARE(history.value(1).toInt(), 1);
    QCOMPARE(history.value(2).toInt(), 8);

    const QStringList tables = tableNames(database);
    for (const QString &retained : {QStringLiteral("tasks"), QStringLiteral("task_tags"),
                                    QStringLiteral("task_subtasks"), QStringLiteral("currency_favorites"),
                                    QStringLiteral("favorite_categories"), QStringLiteral("favorite_news_items"),
                                    QStringLiteral("favorite_update_checkpoints"), QStringLiteral("saved_weather"),
                                    QStringLiteral("desktop_apps"), QStringLiteral("setups"),
                                    QStringLiteral("steam_games"), QStringLiteral("wallpapers"),
                                    QStringLiteral("preferences")}) {
        QVERIFY2(tables.contains(retained), qPrintable(retained));
    }
    for (const QString &excluded : {QStringLiteral("notes"), QStringLiteral("bookmarks"),
                                    QStringLiteral("git_repositories"), QStringLiteral("voice_state"),
                                    QStringLiteral("utilities"), QStringLiteral("website_search")}) {
        QVERIFY2(!tables.contains(excluded), qPrintable(excluded));
    }

    history.finish();
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::failedMigrationLeavesNoPartialVersion()
{
    const QString connectionName = QStringLiteral("storage-migration-rollback-%1")
                                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    QSqlQuery setup(database);
    QVERIFY(setup.exec(QStringLiteral("CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)")));
    QVERIFY(setup.exec(QStringLiteral("CREATE TRIGGER reject_migration BEFORE INSERT ON schema_migrations BEGIN SELECT RAISE(ABORT, 'fixture failure'); END")));

    QVERIFY(!storage::SchemaMigrator::apply(database));
    QVERIFY(setup.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='tasks'")));
    QVERIFY(!setup.next()); // migration 1 DDL rolled back with its rejected ledger write
    QVERIFY(setup.exec(QStringLiteral("SELECT COUNT(*) FROM schema_migrations")));
    QVERIFY(setup.next());
    QCOMPARE(setup.value(0).toInt(), 0);

    setup.finish();
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::migratorRejectsUntrackedAndUnknownHistories()
{
    const QString untrackedName = QStringLiteral("storage-untracked-%1")
                                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase untracked = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), untrackedName);
    untracked.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(untracked.open());
    QSqlQuery untrackedSetup(untracked);
    QVERIFY(untrackedSetup.exec(QStringLiteral("CREATE TABLE untracked_fixture(value TEXT)")));
    QVERIFY(!storage::SchemaMigrator::apply(untracked));
    QVERIFY(untrackedSetup.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE name='schema_migrations'")));
    QVERIFY(!untrackedSetup.next());
    untrackedSetup.finish();
    untracked.close();
    untracked = QSqlDatabase();
    QSqlDatabase::removeDatabase(untrackedName);

    const QString unknownName = QStringLiteral("storage-unknown-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase unknown = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), unknownName);
    unknown.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(unknown.open());
    QSqlQuery unknownSetup(unknown);
    QVERIFY(unknownSetup.exec(QStringLiteral("CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)")));
    QVERIFY(unknownSetup.exec(QStringLiteral("INSERT INTO schema_migrations(version,name) VALUES(9,'future')")));
    QVERIFY(!storage::SchemaMigrator::apply(unknown));
    unknownSetup.finish();
    unknown.close();
    unknown = QSqlDatabase();
    QSqlDatabase::removeDatabase(unknownName);
}

void StorageRepositoryTest::taskAggregateUpdateIsAtomicAndRevisionChecked()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::StorageRepositories repositories(database);

    storage::TaskInput input;
    input.title = QStringLiteral("  Parent task  ");
    input.difficulty = QStringLiteral("hard");
    input.tags = QStringList{QStringLiteral("Planning"), QStringLiteral(" planning ")};
    input.subtasks = QVector<storage::SubtaskInput>{{0, QStringLiteral("First step"), false}};
    const auto created = repositories.tasks.create(input);
    QVERIFY(created.succeeded());
    QCOMPARE(created.value.title, QStringLiteral("Parent task"));
    QCOMPARE(created.value.revision, qint64(1));
    QCOMPARE(created.value.tags, QStringList{QStringLiteral("Planning")});
    QCOMPARE(created.value.subtasks.size(), 1);

    storage::TaskInput invalidUpdate = input;
    invalidUpdate.title = QStringLiteral("Should roll back");
    invalidUpdate.tags = QStringList{QStringLiteral("Changed")};
    invalidUpdate.subtasks = QVector<storage::SubtaskInput>{{999999, QStringLiteral("Unknown id"), false}};
    const auto rejected = repositories.tasks.update(created.value.id, 1, invalidUpdate);
    QVERIFY(!rejected.succeeded());
    QCOMPARE(rejected.error.category(), domain::ErrorCategory::Conflict);

    const auto afterRollback = repositories.tasks.get(created.value.id);
    QVERIFY(afterRollback.succeeded());
    QCOMPARE(afterRollback.value.title, QStringLiteral("Parent task"));
    QCOMPARE(afterRollback.value.revision, qint64(1));
    QCOMPARE(afterRollback.value.tags, QStringList{QStringLiteral("Planning")});
    QCOMPARE(afterRollback.value.subtasks.size(), 1);

    const auto completed = repositories.tasks.setSubtaskCompleted(created.value.id,
        created.value.subtasks.front().id, 1, true);
    QVERIFY(completed.succeeded());
    QCOMPARE(completed.value.revision, qint64(2));
    QVERIFY(completed.value.subtasks.front().completed);
    QCOMPARE(repositories.tasks.setCompleted(created.value.id, 1, true).error.category(),
             domain::ErrorCategory::Conflict);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::taskSearchFiltersPagesAndOmittedMetadata()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::StorageRepositories repositories(database);

    storage::TaskInput hard;
    hard.title = QStringLiteral("Release plan");
    hard.description = QStringLiteral("ship the backend safely");
    hard.dueDate = QDate::currentDate().addDays(-1).toString(Qt::ISODate);
    hard.priority = QStringLiteral("high");
    hard.difficulty = QStringLiteral("hard");
    hard.tags = QStringList{QStringLiteral("Work"), QStringLiteral("Urgent")};
    hard.subtasks = QVector<storage::SubtaskInput>{{0, QStringLiteral("Provision server"), false}};
    const auto created = repositories.tasks.create(hard);
    QVERIFY(created.succeeded());

    storage::TaskInput second;
    second.title = QStringLiteral("100%_ready");
    second.priority = QStringLiteral("low");
    second.difficulty = QStringLiteral("easy");
    second.tags = QStringList{QStringLiteral("Work")};
    const auto secondCreated = repositories.tasks.create(second);
    QVERIFY(secondCreated.succeeded());

    storage::TaskFilter query;
    query.query = QStringLiteral("server");
    const auto subtaskSearch = repositories.tasks.list(query);
    QVERIFY(subtaskSearch.succeeded());
    QCOMPARE(subtaskSearch.value.size(), 1);
    QCOMPARE(subtaskSearch.value.front().id, created.value.id);

    query.query = QStringLiteral("safely");
    const auto descriptionSearch = repositories.tasks.list(query);
    QVERIFY(descriptionSearch.succeeded());
    QCOMPARE(descriptionSearch.value.size(), 1);
    QCOMPARE(descriptionSearch.value.front().id, created.value.id);

    query.query = QStringLiteral("urgent");
    const auto tagSearch = repositories.tasks.list(query);
    QVERIFY(tagSearch.succeeded());
    QCOMPARE(tagSearch.value.size(), 1);
    QCOMPARE(tagSearch.value.front().id, created.value.id);

    query = {};
    query.query = QStringLiteral("%_");
    const auto literalWildcard = repositories.tasks.list(query);
    QVERIFY(literalWildcard.succeeded());
    QCOMPARE(literalWildcard.value.size(), 1);
    QCOMPARE(literalWildcard.value.front().id, secondCreated.value.id);

    query = {};
    query.tags = {QStringLiteral("work"), QStringLiteral("URGENT")};
    const auto andTags = repositories.tasks.list(query);
    QVERIFY(andTags.succeeded());
    QCOMPARE(andTags.value.size(), 1);
    QCOMPARE(andTags.value.front().id, created.value.id);

    query = {};
    query.overdue = true;
    query.completion = QStringLiteral("incomplete");
    const auto overdue = repositories.tasks.list(query);
    QVERIFY(overdue.succeeded());
    QCOMPARE(overdue.value.size(), 1);
    QCOMPARE(overdue.value.front().id, created.value.id);

    const auto completed = repositories.tasks.setCompleted(created.value.id,
                                                           created.value.revision, true);
    QVERIFY(completed.succeeded());
    query = {};
    query.completion = QStringLiteral("complete");
    query.dueDate = hard.dueDate;
    const auto completionFilter = repositories.tasks.list(query);
    QVERIFY(completionFilter.succeeded());
    QCOMPARE(completionFilter.value.size(), 1);
    QCOMPARE(completionFilter.value.front().id, created.value.id);

    query = {};
    query.dueDate = QDate::currentDate().addDays(-1).toString(Qt::ISODate);
    query.completion = QStringLiteral("all");
    QCOMPARE(repositories.tasks.list(query).value.size(), 1);

    storage::TaskInput metadataOnlyUpdate;
    metadataOnlyUpdate.title = QStringLiteral("Renamed safely");
    metadataOnlyUpdate.dueDate = hard.dueDate;
    metadataOnlyUpdate.priority = hard.priority;
    const auto metadataPreserved = repositories.tasks.update(
        created.value.id, completed.value.revision, metadataOnlyUpdate);
    QVERIFY(metadataPreserved.succeeded());
    QCOMPARE(metadataPreserved.value.difficulty, QStringLiteral("hard"));
    QCOMPARE(metadataPreserved.value.tags, QStringList({QStringLiteral("Work"), QStringLiteral("Urgent")}));
    QCOMPARE(metadataPreserved.value.subtasks.size(), 1);
    QCOMPARE(metadataPreserved.value.dueDate, hard.dueDate);
    QCOMPARE(metadataPreserved.value.priority, QStringLiteral("high"));

    storage::TaskInput replaceSubtaskOnly;
    replaceSubtaskOnly.title = metadataPreserved.value.title;
    replaceSubtaskOnly.dueDate = metadataPreserved.value.dueDate;
    replaceSubtaskOnly.priority = metadataPreserved.value.priority;
    replaceSubtaskOnly.subtasks = QVector<storage::SubtaskInput>{
        {metadataPreserved.value.subtasks.front().id, QStringLiteral("Provision safely"), false}};
    const auto updatedSubtask = repositories.tasks.update(
        created.value.id, metadataPreserved.value.revision, replaceSubtaskOnly);
    QVERIFY(updatedSubtask.succeeded());
    QCOMPARE(updatedSubtask.value.difficulty, QStringLiteral("hard"));
    QCOMPARE(updatedSubtask.value.subtasks.front().title, QStringLiteral("Provision safely"));

    storage::TaskFilter paging;
    paging.sort = QStringLiteral("id");
    paging.direction = QStringLiteral("asc");
    paging.limit = 1;
    paging.offset = 1;
    const auto secondPage = repositories.tasks.list(paging);
    QVERIFY(secondPage.succeeded());
    QCOMPARE(secondPage.value.size(), 1);
    QCOMPARE(secondPage.value.front().id, secondCreated.value.id);

    storage::TaskFilter priorityOrder;
    priorityOrder.sort = QStringLiteral("priority");
    priorityOrder.direction = QStringLiteral("asc");
    priorityOrder.limit = 10;
    const auto ordered = repositories.tasks.list(priorityOrder);
    QVERIFY(ordered.succeeded());
    QCOMPARE(ordered.value.at(0).id, created.value.id); // high sorts ahead of low
    QCOMPARE(ordered.value.at(1).id, secondCreated.value.id);

    storage::TaskInput leaveHard;
    leaveHard.title = QStringLiteral("Change difficulty");
    leaveHard.dueDate = hard.dueDate;
    leaveHard.priority = hard.priority;
    leaveHard.difficulty = QStringLiteral("easy");
    QCOMPARE(repositories.tasks.update(created.value.id, updatedSubtask.value.revision,
                                       leaveHard).error.category(),
             domain::ErrorCategory::Validation);
    const auto unchanged = repositories.tasks.get(created.value.id);
    QCOMPARE(unchanged.value.difficulty, QStringLiteral("hard"));
    QCOMPARE(unchanged.value.subtasks.size(), 1);

    leaveHard.subtasks = QVector<storage::SubtaskInput>{};
    const auto cleared = repositories.tasks.update(created.value.id,
                                                   updatedSubtask.value.revision, leaveHard);
    QVERIFY(cleared.succeeded());
    QCOMPARE(cleared.value.difficulty, QStringLiteral("easy"));
    QVERIFY(cleared.value.subtasks.isEmpty());

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::categoryDeleteMovesAssignmentsWithinOneSource()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::FavoritesRepository favorites(database);
    const auto first = favorites.createCategory(QStringLiteral("telegram"), QStringLiteral("First"), QStringLiteral("#abc"));
    const auto second = favorites.createCategory(QStringLiteral("telegram"), QStringLiteral("Second"), QStringLiteral("#AABBCC"));
    QVERIFY(first.succeeded());
    QVERIFY(second.succeeded());
    QCOMPARE(first.value.color, QStringLiteral("#aabbcc"));
    QVERIFY(favorites.addFavorite(QStringLiteral("telegram"), QStringLiteral("sample_channel"), {}).succeeded());
    QVERIFY(favorites.assignFavoriteCategory(QStringLiteral("telegram"),
                                             QStringLiteral("sample_channel"),
                                             first.value.id).succeeded());

    const auto removed = favorites.removeCategory(first.value.id, QStringLiteral("move"), second.value.id);
    QVERIFY(removed.succeeded());
    QCOMPARE(removed.value.affectedFavorites, 1);
    const auto rows = favorites.favorites(QStringLiteral("telegram"));
    QVERIFY(rows.succeeded());
    QCOMPARE(rows.value.size(), 1);
    QVERIFY(rows.value.front().categoryId.has_value());
    QCOMPARE(*rows.value.front().categoryId, second.value.id);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::appOpenUpdatesFavoriteWindowsAndPrunesAtomically()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::FavoriteNewsRepository news(database);
    storage::PreferencesRepository preferences(database);
    const QVector<storage::FavoriteNewsItem> items{
        {QStringLiteral("telegram"), QStringLiteral("old"), QStringLiteral("old-item"),
         QStringLiteral("2026-09-17T00:00:00Z"), QStringLiteral("2026-09-17T01:00:00Z"), QStringLiteral("{}")},
        {QStringLiteral("youtube"), QStringLiteral("new"), QStringLiteral("new-item"),
         QStringLiteral("2026-10-17T00:00:00Z"), QStringLiteral("2026-10-17T01:00:00Z"), QStringLiteral("{}")}
    };
    QVERIFY(news.saveNews(items).succeeded());
    QVERIFY(news.saveScanState({QStringLiteral("2026-10-18T11:00:00Z"), QStringLiteral("[]")}).succeeded());
    QVERIFY(preferences.set(QStringLiteral("favorite_updates.current_opened_at"),
                            QStringLiteral("2026-10-17T12:00:00Z")).succeeded());
    QVERIFY(news.recordAppOpen(QStringLiteral("2026-10-18T12:00:00Z"),
                               QStringLiteral("2026-09-18T12:00:00Z")).succeeded());

    const auto recent = news.recentNews(10);
    QVERIFY(recent.succeeded());
    QCOMPARE(recent.value.size(), 1);
    QCOMPARE(recent.value.front().itemId, QStringLiteral("new-item"));
    QVERIFY(!news.scanState().value.has_value());
    QCOMPARE(preferences.get(QStringLiteral("favorite_updates.previous_opened_at")).value,
             QStringLiteral("2026-10-17T12:00:00Z"));
    QVERIFY(news.recordAppOpen(QStringLiteral("2026-10-19T12:00:00Z"),
                               QStringLiteral("2026-09-19T12:00:00Z")).succeeded());
    QCOMPARE(preferences.get(QStringLiteral("favorite_updates.previous_opened_at")).value,
             QStringLiteral("2026-10-18T12:00:00Z"));

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::setupMembershipPreservesOrderAndEnforcesReferences()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::StorageRepositories repositories(database);
    const auto first = repositories.applications.add(QStringLiteral("First"), QStringLiteral("C:/first.exe"));
    const auto second = repositories.applications.add(QStringLiteral("Second"), QStringLiteral("C:/second.exe"));
    QVERIFY(first.succeeded());
    QVERIFY(second.succeeded());

    storage::Setup setup;
    setup.name = QStringLiteral("Morning");
    setup.description = QStringLiteral("Work applications");
    setup.applicationIds = {second.value.id, first.value.id};
    setup.icon = storage::AssetMetadata{
        QStringLiteral("0123456789abcdef0123456789abcdef.png"),
        QStringLiteral("image/png"), 1};
    const auto saved = repositories.setups.save(setup);
    QVERIFY(saved.succeeded());
    QCOMPARE(saved.value.applicationIds, QVector<qint64>({second.value.id, first.value.id}));
    QCOMPARE(saved.value.icon->filename, setup.icon->filename);
    storage::Setup duplicateName;
    duplicateName.name = QStringLiteral("morning");
    duplicateName.applicationIds = {first.value.id};
    QCOMPARE(repositories.setups.save(duplicateName).error.category(), domain::ErrorCategory::Conflict);
    duplicateName.name = QStringLiteral("MAÑANA");
    storage::Setup unicodeName;
    unicodeName.name = QStringLiteral("Mañana");
    unicodeName.applicationIds = {first.value.id};
    QVERIFY(repositories.setups.save(unicodeName).succeeded());
    QCOMPARE(repositories.setups.save(duplicateName).error.category(), domain::ErrorCategory::Conflict);

    storage::Setup edit = saved.value;
    edit.name = QStringLiteral("Morning revised");
    edit.description = QStringLiteral("Updated text");
    edit.applicationIds = {first.value.id, second.value.id};
    edit.icon.reset(); // An omitted icon edit preserves the stored asset.
    const auto updatedSetup = repositories.setups.save(edit);
    QVERIFY(updatedSetup.succeeded());
    QCOMPARE(updatedSetup.value.applicationIds, QVector<qint64>({first.value.id, second.value.id}));
    QCOMPARE(updatedSetup.value.icon->filename, setup.icon->filename);

    storage::Setup invalid;
    invalid.name = QStringLiteral("Broken");
    invalid.applicationIds = {first.value.id, first.value.id};
    QCOMPARE(repositories.setups.save(invalid).error.category(), domain::ErrorCategory::Validation);
    invalid.name = QStringLiteral("Missing member");
    invalid.applicationIds = {std::numeric_limits<qint64>::max()};
    QCOMPARE(repositories.setups.save(invalid).error.category(), domain::ErrorCategory::Validation);
    const auto afterInvalidSave = repositories.setups.list();
    QVERIFY(afterInvalidSave.succeeded());
    QCOMPARE(afterInvalidSave.value.size(), 2);

    QVERIFY(repositories.applications.remove(second.value.id).succeeded());
    const auto updated = repositories.setups.get(saved.value.id);
    QVERIFY(updated.succeeded());
    QCOMPARE(updated.value.applicationIds, QVector<qint64>({first.value.id}));
    QVERIFY(repositories.setups.remove(updated.value.id).succeeded());
    QVERIFY(repositories.applications.get(first.value.id).succeeded());

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::desktopApplicationPathsAreCaseInsensitive()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::DesktopApplicationsRepository applications(database);
    const auto first = applications.add(QStringLiteral("Editor"), QStringLiteral("C:/Apps/Éditeur.exe"));
    QVERIFY(first.succeeded());
    QCOMPARE(applications.add(QStringLiteral("Duplicate"), QStringLiteral("c:/apps/ÉDITEUR.exe"))
                 .error.category(), domain::ErrorCategory::Conflict);

    const auto second = applications.add(QStringLiteral("Browser"), QStringLiteral("C:/Apps/Browser.exe"));
    QVERIFY(second.succeeded());
    QCOMPARE(applications.update(first.value.id, QStringLiteral("Editor"),
                                 QStringLiteral("c:/APPS/browser.exe"))
                 .error.category(), domain::ErrorCategory::Conflict);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::invalidWeekStartIsRejectedAndDefaultsToMonday()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::PreferencesRepository preferences(database);
    QCOMPARE(preferences.weekStart().value, 1);
    QCOMPARE(preferences.setWeekStart(7).error.category(), domain::ErrorCategory::Validation);
    QVERIFY(preferences.setWeekStart(0).succeeded());
    QCOMPARE(preferences.weekStart().value, 0);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void StorageRepositoryTest::preferencesServiceCommandsValidateAndRoundTripWeekStart()
{
    QString connectionName;
    QSqlDatabase database;
    QVERIFY(openTestDatabase(&database, &connectionName));
    storage::StorageRepositories repositories(database);
    const auto notCancelled = [] { return false; };
    const auto cancelled = [] { return true; };

    auto reply = services::PreferencesService::loadWeekStart()(repositories, notCancelled);
    QVERIFY(!reply.error.isError());
    QCOMPARE(reply.values.value(QStringLiteral("weekStart")).toInt(), 1);

    reply = services::PreferencesService::setWeekStart(0)(repositories, notCancelled);
    QVERIFY(!reply.error.isError());
    QCOMPARE(reply.values.value(QStringLiteral("weekStart")).toInt(), 0);

    reply = services::PreferencesService::loadWeekStart()(repositories, notCancelled);
    QCOMPARE(reply.values.value(QStringLiteral("weekStart")).toInt(), 0);

    reply = services::PreferencesService::setWeekStart(7)(repositories, notCancelled);
    QCOMPARE(reply.error.category(), domain::ErrorCategory::Validation);
    reply = services::PreferencesService::setWeekStart(2)(repositories, cancelled);
    QVERIFY(reply.error.isCancelled());
    QCOMPARE(repositories.preferences.weekStart().value, 0);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

QTEST_GUILESS_MAIN(StorageRepositoryTest)
#include "storage_repository_test.moc"
