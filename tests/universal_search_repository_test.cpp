#include "domain/app_error.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

using namespace something;

class UniversalSearchRepositoryTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void searchesTasksApplicationsAndFavorites();
    void searchesAutomationRulesWithoutExecutingThem();
    void searchesUnicodeCaseVariantsThroughSqlPrefilter();
    void boundsLargeResultSetsAndHonorsCancellation();
    void rejectsEmptyAndMalformedQueries();

private:
    QSqlDatabase m_database;
    QString m_connectionName;
};

void UniversalSearchRepositoryTest::init()
{
    m_connectionName = QStringLiteral("universal-search-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(m_database.open());
    QVERIFY(storage::SchemaMigrator::apply(m_database));

    QSqlQuery query(m_database);
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO tasks(title,description,priority,due_date,revision) "
        "VALUES('Café launch plan','Prepare a release checklist','high','2026-11-02',4)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO tags(name,name_normalized) VALUES('Urgent','urgent')")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO task_tags(task_id,tag_id) VALUES(1,1)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO application_categories(name,name_normalized,description,display_order) "
        "VALUES('Design','design','Creative work tools',0)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO desktop_apps(display_name,executable_path,category_id) "
        "VALUES('Discord','C:/Apps/discord.exe',1)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO setups(name,description) VALUES('Writing suite','Tools for writing')")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO setup_apps(setup_id,app_id,position) VALUES(1,1,0)")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO favorite_categories(name,name_normalized,source) "
        "VALUES('Live News','live news','telegram')")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO telegram_favorites(username,category_id) VALUES('daily_cafe',1)")));
}

void UniversalSearchRepositoryTest::cleanup()
{
    m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
    m_connectionName.clear();
}

void UniversalSearchRepositoryTest::searchesTasksApplicationsAndFavorites()
{
    storage::UniversalSearchRepository repository(m_database);

    const auto task = repository.search(QStringLiteral("café urgent"));
    QVERIFY(task.succeeded());
    QCOMPARE(task.value.size(), 1);
    const QVariantMap taskResult = task.value.first().toMap();
    QCOMPARE(taskResult.value(QStringLiteral("type")).toString(), QStringLiteral("task"));
    QCOMPARE(taskResult.value(QStringLiteral("title")).toString(), QStringLiteral("Café launch plan"));
    QCOMPARE(taskResult.value(QStringLiteral("subtitle")).toString(),
             QStringLiteral("Incomplete · High priority · Due 2026-11-02"));
    const QVariantMap taskPayload = taskResult.value(QStringLiteral("payload")).toMap();
    QCOMPARE(taskPayload.value(QStringLiteral("revision")).toLongLong(), 4);
    QCOMPARE(taskPayload.value(QStringLiteral("tags")).toList().first().toString(), QStringLiteral("Urgent"));
    QCOMPARE(taskResult.value(QStringLiteral("actions")).toList().size(), 2);

    const auto savedApp = repository.search(QStringLiteral("DISCORD"));
    QVERIFY(savedApp.succeeded());
    QCOMPARE(savedApp.value.size(), 1);
    const QVariantMap application = savedApp.value.first().toMap();
    QCOMPARE(application.value(QStringLiteral("type")).toString(), QStringLiteral("desktopApplication"));
    QVERIFY(application.value(QStringLiteral("score")).toInt() > 9000);
    QCOMPARE(application.value(QStringLiteral("payload")).toMap()
                 .value(QStringLiteral("categoryName")).toString(), QStringLiteral("Design"));

    const auto setup = repository.search(QStringLiteral("writing"));
    QVERIFY(setup.succeeded());
    QCOMPARE(setup.value.size(), 1);
    const QVariantMap setupResult = setup.value.first().toMap();
    QCOMPARE(setupResult.value(QStringLiteral("title")).toString(), QStringLiteral("Writing suite"));
    QCOMPARE(setupResult.value(QStringLiteral("payload")).toMap()
                 .value(QStringLiteral("applicationIds")).toList().size(), 1);

    const auto channel = repository.search(QStringLiteral("daily_cafe"));
    QVERIFY(channel.succeeded());
    QCOMPARE(channel.value.size(), 1);
    QCOMPARE(channel.value.first().toMap().value(QStringLiteral("type")).toString(),
             QStringLiteral("telegramFavorite"));
}

