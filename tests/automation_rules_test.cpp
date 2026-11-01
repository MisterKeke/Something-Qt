#include "domain/automation_rules.h"

#include <QtTest>

using namespace something::domain;

class AutomationRulesTest final : public QObject {
    Q_OBJECT

private slots:
    void taskRuleMatchesTypedContext();
    void steamPriceRuleRequiresMarketAndThresholdCrossing();
    void applicationAndSteamTriggersMatchExactIdentities();
    void scheduledRulesEvaluateLocalCalendarConditions();
    void timeRangeIncludesItsEntireFinalMinute();
    void scheduledTimeMatchesMinuteWhenSecondsAndMillisecondsArePresent();
    void foregroundEntriesIgnoreStateChangesInsideOneForegroundSession();
    void rejectsConditionsThatDoNotFitTheTrigger();
    void rejectsInvalidEventAndConditionOperator();
    void notificationPlaceholdersRequireEventData();
};

void AutomationRulesTest::taskRuleMatchesTypedContext()
{
    AutomationRule rule;
    rule.id = QStringLiteral("task-example");
    rule.name = QStringLiteral("Finish work");
    rule.triggerType = QStringLiteral("task.completed");
    rule.triggerConfig = {{QStringLiteral("taskId"), 0},
                          {QStringLiteral("tag"), QStringLiteral("work")},
                          {QStringLiteral("priority"), QStringLiteral("high")}};
    rule.conditions = {{QStringLiteral("task_priority"), QStringLiteral("high"),
                        QStringLiteral("equals")},
                       {QStringLiteral("task_tag"), QStringLiteral("work"),
                        QStringLiteral("contains")}};
    AutomationAction action;
    action.type = QStringLiteral("show_notification");
    action.message = QStringLiteral("{task.title} completed");
    rule.actions.push_back(action);
    QVERIFY(!AutomationRules::normalizeRule(rule).isError());

    AutomationEvent event;
    event.id = QStringLiteral("task:completed:task:41:revision:2");
    event.type = QStringLiteral("task.completed");
    event.sourceModule = QStringLiteral("tasks");
    event.sourceEntityId = QStringLiteral("41");
    event.occurredAtUtc = QDateTime::currentDateTimeUtc();
    event.payload = {{QStringLiteral("task.id"), 41},
                     {QStringLiteral("task.title"), QStringLiteral("Finish work")},
                     {QStringLiteral("task.priority"), QStringLiteral("high")},
                     {QStringLiteral("task.tags"), QStringList{QStringLiteral("Work")}}};

    QVERIFY(!AutomationRules::validateEvent(event).isError());
    QVERIFY(AutomationRules::triggerMatches(rule, event));
    QVERIFY(AutomationRules::conditionsMatch(rule, event));
    QCOMPARE(AutomationRules::actionSummary(rule), QStringLiteral("show notification"));

    event.payload.insert(QStringLiteral("task.priority"), QStringLiteral("low"));
    QVERIFY(!AutomationRules::conditionsMatch(rule, event));
}

void AutomationRulesTest::steamPriceRuleRequiresMarketAndThresholdCrossing()
{
    AutomationRule rule;
    rule.id = QStringLiteral("price-example");
    rule.name = QStringLiteral("Game price alert");
    rule.triggerType = QStringLiteral("steam.price_target_reached");
    rule.triggerConfig = {{QStringLiteral("appId"), 570},
                          {QStringLiteral("country"), QStringLiteral("TR")},
                          {QStringLiteral("currency"), QStringLiteral("TRY")},
                          {QStringLiteral("targetMinor"), 30000}};
    AutomationAction action;
    action.type = QStringLiteral("show_notification");
    action.message = QStringLiteral("{steam.gameName} reached {steam.priceMinor}");
    rule.actions.push_back(action);
    QVERIFY(!AutomationRules::normalizeRule(rule).isError());

    AutomationEvent event;
    event.id = QStringLiteral("steam:price:570:TR:2026-11-01T12:00:00.000Z");
    event.type = QStringLiteral("steam.price_target_reached");
    event.sourceModule = QStringLiteral("steam-price-tracking");
    event.sourceEntityId = QStringLiteral("570");
    event.occurredAtUtc = QDateTime::currentDateTimeUtc();
    event.payload = {{QStringLiteral("steam.appId"), 570},
                     {QStringLiteral("steam.country"), QStringLiteral("TR")},
                     {QStringLiteral("steam.currency"), QStringLiteral("TRY")},
                     {QStringLiteral("steam.previousPriceMinor"), 31000},
                     {QStringLiteral("steam.priceMinor"), 30000},
                     {QStringLiteral("steam.gameName"), QStringLiteral("Example")}};
    QVERIFY(AutomationRules::triggerMatches(rule, event));

    event.payload.insert(QStringLiteral("steam.country"), QStringLiteral("US"));
    QVERIFY(!AutomationRules::triggerMatches(rule, event));
    event.payload.insert(QStringLiteral("steam.country"), QStringLiteral("TR"));
    event.payload.insert(QStringLiteral("steam.previousPriceMinor"), 29000);
    QVERIFY(!AutomationRules::triggerMatches(rule, event));
}

