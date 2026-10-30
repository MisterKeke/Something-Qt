#include "domain/scheduling_engine.h"

#include <QtTest>

#include <algorithm>
#include <utility>

using namespace something::domain;

namespace {

SchedulingInput baseInput()
{
    SchedulingInput input;
    input.nowUtc = QDateTime(QDate(2026, 1, 5), QTime(8, 0), Qt::UTC);
    input.localTimeZone = QTimeZone::utc();
    input.preferences.preferredWorkingDays = {1, 2, 3, 4, 5};
    input.preferences.workdayStart = QTime(9, 0);
    input.preferences.workdayEnd = QTime(17, 0);
    input.preferences.maximumPlannedWorkMinutesPerDay = 360;
    input.preferences.defaultEstimatedDurationMinutes = 60;
    input.preferences.breakBetweenWorkBlocksMinutes = 15;
    input.preferences.revision = 1;
    input.planningHorizonDays = 10;
    return input;
}

Task makeTask(qint64 id, QString dueDate = {}, QString priority = QStringLiteral("medium"),
              QString difficulty = QStringLiteral(""))
{
    Task task;
    task.id = id;
    task.title = QStringLiteral("Task %1").arg(id);
    task.dueDate = std::move(dueDate);
    task.priority = std::move(priority);
    task.difficulty = std::move(difficulty);
    task.revision = 1;
    return task;
}

TaskSchedule makeSchedule(qint64 taskId, QDate plannedDate,
                          std::optional<QTime> startTime, int durationMinutes,
                          qint64 revision = 1)
{
    TaskSchedule schedule;
    schedule.taskId = taskId;
    schedule.plannedDate = plannedDate;
    schedule.plannedStartTime = startTime;
    schedule.estimatedDurationMinutes = durationMinutes;
    schedule.revision = revision;
    return schedule;
}

const ScheduleRecommendation *recommendationFor(const SchedulingResult &result, qint64 taskId)
{
    const auto it = std::find_if(result.recommendations.cbegin(), result.recommendations.cend(),
                                 [taskId](const ScheduleRecommendation &recommendation) {
        return recommendation.task.id == taskId;
    });
    return it == result.recommendations.cend() ? nullptr : &*it;
}

const SchedulingDaySummary *summaryFor(const SchedulingResult &result, QDate date)
{
    const auto it = std::find_if(result.days.cbegin(), result.days.cend(),
                                 [date](const SchedulingDaySummary &summary) {
        return summary.date == date;
    });
    return it == result.days.cend() ? nullptr : &*it;
}

} // namespace

class SchedulingTest final : public QObject {
    Q_OBJECT

private slots:
    void overlappingExistingBlocksAreReportedForBothTasks();
    void highPriorityTiesUseStableTaskIdOrder();
    void overdueAndSameDayDeadlinesAreExplicitAndNeverScheduledInThePast();
    void noWorkingDaysAndExhaustedDailyCapacityAreUnschedulable();
    void missingDurationUsesDefaultForUndatedTask();
    void daylightSavingGapAndRepeatedHourRemainValid();
    void invalidPreferencesAreRejected();
};

void SchedulingTest::overlappingExistingBlocksAreReportedForBothTasks()
{
    SchedulingInput input = baseInput();
    input.incompleteTasks = {makeTask(11), makeTask(12)};
    input.existingSchedules = {
        makeSchedule(11, QDate(2026, 1, 6), QTime(10, 0), 60),
        makeSchedule(12, QDate(2026, 1, 6), QTime(10, 0), 60),
    };

    SchedulingResult result;
    QVERIFY(!calculateScheduleRecommendations(input, &result).isError());
    const auto *first = recommendationFor(result, 11);
    const auto *second = recommendationFor(result, 12);
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(first->hasConflict);
    QVERIFY(second->hasConflict);
    QVERIFY(first->warnings.join(QLatin1Char(' ')).contains(QStringLiteral("conflicts")));
    QVERIFY(second->warnings.join(QLatin1Char(' ')).contains(QStringLiteral("conflicts")));
    const auto *day = summaryFor(result, QDate(2026, 1, 6));
    QVERIFY(day);
    QCOMPARE(day->plannedMinutes, 120);
}