void UniversalSearchRepositoryTest::searchesAutomationRulesWithoutExecutingThem()
{
    storage::AutomationRepository automations(m_database);
    domain::AutomationRule rule;
    rule.name = QStringLiteral("Weekday focus reminder");
    rule.description = QStringLiteral("Start the writing setup on weekdays");
    rule.triggerType = QStringLiteral("schedule.time_reached");
    rule.triggerConfig = {{QStringLiteral("time"), QStringLiteral("09:00")},
                          {QStringLiteral("weekdays"), QVariantList{1, 2, 3, 4, 5}}};
    domain::AutomationAction action;
    action.type = QStringLiteral("show_notification");
    action.message = QStringLiteral("Focus time");
    rule.actions.push_back(action);
    const auto saved = automations.saveRule(rule, 0);
    QVERIFY(saved.succeeded());

    storage::UniversalSearchRepository search(m_database);
    const auto results = search.search(QStringLiteral("weekday focus"));
    QVERIFY(results.succeeded());
    QCOMPARE(results.value.size(), 1);
    const QVariantMap result = results.value.front().toMap();
    QCOMPARE(result.value(QStringLiteral("type")).toString(), QStringLiteral("automationRule"));
    QCOMPARE(result.value(QStringLiteral("routeId")).toString(), QStringLiteral("automationRules"));
    QCOMPARE(result.value(QStringLiteral("targetId")).toString(), saved.value.id);
    const QVariantList actions = result.value(QStringLiteral("actions")).toList();
    QCOMPARE(actions.size(), 2);
    QCOMPARE(actions.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("open"));
    QCOMPARE(actions.at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("edit"));
}

void UniversalSearchRepositoryTest::searchesUnicodeCaseVariantsThroughSqlPrefilter()
{
    QSqlQuery insert(m_database);
    QVERIFY(insert.exec(QStringLiteral(
        "INSERT INTO tasks(title,description,priority,revision) "
        "VALUES('МиР Ééclair','Unicode search fixture','medium',1)")));

    storage::UniversalSearchRepository repository(m_database);
    const auto cyrillic = repository.search(QStringLiteral("мир"));
    QVERIFY(cyrillic.succeeded());
    QCOMPARE(cyrillic.value.size(), 1);
    QCOMPARE(cyrillic.value.first().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("МиР Ééclair"));

    const auto accented = repository.search(QStringLiteral("ééclair"));
    QVERIFY(accented.succeeded());
    QCOMPARE(accented.value.size(), 1);

    const auto canonicalEquivalent = repository.search(
        QString::fromUtf8("Cafe\xcc\x81"));
    QVERIFY(canonicalEquivalent.succeeded());
    QCOMPARE(canonicalEquivalent.value.size(), 1);
}

void UniversalSearchRepositoryTest::boundsLargeResultSetsAndHonorsCancellation()
{
    QSqlQuery insert(m_database);
    QVERIFY(insert.prepare(QStringLiteral(
        "INSERT INTO tasks(title,description,priority,revision) VALUES(?,?, 'medium',1)")));
    QVERIFY(m_database.transaction());
    for (int index = 0; index < 1500; ++index) {
        insert.bindValue(0, QStringLiteral("Bulk index %1").arg(index));
        insert.bindValue(1, QStringLiteral("A bounded search fixture"));
        QVERIFY(insert.exec());
        insert.finish();
    }
    QVERIFY(m_database.commit());

    storage::UniversalSearchRepository repository(m_database);
    const auto large = repository.search(QStringLiteral("bulk"));
    QVERIFY(large.succeeded());
    QVERIFY(large.value.size() <= 32);
    const auto cancelled = repository.search(QStringLiteral("bulk"), [] { return true; });
    QVERIFY(cancelled.error.isCancelled());
}

void UniversalSearchRepositoryTest::rejectsEmptyAndMalformedQueries()
{
    storage::UniversalSearchRepository repository(m_database);
    const auto empty = repository.search(QStringLiteral("   "));
    QCOMPARE(empty.error.category(), domain::ErrorCategory::Validation);
    const auto noResults = repository.search(QStringLiteral("no-such-record-72851"));
    QVERIFY(noResults.succeeded());
    QVERIFY(noResults.value.isEmpty());
    const auto tooManyWords = repository.search(QStringLiteral("one two three four five six seven eight nine"));
    QCOMPARE(tooManyWords.error.category(), domain::ErrorCategory::Validation);
}

QTEST_GUILESS_MAIN(UniversalSearchRepositoryTest)
#include "universal_search_repository_test.moc"