void AutomationRulesTest::applicationAndSteamTriggersMatchExactIdentities()
{
    AutomationRule applicationRule;
    applicationRule.id = QStringLiteral("application-rule");
    applicationRule.name = QStringLiteral("App started");
    applicationRule.triggerType = QStringLiteral("application.session_started");
    applicationRule.triggerConfig = {{QStringLiteral("applicationId"),
                                      QStringLiteral("path:c:/games/example.exe")}};
    AutomationAction applicationAction;
    applicationAction.type = QStringLiteral("show_notification");
    applicationAction.message = QStringLiteral("Started {application.name}");
    applicationRule.actions.push_back(applicationAction);
    QVERIFY(!AutomationRules::normalizeRule(applicationRule).isError());

    AutomationEvent applicationEvent;
    applicationEvent.id = QStringLiteral("usage:session:session-1");
    applicationEvent.type = applicationRule.triggerType;
    applicationEvent.sourceModule = QStringLiteral("application-usage");
    applicationEvent.sourceEntityId = QStringLiteral("path:c:/games/example.exe");
    applicationEvent.occurredAtUtc = QDateTime::currentDateTimeUtc();
    applicationEvent.payload = {{QStringLiteral("application.id"),
                                 QStringLiteral("path:c:/games/example.exe")},
                                {QStringLiteral("application.name"), QStringLiteral("Example")}};
    QVERIFY(AutomationRules::triggerMatches(applicationRule, applicationEvent));
    applicationEvent.payload.insert(QStringLiteral("application.id"),
                                    QStringLiteral("path:c:/games/other.exe"));
    QVERIFY(!AutomationRules::triggerMatches(applicationRule, applicationEvent));

    AutomationRule steamRule;
    steamRule.id = QStringLiteral("steam-release-rule");
    steamRule.name = QStringLiteral("Release changes");
    steamRule.triggerType = QStringLiteral("steam.release_date_changed");
    steamRule.triggerConfig = {{QStringLiteral("appId"), 730}};
    AutomationAction steamAction;
    steamAction.type = QStringLiteral("show_notification");
    steamAction.message = QStringLiteral("{steam.gameName}: {steam.releaseDate}");
    steamRule.actions.push_back(steamAction);
    QVERIFY(!AutomationRules::normalizeRule(steamRule).isError());

    AutomationEvent steamEvent;
    steamEvent.id = QStringLiteral("steam:release:730:event-1");
    steamEvent.type = steamRule.triggerType;
    steamEvent.sourceModule = QStringLiteral("steam-release-radar");
    steamEvent.sourceEntityId = QStringLiteral("730");
    steamEvent.occurredAtUtc = QDateTime::currentDateTimeUtc();
    steamEvent.payload = {{QStringLiteral("steam.appId"), 730},
                          {QStringLiteral("steam.gameName"), QStringLiteral("Example")},
                          {QStringLiteral("steam.releaseDate"), QStringLiteral("Coming soon")}};
    QVERIFY(AutomationRules::triggerMatches(steamRule, steamEvent));
    steamEvent.payload.insert(QStringLiteral("steam.appId"), 570);
    QVERIFY(!AutomationRules::triggerMatches(steamRule, steamEvent));
}