void SchedulingTest::highPriorityTiesUseStableTaskIdOrder()
{
    SchedulingInput input = baseInput();
    input.preferences.preferredWorkingDays = {1, 2, 3};
    input.preferences.workdayEnd = QTime(10, 0);
    input.preferences.maximumPlannedWorkMinutesPerDay = 60;
    input.preferences.breakBetweenWorkBlocksMinutes = 1;
    input.planningHorizonDays = 3;
    input.incompleteTasks = {
        makeTask(3, QStringLiteral("2026-01-08"), QStringLiteral("low")),
        makeTask(2, QStringLiteral("2026-01-08"), QStringLiteral("high")),
        makeTask(1, QStringLiteral("2026-01-08"), QStringLiteral("high")),
    };

    SchedulingResult result;
    QVERIFY(!calculateScheduleRecommendations(input, &result).isError());
    QCOMPARE(result.recommendations.size(), 3);
    QCOMPARE(result.recommendations.at(0).task.id, qint64(1));
    QCOMPARE(result.recommendations.at(1).task.id, qint64(2));
    QCOMPARE(result.recommendations.at(2).task.id, qint64(3));
    QCOMPARE(result.recommendations.at(0).plannedDate.value(), QDate(2026, 1, 5));
    QCOMPARE(result.recommendations.at(1).plannedDate.value(), QDate(2026, 1, 6));
    QCOMPARE(result.recommendations.at(2).plannedDate.value(), QDate(2026, 1, 7));

    std::reverse(input.incompleteTasks.begin(), input.incompleteTasks.end());
    SchedulingResult reversedResult;
    QVERIFY(!calculateScheduleRecommendations(input, &reversedResult).isError());
    QCOMPARE(reversedResult.recommendations.size(), result.recommendations.size());
    for (int i = 0; i < result.recommendations.size(); ++i) {
        QCOMPARE(reversedResult.recommendations.at(i).task.id,
                 result.recommendations.at(i).task.id);
        QCOMPARE(reversedResult.recommendations.at(i).plannedDate.value(),
                 result.recommendations.at(i).plannedDate.value());
        QCOMPARE(reversedResult.recommendations.at(i).plannedStartTime.value(),
                 result.recommendations.at(i).plannedStartTime.value());
    }
}

void SchedulingTest::overdueAndSameDayDeadlinesAreExplicitAndNeverScheduledInThePast()
{
    SchedulingInput input = baseInput();
    input.nowUtc = QDateTime(QDate(2026, 1, 5), QTime(9, 31, 30), Qt::UTC);
    input.preferences.defaultEstimatedDurationMinutes = 30;
    input.preferences.maximumPlannedWorkMinutesPerDay = 120;
    input.incompleteTasks = {
        makeTask(21, QStringLiteral("2026-01-04")),
        makeTask(22, QStringLiteral("2026-01-05")),
    };

    SchedulingResult result;
    QVERIFY(!calculateScheduleRecommendations(input, &result).isError());
    const auto *overdue = recommendationFor(result, 21);
    const auto *dueToday = recommendationFor(result, 22);
    QVERIFY(overdue);
    QVERIFY(dueToday);
    QVERIFY(overdue->overdue);
    QVERIFY(overdue->warnings.join(QLatin1Char(' ')).contains(QStringLiteral("overdue")));
    QVERIFY(!dueToday->overdue);
    QCOMPARE(overdue->plannedDate.value(), QDate(2026, 1, 5));
    QCOMPARE(dueToday->plannedDate.value(), QDate(2026, 1, 5));
    QCOMPARE(overdue->plannedStartTime.value(), QTime(9, 32));
    QCOMPARE(dueToday->plannedStartTime.value(), QTime(10, 17));
    QVERIFY(QDateTime(result.today, overdue->plannedStartTime.value(), Qt::UTC)
            >= input.nowUtc);
}

