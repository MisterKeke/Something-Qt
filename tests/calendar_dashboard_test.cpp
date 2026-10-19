#include "domain/clock.h"
#include "domain/task_date_rules.h"
#include "models/calendar_months_model.h"
#include "models/tasks_model.h"
#include "services/task_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTime>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest>

#include <utility>

using namespace something;

namespace {

class FixedClock final : public domain::Clock {
public:
    FixedClock(QDateTime instantUtc, QTimeZone zone)
        : m_instantUtc(std::move(instantUtc)), m_zone(std::move(zone))
    {
    }

    QDateTime nowUtc() const override { return m_instantUtc; }
    QTimeZone localTimeZone() const override { return m_zone; }

private:
    QDateTime m_instantUtc;
    QTimeZone m_zone;
};

storage::TaskInput taskInput(QString title, QString dueDate, QString priority = QStringLiteral("medium"))
{
    storage::TaskInput input;
    input.title = std::move(title);
    input.dueDate = std::move(dueDate);
    input.priority = std::move(priority);
    return input;
}

QString newConnectionName()
{
    return QStringLiteral("calendar-dashboard-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

bool openDatabase(QSqlDatabase *database, const QString &connectionName)
{
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database->setDatabaseName(QStringLiteral(":memory:"));
    return database->open() && storage::SchemaMigrator::apply(*database);
}

void closeDatabase(QSqlDatabase *database, const QString &connectionName)
{
    database->close();
    *database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

} // namespace

class CalendarDashboardTest final : public QObject {
    Q_OBJECT

private slots:
    void injectedClockUsesExplicitLocalZoneForDateOnlyTasks();
    void dashboardWeekBoundariesHonorAllPreferencesAndDefault();
    void calendarMonthGridIsMondayFirstAndUsesObservedMonthRange();
    void dateProjectionsKeepCalendarAndDashboardRulesSeparate();
    void calendarQueryIncludesCompletedTasksAndFetchesAllPages();
    void dashboardQueriesUseTodayAndConfiguredWeekBuckets();
    void calendarMutationSyncUsesTargetedModelRows();
};

void CalendarDashboardTest::injectedClockUsesExplicitLocalZoneForDateOnlyTasks()
{
    const QDateTime instantUtc(QDate(2026, 10, 18), QTime(22, 30), Qt::UTC);
    const QTimeZone localZone = QTimeZone::fromSecondsAheadOfUtc(3 * 60 * 60);
    QVERIFY(localZone.isValid());
    const FixedClock clock(instantUtc, localZone);
    QCOMPARE(clock.localDate(), QDate(2026, 10, 19));

    domain::Task task;
    task.dueDate = QStringLiteral("2026-10-18");
    QVERIFY(domain::taskIsDueOn(task, QDate(2026, 10, 18)));
    QVERIFY(!domain::taskIsDueOn(task, clock.localDate()));
}

void CalendarDashboardTest::dashboardWeekBoundariesHonorAllPreferencesAndDefault()
{
    const QDate monday(2026, 10, 19);
    for (int weekStart = 0; weekStart <= 6; ++weekStart) {
        for (int dayOffset = 0; dayOffset < 7; ++dayOffset) {
            const QDate today = monday.addDays(dayOffset);
            const auto range = domain::dashboardWeekRange(today, weekStart);
            const int todaySundayZero = today.dayOfWeek() % 7;
            const int daysFromStart = (todaySundayZero - weekStart + 7) % 7;
            QCOMPARE(range.weekStartSundayZero, weekStart);
            QCOMPARE(range.from, today.addDays(1));
            QCOMPARE(range.through, today.addDays(6 - daysFromStart));
        }
    }

    const auto invalidPreference = domain::dashboardWeekRange(monday, 99);
    QCOMPARE(invalidPreference.weekStartSundayZero, 1); // persisted default is Monday
    QCOMPARE(invalidPreference.from, monday.addDays(1));
    QCOMPARE(invalidPreference.through, monday.addDays(6));
}

void CalendarDashboardTest::calendarMonthGridIsMondayFirstAndUsesObservedMonthRange()
{
    models::CalendarMonthsModel months;
    const QDate today(2026, 10, 19);
    months.setCalendar(2026, today, QStringLiteral("2026-10-19"));
    QCOMPARE(months.count(), 3); // current month through December

    const QVariantList octoberDays = months.data(months.index(0, 0), models::CalendarMonthsModel::DaysRole)
                                         .toList();
    QCOMPARE(octoberDays.size(), 42);
    QCOMPARE(octoberDays.at(0).toMap().value(QStringLiteral("day")).toInt(), 0);
    QCOMPARE(octoberDays.at(1).toMap().value(QStringLiteral("day")).toInt(), 0);
    QCOMPARE(octoberDays.at(2).toMap().value(QStringLiteral("day")).toInt(), 0);
    QCOMPARE(octoberDays.at(3).toMap().value(QStringLiteral("day")).toInt(), 1); // Thu
    const QVariantMap selected = octoberDays.at(21).toMap();
    QCOMPARE(selected.value(QStringLiteral("date")).toString(), QStringLiteral("2026-10-19"));
    QVERIFY(selected.value(QStringLiteral("today")).toBool());
    QVERIFY(selected.value(QStringLiteral("selected")).toBool());

    months.setCalendar(2025, today, QStringLiteral("2026-10-19"));
    QCOMPARE(months.count(), 12);
    const QVariantList januaryDays = months.data(months.index(0, 0), models::CalendarMonthsModel::DaysRole)
                                        .toList();
    QCOMPARE(januaryDays.at(0).toMap().value(QStringLiteral("day")).toInt(), 0);
    QCOMPARE(januaryDays.at(1).toMap().value(QStringLiteral("day")).toInt(), 0);
    QCOMPARE(januaryDays.at(2).toMap().value(QStringLiteral("day")).toInt(), 1); // Wed
}

void CalendarDashboardTest::dateProjectionsKeepCalendarAndDashboardRulesSeparate()
{
    const QDate today(2026, 10, 19);
    domain::Task task;
    task.dueDate = QStringLiteral("2026-10-18");
    QVERIFY(!domain::taskIsDueOn(task, today));
    QVERIFY(domain::taskBelongsInDashboardToday(task, today)); // overdue is in Today
    QVERIFY(!domain::taskBelongsInDashboardWeek(task, today.addDays(1), today.addDays(6)));

    task.dueDate = QStringLiteral("2026-10-19");
    task.completed = true;
    QVERIFY(domain::taskIsDueOn(task, today)); // Calendar includes completed records
    QVERIFY(!domain::taskBelongsInDashboardToday(task, today));
    QVERIFY(!domain::taskBelongsInDashboardWeek(task, today.addDays(1), today.addDays(6)));

    task.completed = false;
    task.dueDate = QStringLiteral("2026-10-22");
    QVERIFY(domain::taskBelongsInDashboardWeek(task, today.addDays(1), today.addDays(6)));
}

void CalendarDashboardTest::calendarQueryIncludesCompletedTasksAndFetchesAllPages()
{
    const QString connectionName = newConnectionName();
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, connectionName));
    storage::StorageRepositories repositories(database);
    const QDate dueDate(2026, 10, 19);
    qint64 completedId = 0;
    constexpr int taskCount = 205;
    for (int index = 0; index < taskCount; ++index) {
        auto created = repositories.tasks.create(taskInput(
            QStringLiteral("Calendar task %1").arg(index), dueDate.toString(Qt::ISODate)));
        QVERIFY2(created.succeeded(), "calendar fixture task should be saved");
        if (index == 0) {
            completedId = created.value.id;
            const auto completed = repositories.tasks.setCompleted(
                created.value.id, created.value.revision, true);
            QVERIFY(completed.succeeded());
        }
    }

    const storage::StorageReply reply = services::TaskService::calendarDate(dueDate)(
        repositories, [] { return false; });
    QVERIFY(!reply.error.isError());
    const QVariantList rows = reply.values.value(QStringLiteral("tasks")).toList();
    QCOMPARE(rows.size(), taskCount);
    bool foundCompleted = false;
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("id")).toLongLong() == completedId) {
            foundCompleted = row.value(QStringLiteral("completed")).toBool();
            QCOMPARE(row.value(QStringLiteral("dueDate")).toString(), QStringLiteral("2026-10-19"));
        }
    }
    QVERIFY(foundCompleted);
    closeDatabase(&database, connectionName);
}