void AutomationRulesTest::scheduledRulesEvaluateLocalCalendarConditions()
{
    AutomationRule rule;
    rule.id = QStringLiteral("weekday-schedule");
    rule.name = QStringLiteral("Weekday schedule");
    rule.triggerType = QStringLiteral("schedule.time_reached");
    rule.triggerConfig = {{QStringLiteral("time"), QStringLiteral("09:00")},
                          {QStringLiteral("weekdays"), QVariantList{1, 2, 3, 4, 5}}};
    rule.conditions = {{QStringLiteral("time_range"), QStringLiteral("00:00/23:59"),
                        QStringLiteral("between")}};
    AutomationAction action;
    action.type = QStringLiteral("show_notification");
    action.message = QStringLiteral("Scheduled at {schedule.localTime}");
    rule.actions.push_back(action);
    QVERIFY(!AutomationRules::normalizeRule(rule).isError());

    AutomationEvent event;
    event.id = QStringLiteral("schedule:weekday-schedule:2026-11-02 09:00");
    event.type = rule.triggerType;
    event.sourceModule = QStringLiteral("automation-scheduler");
    event.sourceEntityId = rule.id;
    event.occurredAtUtc = QDateTime::currentDateTimeUtc();
    event.payload = {{QStringLiteral("schedule.id"), rule.id},
                     {QStringLiteral("schedule.localTime"), QStringLiteral("09:00")}};
    QVERIFY(AutomationRules::triggerMatches(rule, event));
    QVERIFY(AutomationRules::conditionsMatch(rule, event));

    const int localDay = event.occurredAtUtc.toLocalTime().date().dayOfWeek();
    rule.conditions.push_back({QStringLiteral("day_of_week"), QString::number(localDay),
                               QStringLiteral("equals")});
    QVERIFY(!AutomationRules::normalizeRule(rule).isError());
    QVERIFY(AutomationRules::conditionsMatch(rule, event));
    rule.conditions.last().operand = QString::number(localDay == 7 ? 1 : localDay + 1);
    QVERIFY(!AutomationRules::conditionsMatch(rule, event));
}

void AutomationRulesTest::scheduledTimeMatchesMinuteWhenSecondsAndMillisecondsArePresent()
{
    const QDateTime localNow(QDate(2026, 11, 1), QTime(9, 0, 37, 842), Qt::UTC);
    QVERIFY(AutomationRules::scheduledTimeMatchesMinute(QStringLiteral("09:00"), localNow));
    QVERIFY(!AutomationRules::scheduledTimeMatchesMinute(QStringLiteral("09:01"), localNow));
    QVERIFY(!AutomationRules::scheduledTimeMatchesMinute(QStringLiteral("09:0x"), localNow));
}

void AutomationRulesTest::timeRangeIncludesItsEntireFinalMinute()
{
    const auto matchesAtLocalTime = [](const QString &range, const QTime &time) {
        AutomationRule rule;
        rule.id = QStringLiteral("time-range");
        rule.name = QStringLiteral("Time range");
        rule.triggerType = QStringLiteral("schedule.time_reached");
        rule.triggerConfig = {{QStringLiteral("time"), QStringLiteral("09:00")},
                              {QStringLiteral("weekdays"), QVariantList{1}}};
        rule.conditions = {{QStringLiteral("time_range"), range, QStringLiteral("between")}};
        AutomationAction action;
        action.type = QStringLiteral("show_notification");
        action.message = QStringLiteral("Scheduled");
        rule.actions.push_back(action);
        if (AutomationRules::normalizeRule(rule).isError()) return false;

        const QDate date(2026, 6, 15); // Monday, outside local DST transition dates.
        const QDateTime local(date, time, QTimeZone::systemTimeZone());
        if (!local.isValid()) return false;
        AutomationEvent event;
        event.id = QStringLiteral("schedule:time-range:event");
        event.type = rule.triggerType;
        event.sourceModule = QStringLiteral("automation-scheduler");
        event.sourceEntityId = rule.id;
        event.occurredAtUtc = local.toUTC();
        event.payload = {{QStringLiteral("schedule.id"), rule.id}};
        return AutomationRules::conditionsMatch(rule, event);
    };

    QVERIFY(matchesAtLocalTime(QStringLiteral("09:00/10:00"), QTime(10, 0, 59, 999)));
    QVERIFY(!matchesAtLocalTime(QStringLiteral("09:00/10:00"), QTime(10, 1, 0)));
    QVERIFY(matchesAtLocalTime(QStringLiteral("23:00/01:00"), QTime(1, 0, 59, 999)));
    QVERIFY(!matchesAtLocalTime(QStringLiteral("23:00/01:00"), QTime(1, 1, 0)));
}