void SchedulingTest::noWorkingDaysAndExhaustedDailyCapacityAreUnschedulable()
{
    SchedulingInput noDays = baseInput();
    noDays.preferences.preferredWorkingDays.clear();
    noDays.incompleteTasks = {makeTask(31)};
    SchedulingResult noDaysResult;
    QVERIFY(!calculateScheduleRecommendations(noDays, &noDaysResult).isError());
    const auto *withoutDay = recommendationFor(noDaysResult, 31);
    QVERIFY(withoutDay);
    QCOMPARE(withoutDay->state, RecommendationState::Unscheduled);
    QVERIFY(withoutDay->reason.contains(QStringLiteral("no working days")));
    QVERIFY(!noDaysResult.warnings.isEmpty());

    SchedulingInput fullDay = baseInput();
    fullDay.preferences.preferredWorkingDays = {1};
    fullDay.preferences.workdayEnd = QTime(10, 0);
    fullDay.preferences.maximumPlannedWorkMinutesPerDay = 60;
    fullDay.planningHorizonDays = 1;
    fullDay.incompleteTasks = {makeTask(32), makeTask(33)};
    fullDay.existingSchedules = {
        makeSchedule(33, QDate(2026, 1, 5), std::nullopt, 60),
    };
    SchedulingResult fullDayResult;
    QVERIFY(!calculateScheduleRecommendations(fullDay, &fullDayResult).isError());
    const auto *retained = recommendationFor(fullDayResult, 33);
    const auto *blocked = recommendationFor(fullDayResult, 32);
    QVERIFY(retained);
    QVERIFY(blocked);
    QCOMPARE(retained->state, RecommendationState::Scheduled);
    QVERIFY(retained->retainedExistingSchedule);
    QCOMPARE(blocked->state, RecommendationState::Unscheduled);
    const auto *day = summaryFor(fullDayResult, QDate(2026, 1, 5));
    QVERIFY(day);
    QCOMPARE(day->plannedMinutes, 60);
    QCOMPARE(day->remainingMinutes, 0);
}

void SchedulingTest::missingDurationUsesDefaultForUndatedTask()
{
    SchedulingInput input = baseInput();
    input.preferences.defaultEstimatedDurationMinutes = 45;
    input.incompleteTasks = {makeTask(41)};

    SchedulingResult result;
    QVERIFY(!calculateScheduleRecommendations(input, &result).isError());
    const auto *recommendation = recommendationFor(result, 41);
    QVERIFY(recommendation);
    QVERIFY(recommendation->usedDefaultDuration);
    QCOMPARE(recommendation->estimatedDurationMinutes, 45);
    QCOMPARE(recommendation->state, RecommendationState::Scheduled);
    QVERIFY(!recommendation->plannedDate->isNull());
}