void CalendarDashboardTest::dashboardQueriesUseTodayAndConfiguredWeekBuckets()
{
    const QString connectionName = newConnectionName();
    QSqlDatabase database;
    QVERIFY(openDatabase(&database, connectionName));
    storage::StorageRepositories repositories(database);
    const QDate today(2026, 10, 21); // Wednesday
    QVERIFY(repositories.preferences.setWeekStart(0).succeeded()); // Sunday

    const auto overdue = repositories.tasks.create(taskInput(QStringLiteral("Overdue"),
                                                               QStringLiteral("2026-10-20")));
    const auto dueToday = repositories.tasks.create(taskInput(QStringLiteral("Today"),
                                                                today.toString(Qt::ISODate)));
    const auto undated = repositories.tasks.create(taskInput(QStringLiteral("Undated"), {}));
    const auto completedToday = repositories.tasks.create(taskInput(QStringLiteral("Completed"),
                                                                       today.toString(Qt::ISODate)));
    QVERIFY(overdue.succeeded() && dueToday.succeeded() && undated.succeeded()
            && completedToday.succeeded());
    QVERIFY(repositories.tasks.setCompleted(completedToday.value.id,
                                             completedToday.value.revision, true).succeeded());

    const storage::StorageReply todayReply = services::TaskService::dashboardToday(today)(
        repositories, [] { return false; });
    QVERIFY(!todayReply.error.isError());
    const QVariantList todayRows = todayReply.values.value(QStringLiteral("dueToday")).toList();
    const QVariantList overdueRows = todayReply.values.value(QStringLiteral("overdue")).toList();
    QCOMPARE(todayRows.size(), 1);
    QCOMPARE(todayRows.front().toMap().value(QStringLiteral("id")).toLongLong(), dueToday.value.id);
    QCOMPARE(overdueRows.size(), 1);
    QCOMPARE(overdueRows.front().toMap().value(QStringLiteral("id")).toLongLong(), overdue.value.id);
    QVERIFY(todayReply.values.value(QStringLiteral("undated")).toList().isEmpty());

    const storage::StorageReply withUndated = services::TaskService::dashboardToday(today, true, true)(
        repositories, [] { return false; });
    QVERIFY(!withUndated.error.isError());
    QCOMPARE(withUndated.values.value(QStringLiteral("undated")).toList().size(), 1);
    QCOMPARE(withUndated.values.value(QStringLiteral("undated")).toList().front()
                 .toMap().value(QStringLiteral("id")).toLongLong(), undated.value.id);

    const auto thursdayLow = repositories.tasks.create(taskInput(
        QStringLiteral("Thursday low"), QStringLiteral("2026-10-22"), QStringLiteral("low")));
    const auto thursdayHigh = repositories.tasks.create(taskInput(
        QStringLiteral("Thursday high"), QStringLiteral("2026-10-22"), QStringLiteral("high")));
    const auto saturday = repositories.tasks.create(taskInput(
        QStringLiteral("Saturday"), QStringLiteral("2026-10-24")));
    const auto nextSunday = repositories.tasks.create(taskInput(
        QStringLiteral("Next Sunday"), QStringLiteral("2026-10-25")));
    QVERIFY(thursdayLow.succeeded() && thursdayHigh.succeeded() && saturday.succeeded()
            && nextSunday.succeeded());

    const storage::StorageReply weekReply = services::TaskService::dashboardWeek(today)(
        repositories, [] { return false; });
    QVERIFY(!weekReply.error.isError());
    QCOMPARE(weekReply.values.value(QStringLiteral("fromDate")).toString(), QStringLiteral("2026-10-22"));
    QCOMPARE(weekReply.values.value(QStringLiteral("throughDate")).toString(), QStringLiteral("2026-10-24"));
    QCOMPARE(weekReply.values.value(QStringLiteral("weekStart")).toInt(), 0);
    const QVariantList weekRows = weekReply.values.value(QStringLiteral("items")).toList();
    QCOMPARE(weekRows.size(), 3);
    QCOMPARE(weekRows.at(0).toMap().value(QStringLiteral("id")).toLongLong(), thursdayHigh.value.id);
    QCOMPARE(weekRows.at(1).toMap().value(QStringLiteral("id")).toLongLong(), thursdayLow.value.id);
    QCOMPARE(weekRows.at(2).toMap().value(QStringLiteral("id")).toLongLong(), saturday.value.id);
    for (const QVariant &row : weekRows)
        QVERIFY(row.toMap().value(QStringLiteral("id")).toLongLong() != nextSunday.value.id);

    closeDatabase(&database, connectionName);
}

