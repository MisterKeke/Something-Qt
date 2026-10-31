#include <QtTest>

#include "storage/application_usage_repository.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QDate>
#include <QDateTime>
#include <QSqlQuery>
#include <QTime>
#include <QUuid>

using namespace something;

class ApplicationUsageFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void migrationCreatesUsageSchemaWithoutReplacingExistingData();
    void checkpointsAggregateAllFourStatesAndUnknownTime();
    void overviewTotalsIncludeApplicationsBeyondVisiblePage();
    void exclusionsConfigurationRecoveryAndHistoryDeletion();
};

void ApplicationUsageFeatureTest::migrationCreatesUsageSchemaWithoutReplacingExistingData()
{
    const QString connection = QStringLiteral("usage-migration-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    QVERIFY(storage::SchemaMigrator::apply(database));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 20);

    QSqlQuery fixture(database);
    QVERIFY(fixture.exec(QStringLiteral(
        "INSERT INTO tasks(title) VALUES('kept during usage migration')")));
    for (const QString &table : {QStringLiteral("usage_segments"),
                                 QStringLiteral("usage_sessions"),
                                 QStringLiteral("usage_tracked_applications"),
                                 QStringLiteral("usage_tracking_settings"),
                                 QStringLiteral("usage_excluded_applications")})
        QVERIFY(fixture.exec(QStringLiteral("DROP TABLE ") + table));
    QVERIFY(fixture.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version>=20")));
    fixture.finish();
    QString diagnostic;
    QVERIFY2(storage::SchemaMigrator::apply(database, &diagnostic), qPrintable(diagnostic));
    QVERIFY(fixture.exec(QStringLiteral(
        "SELECT title FROM tasks WHERE id=1")));
    QVERIFY(fixture.next());
    QCOMPARE(fixture.value(0).toString(), QStringLiteral("kept during usage migration"));
    fixture.finish();
    QVERIFY(fixture.exec(QStringLiteral(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name IN ('usage_tracked_applications','usage_sessions','usage_segments',"
        "'usage_tracking_settings','usage_excluded_applications')")));
    QVERIFY(fixture.next());
    QCOMPARE(fixture.value(0).toInt(), 5);
    fixture.finish();

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

void ApplicationUsageFeatureTest::checkpointsAggregateAllFourStatesAndUnknownTime()
{
    const QString connection = QStringLiteral("usage-checkpoint-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    QVERIFY(storage::SchemaMigrator::apply(database));
    storage::StorageRepositories repositories(database);

    const QString appId = QStringLiteral("path:c:/apps/editor.exe");
    const QString sessionId = QStringLiteral("session-1");
    storage::ApplicationUsageCheckpoint batch;
    batch.applications.push_back({appId, QStringLiteral("C:/Apps/editor.exe"),
        QStringLiteral("editor.exe"), QStringLiteral("Editor"),
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:04:00.000Z")});
    batch.sessions.push_back({sessionId, appId, QStringLiteral("2026-11-01T08:00:00.000Z"),
        QStringLiteral("2026-11-01T08:04:00.000Z"), QStringLiteral("2026-11-01T08:04:00.000Z"),
        QStringLiteral("closed"), 150000, false});
    const auto addSegment = [&](const QString &id, const QString &state,
                                const QString &start, const QString &end, qint64 duration) {
        storage::ApplicationUsageSegment segment;
        segment.segmentId = id;
        segment.sessionId = sessionId;
        segment.applicationId = appId;
        segment.state = state;
        segment.startedUtc = start;
        segment.endedUtc = end;
        segment.localDay = QStringLiteral("2026-11-01");
        segment.localHour = 11;
        segment.durationMs = duration;
        batch.segments.push_back(segment);
    };
    addSegment(QStringLiteral("segment-1"), QStringLiteral("ForegroundFullscreen"),
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:00:30.000Z"), 30000);
    addSegment(QStringLiteral("segment-2"), QStringLiteral("ForegroundWindowed"),
        QStringLiteral("2026-11-01T08:00:30.000Z"), QStringLiteral("2026-11-01T08:01:00.000Z"), 30000);
    addSegment(QStringLiteral("segment-3"), QStringLiteral("BackgroundVisible"),
        QStringLiteral("2026-11-01T08:01:00.000Z"), QStringLiteral("2026-11-01T08:01:30.000Z"), 30000);
    addSegment(QStringLiteral("segment-4"), QStringLiteral("Minimized"),
        QStringLiteral("2026-11-01T08:01:30.000Z"), QStringLiteral("2026-11-01T08:02:00.000Z"), 30000);
    addSegment(QStringLiteral("segment-5"), QStringLiteral("Unknown"),
        QStringLiteral("2026-11-01T08:02:00.000Z"), QStringLiteral("2026-11-01T08:02:30.000Z"), 30000);
    QVERIFY(repositories.applicationUsage.recordCheckpoint(batch, 0));
    // Replaying a checkpoint is safe: segment identifiers prevent duplicate durations.
    QVERIFY(repositories.applicationUsage.recordCheckpoint(batch, 0));

    const QVariantMap overview = repositories.applicationUsage.queryOverview(
        QStringLiteral("2026-11-01"), QStringLiteral("2026-11-01"), {}, {}, {}, QStringLiteral("total"));
    QCOMPARE(overview.value(QStringLiteral("totalMs")).toLongLong(), 150000);
    QCOMPARE(overview.value(QStringLiteral("foregroundMs")).toLongLong(), 60000);
    QCOMPARE(overview.value(QStringLiteral("fullscreenMs")).toLongLong(), 30000);
    QCOMPARE(overview.value(QStringLiteral("applicationCount")).toInt(), 1);

    const QVariantMap detail = repositories.applicationUsage.queryApplication(
        appId, QStringLiteral("2026-11-01"), QStringLiteral("2026-11-01"), {}, {});
    QCOMPARE(detail.value(QStringLiteral("totalMs")).toLongLong(), 150000);
    QCOMPARE(detail.value(QStringLiteral("foregroundMs")).toLongLong(), 60000);
    QCOMPARE(detail.value(QStringLiteral("sessionCount")).toInt(), 1);
    const QVariantMap distribution = detail.value(QStringLiteral("distribution")).toMap();
    QCOMPARE(distribution.value(QStringLiteral("BackgroundVisible")).toLongLong(), 30000);
    QCOMPARE(distribution.value(QStringLiteral("Minimized")).toLongLong(), 30000);
    QCOMPARE(distribution.value(QStringLiteral("Unknown")).toLongLong(), 30000);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

void ApplicationUsageFeatureTest::exclusionsConfigurationRecoveryAndHistoryDeletion()
{
    const QString connection = QStringLiteral("usage-config-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    QVERIFY(storage::SchemaMigrator::apply(database));
    storage::StorageRepositories repositories(database);

    bool enabled = false;
    int retention = 0;
    QVector<storage::ApplicationUsageExclusion> exclusions;
    QVERIFY(repositories.applicationUsage.loadConfiguration(&enabled, &retention, &exclusions));
    QVERIFY(enabled);
    QCOMPARE(retention, 365);
    QVERIFY(exclusions.isEmpty());
    QVERIFY(repositories.applicationUsage.setEnabled(false));
    QVERIFY(repositories.applicationUsage.setRetentionDays(90));
    storage::ApplicationUsageExclusion exclusion{
        QStringLiteral("filename:editor.exe"), {}, QStringLiteral("editor.exe"), true,
        QStringLiteral("2026-11-01T08:00:00.000Z")};
    QVERIFY(repositories.applicationUsage.addExclusion(exclusion));
    QVERIFY(repositories.applicationUsage.loadConfiguration(&enabled, &retention, &exclusions));
    QVERIFY(!enabled);
    QCOMPARE(retention, 90);
    QCOMPARE(exclusions.size(), 1);
    QVERIFY(exclusions.constFirst().matchByName);
    QVERIFY(repositories.applicationUsage.removeExclusion(exclusion.applicationId));

    const QString appId = QStringLiteral("path:c:/apps/browser.exe");
    storage::ApplicationUsageCheckpoint batch;
    batch.applications.push_back({appId, QStringLiteral("C:/Apps/browser.exe"),
        QStringLiteral("browser.exe"), QStringLiteral("Browser"),
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:00:15.000Z")});
    batch.sessions.push_back({QStringLiteral("open-session"), appId,
        QStringLiteral("2026-11-01T08:00:00.000Z"), {},
        QStringLiteral("2026-11-01T08:00:15.000Z"), {}, 15000, true});
    QVERIFY(repositories.applicationUsage.recordCheckpoint(batch, 0));
    QVERIFY(repositories.applicationUsage.recoverInterruptedSessions(
        QStringLiteral("2026-11-01T09:00:00.000Z")));
    QSqlQuery recovered(database);
    QVERIFY(recovered.exec(QStringLiteral(
        "SELECT ended_utc,end_reason,duration_ms FROM usage_sessions WHERE session_id='open-session'")));
    QVERIFY(recovered.next());
    QCOMPARE(recovered.value(0).toString(), QStringLiteral("2026-11-01T08:00:15.000Z"));
    QCOMPARE(recovered.value(1).toString(), QStringLiteral("interrupted"));
    QCOMPARE(recovered.value(2).toLongLong(), 15000);
    recovered.finish();

    const QDate expiredDay = QDate::currentDate().addDays(-400);
    const QString oldStart = QDateTime(expiredDay, QTime(10, 0), Qt::LocalTime)
        .toUTC().toString(Qt::ISODateWithMs);
    const QString oldEnd = QDateTime(expiredDay, QTime(10, 1), Qt::LocalTime)
        .toUTC().toString(Qt::ISODateWithMs);
    storage::ApplicationUsageCheckpoint expired;
    const QString expiredApplication = QStringLiteral("path:c:/apps/expired.exe");
    expired.applications.push_back({expiredApplication, QStringLiteral("C:/Apps/expired.exe"),
        QStringLiteral("expired.exe"), QStringLiteral("Expired"), oldStart, oldEnd});
    expired.sessions.push_back({QStringLiteral("expired-session"), expiredApplication,
        oldStart, oldEnd, oldEnd, QStringLiteral("closed"), 60000, false});
    expired.segments.push_back({QStringLiteral("expired-segment"),
        QStringLiteral("expired-session"), expiredApplication,
        QStringLiteral("ForegroundWindowed"), oldStart, oldEnd,
        expiredDay.toString(Qt::ISODate), 10, 60000});
    QVERIFY(repositories.applicationUsage.recordCheckpoint(expired, 0));
    QVERIFY(repositories.applicationUsage.setRetentionDays(90));
    QSqlQuery retained(database);
    QVERIFY(retained.exec(QStringLiteral(
        "SELECT COUNT(*) FROM usage_tracked_applications WHERE application_id='path:c:/apps/expired.exe'")));
    QVERIFY(retained.next());
    QCOMPARE(retained.value(0).toInt(), 0);
    retained.finish();

    QVERIFY(repositories.applicationUsage.deleteHistory(appId));
    QVERIFY(recovered.exec(QStringLiteral("SELECT COUNT(*) FROM usage_sessions")));
    QVERIFY(recovered.next());
    QCOMPARE(recovered.value(0).toInt(), 0);
    recovered.finish();
    QVERIFY(recovered.exec(QStringLiteral(
        "SELECT COUNT(*) FROM usage_tracked_applications WHERE application_id='path:c:/apps/browser.exe'")));
    QVERIFY(recovered.next());
    QCOMPARE(recovered.value(0).toInt(), 0);

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

void ApplicationUsageFeatureTest::overviewTotalsIncludeApplicationsBeyondVisiblePage()
{
    const QString connection = QStringLiteral("usage-overview-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    QVERIFY(storage::SchemaMigrator::apply(database));
    storage::StorageRepositories repositories(database);

    storage::ApplicationUsageCheckpoint batch;
    for (int index = 0; index < 501; ++index) {
        const QString suffix = QStringLiteral("%1").arg(index, 3, 10, QLatin1Char('0'));
        const QString applicationId = QStringLiteral("app:%1").arg(suffix);
        const QString sessionId = QStringLiteral("session:%1").arg(suffix);
        const QString segmentId = QStringLiteral("segment:%1").arg(suffix);
        const qint64 duration = index == 500 ? 2000 : 1000;
        batch.applications.push_back({applicationId, {}, QStringLiteral("app.exe"),
            QStringLiteral("Application %1").arg(suffix),
            QStringLiteral("2026-11-01T08:00:00.000Z"),
            QStringLiteral("2026-11-01T08:01:00.000Z")});
        batch.sessions.push_back({sessionId, applicationId,
            QStringLiteral("2026-11-01T08:00:00.000Z"),
            QStringLiteral("2026-11-01T08:01:00.000Z"),
            QStringLiteral("2026-11-01T08:01:00.000Z"), QStringLiteral("closed"),
            duration, false});
        batch.segments.push_back({segmentId, sessionId, applicationId,
            QStringLiteral("ForegroundFullscreen"),
            QStringLiteral("2026-11-01T08:00:00.000Z"),
            QStringLiteral("2026-11-01T08:01:00.000Z"),
            QStringLiteral("2026-11-01"), 11, duration});
    }
    QVERIFY(repositories.applicationUsage.recordCheckpoint(batch, 0));

    const QVariantMap overview = repositories.applicationUsage.queryOverview(
        QStringLiteral("2026-11-01"), QStringLiteral("2026-11-01"), {}, {}, {},
        QStringLiteral("name"));
    QCOMPARE(overview.value(QStringLiteral("applicationCount")).toLongLong(), 501);
    QCOMPARE(overview.value(QStringLiteral("applications")).toList().size(), 500);
    QCOMPARE(overview.value(QStringLiteral("totalMs")).toLongLong(), 502000);
    QCOMPARE(overview.value(QStringLiteral("fullscreenMs")).toLongLong(), 502000);
    QCOMPARE(overview.value(QStringLiteral("mostUsed")).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Application 500"));

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

QTEST_GUILESS_MAIN(ApplicationUsageFeatureTest)
#include "application_usage_feature_test.moc"