void SchedulingTest::daylightSavingGapAndRepeatedHourRemainValid()
{
    const QTimeZone newYork(QByteArrayLiteral("America/New_York"));
    QVERIFY(newYork.isValid());

    SchedulingInput spring = baseInput();
    spring.localTimeZone = newYork;
    spring.nowUtc = QDateTime(QDate(2026, 3, 8), QTime(5, 0), Qt::UTC);
    spring.preferences.preferredWorkingDays = {7};
    spring.preferences.workdayStart = QTime(1, 0);
    spring.preferences.workdayEnd = QTime(4, 0);
    spring.preferences.maximumPlannedWorkMinutesPerDay = 120;
    spring.preferences.breakBetweenWorkBlocksMinutes = 30;
    spring.incompleteTasks = {makeTask(51), makeTask(52)};
    spring.existingSchedules = {
        makeSchedule(51, QDate(2026, 3, 8), QTime(1, 0), 30),
    };
    SchedulingResult springResult;
    QVERIFY(!calculateScheduleRecommendations(spring, &springResult).isError());
    const auto *afterGap = recommendationFor(springResult, 52);
    QVERIFY(afterGap);
    QCOMPARE(afterGap->state, RecommendationState::Scheduled);
    QCOMPARE(afterGap->plannedDate.value(), QDate(2026, 3, 8));
    QCOMPARE(afterGap->plannedStartTime.value(), QTime(3, 0));

    SchedulingInput autumn = baseInput();
    autumn.localTimeZone = newYork;
    autumn.nowUtc = QDateTime(QDate(2026, 11, 1), QTime(4, 0), Qt::UTC);
    autumn.preferences.preferredWorkingDays = {7};
    autumn.preferences.workdayStart = QTime(1, 0);
    autumn.preferences.workdayEnd = QTime(3, 0);
    autumn.preferences.maximumPlannedWorkMinutesPerDay = 120;
    autumn.preferences.breakBetweenWorkBlocksMinutes.reset();
    autumn.incompleteTasks = {makeTask(53)};
    SchedulingResult autumnResult;
    QVERIFY(!calculateScheduleRecommendations(autumn, &autumnResult).isError());
    const auto *duringRepeatedHour = recommendationFor(autumnResult, 53);
    QVERIFY(duringRepeatedHour);
    QCOMPARE(duringRepeatedHour->state, RecommendationState::Scheduled);
    QCOMPARE(duringRepeatedHour->plannedDate.value(), QDate(2026, 11, 1));
    QCOMPARE(duringRepeatedHour->plannedStartTime.value(), QTime(1, 0));
    const QDateTime chosenStart(duringRepeatedHour->plannedDate.value(),
                                duringRepeatedHour->plannedStartTime.value(), newYork);
    QVERIFY(chosenStart.isValid());
    QVERIFY(chosenStart.toUTC().isValid());
}

void SchedulingTest::invalidPreferencesAreRejected()
{
    QVector<SchedulingPreferences> invalidPreferences;
    SchedulingPreferences zeroCapacity = baseInput().preferences;
    zeroCapacity.maximumPlannedWorkMinutesPerDay = 0;
    invalidPreferences.push_back(zeroCapacity);

    SchedulingPreferences reversedHours = baseInput().preferences;
    reversedHours.workdayStart = QTime(17, 0);
    reversedHours.workdayEnd = QTime(9, 0);
    invalidPreferences.push_back(reversedHours);

    SchedulingPreferences tooMuchDailyWork = baseInput().preferences;
    tooMuchDailyWork.maximumPlannedWorkMinutesPerDay = 481;
    invalidPreferences.push_back(tooMuchDailyWork);

    SchedulingPreferences duplicateDays = baseInput().preferences;
    duplicateDays.preferredWorkingDays = {1, 1};
    invalidPreferences.push_back(duplicateDays);

    SchedulingPreferences invalidDay = baseInput().preferences;
    invalidDay.preferredWorkingDays = {8};
    invalidPreferences.push_back(invalidDay);

    SchedulingPreferences zeroBreak = baseInput().preferences;
    zeroBreak.breakBetweenWorkBlocksMinutes = 0;
    invalidPreferences.push_back(zeroBreak);

    for (SchedulingPreferences preferences : std::as_const(invalidPreferences)) {
        QVERIFY(normalizeSchedulingPreferences(preferences).isError());
        SchedulingInput input = baseInput();
        input.preferences = preferences;
        input.incompleteTasks = {makeTask(61)};
        SchedulingResult result;
        QCOMPARE(calculateScheduleRecommendations(input, &result).category(),
                 ErrorCategory::Validation);
    }
}

QTEST_GUILESS_MAIN(SchedulingTest)
#include "scheduling_test.moc"