void CalendarDashboardTest::calendarMutationSyncUsesTargetedModelRows()
{
    models::TasksModel tasks;
    const QDate today(2026, 10, 19);
    tasks.setToday(today);
    domain::Task first;
    first.id = 1;
    first.title = QStringLiteral("First");
    first.createdAt = QStringLiteral("2026-10-01T09:00:00Z");
    domain::Task second;
    second.id = 2;
    second.title = QStringLiteral("Second");
    second.createdAt = QStringLiteral("2026-10-02T09:00:00Z");
    tasks.replacePage({second, first});

    QSignalSpy resets(&tasks, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&tasks, &QAbstractItemModel::dataChanged);
    QSignalSpy moved(&tasks, &QAbstractItemModel::rowsMoved);
    QSignalSpy inserted(&tasks, &QAbstractItemModel::rowsInserted);

    second.title = QStringLiteral("Second updated");
    second.revision = 2;
    QVERIFY(tasks.applyTask(second));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.front().front().value<QModelIndex>().row(), 0);

    second.createdAt = QStringLiteral("2026-09-30T09:00:00Z");
    tasks.upsertByCreatedOrder(second);
    QCOMPARE(moved.size(), 1);
    QCOMPARE(tasks.taskAt(0)->id, qint64(1));
    QCOMPARE(tasks.taskAt(1)->id, qint64(2));

    domain::Task third;
    third.id = 3;
    third.title = QStringLiteral("Third");
    third.createdAt = QStringLiteral("2026-10-04T09:00:00Z");
    tasks.upsertByCreatedOrder(third);
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(tasks.taskAt(0)->id, qint64(3));
    QCOMPARE(resets.size(), 0);
}

QTEST_GUILESS_MAIN(CalendarDashboardTest)
#include "calendar_dashboard_test.moc"