void AutomationRulesTest::foregroundEntriesIgnoreStateChangesInsideOneForegroundSession()
{
    QVERIFY(AutomationRules::isForegroundEntry(false, QStringLiteral("ForegroundFullscreen")));
    QVERIFY(!AutomationRules::isForegroundEntry(true, QStringLiteral("ForegroundWindowed")));
    QVERIFY(!AutomationRules::isForegroundEntry(true, QStringLiteral("Unknown")));
    QVERIFY(!AutomationRules::isForegroundEntry(false, QStringLiteral("BackgroundVisible")));
    QVERIFY(AutomationRules::isForegroundEntry(false, QStringLiteral("ForegroundWindowed")));
}

void AutomationRulesTest::rejectsConditionsThatDoNotFitTheTrigger()
{
    AutomationRule rule;
    rule.id = QStringLiteral("invalid-condition");
    rule.name = QStringLiteral("Invalid rule");
    rule.triggerType = QStringLiteral("task.completed");
    rule.conditions.push_back({QStringLiteral("time_range"), QStringLiteral("09:00/10:00"),
                               QStringLiteral("between")});
    AutomationAction action;
    action.type = QStringLiteral("show_notification");
    action.message = QStringLiteral("Done");
    rule.actions.push_back(action);
    QVERIFY(AutomationRules::normalizeRule(rule).isError());
}

void AutomationRulesTest::rejectsInvalidEventAndConditionOperator()
{
    AutomationEvent invalidEvent;
    invalidEvent.id = QStringLiteral("missing-app-id");
    invalidEvent.type = QStringLiteral("application.session_started");
    invalidEvent.sourceModule = QStringLiteral("application-usage");
    invalidEvent.occurredAtUtc = QDateTime::currentDateTimeUtc();
    QVERIFY(AutomationRules::validateEvent(invalidEvent).isError());

    AutomationRule rule;
    rule.id = QStringLiteral("bad-operator");
    rule.name = QStringLiteral("Bad operator");
    rule.triggerType = QStringLiteral("task.completed");
    rule.conditions.push_back({QStringLiteral("task_id"), QStringLiteral("42"),
                               QStringLiteral("less_equal")});
    AutomationAction action;
    action.type = QStringLiteral("show_notification");
    action.message = QStringLiteral("Done");
    rule.actions.push_back(action);
    QVERIFY(AutomationRules::normalizeRule(rule).isError());
}

void AutomationRulesTest::notificationPlaceholdersRequireEventData()
{
    AutomationEvent event;
    event.id = QStringLiteral("task-event");
    event.type = QStringLiteral("task.completed");
    event.sourceModule = QStringLiteral("tasks");
    event.occurredAtUtc = QDateTime::currentDateTimeUtc();
    event.payload = {{QStringLiteral("task.id"), 2},
                     {QStringLiteral("task.title"), QStringLiteral("Write docs")}};

    bool valid = false;
    QCOMPARE(AutomationRules::expandNotification(QStringLiteral("{task.title} done"), event, &valid),
             QStringLiteral("Write docs done"));
    QVERIFY(valid);
    QCOMPARE(AutomationRules::expandNotification(QStringLiteral("{steam.gameName}"), event, &valid),
             QString{});
    QVERIFY(!valid);
}

QTEST_GUILESS_MAIN(AutomationRulesTest)
#include "automation_rules_test.moc"
