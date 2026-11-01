#include "../src/domain/task_rules.h"

#include <QtTest>

using namespace something::domain;

class TaskDomainTest final : public QObject {
    Q_OBJECT

private slots:
    void inputNormalizationAndValidation();
    void filterNormalizationAndValidation();
    void taskAutomationValidationAndUrlNormalization();
};

void TaskDomainTest::inputNormalizationAndValidation()
{
    TaskInput input;
    input.title = QStringLiteral("  Plan release  ");
    input.description = QStringLiteral("  include the rollback plan  ");
    input.dueDate = QStringLiteral("2026-02-28");
    input.priority.clear();
    input.difficulty = QStringLiteral(" HARD ");
    input.tags = QStringList{QStringLiteral(" Roadmap "), QStringLiteral("roadmap"),
                             QStringLiteral("QA")};
    input.subtasks = QVector<SubtaskInput>{{0, QStringLiteral(" Prepare build "), false},
                                           {0, QStringLiteral("Review release notes"), true}};

    QVERIFY(!normalizeTaskInput(input).isError());
    QCOMPARE(input.title, QStringLiteral("Plan release"));
    QCOMPARE(input.description, QStringLiteral("include the rollback plan"));
    QCOMPARE(input.priority, QStringLiteral("medium"));
    QCOMPARE(input.difficulty.value(), QStringLiteral("hard"));
    QCOMPARE(input.tags.value(), QStringList({QStringLiteral("Roadmap"), QStringLiteral("QA")}));
    QCOMPARE(input.subtasks->front().title, QStringLiteral("Prepare build"));

    TaskInput invalidDate;
    invalidDate.title = QStringLiteral("Invalid calendar date");
    invalidDate.dueDate = QStringLiteral("2026-02-29");
    QCOMPARE(normalizeTaskInput(invalidDate).category(), ErrorCategory::Validation);

    TaskInput invalidSubtasks;
    invalidSubtasks.title = QStringLiteral("No hard metadata");
    invalidSubtasks.difficulty = QStringLiteral("easy");
    invalidSubtasks.subtasks = QVector<SubtaskInput>{{0, QStringLiteral("Child"), false}};
    QCOMPARE(normalizeTaskInput(invalidSubtasks).category(), ErrorCategory::Validation);

    TaskInput duplicateIds;
    duplicateIds.title = QStringLiteral("Duplicate IDs");
    duplicateIds.difficulty = QStringLiteral("hard");
    duplicateIds.subtasks = QVector<SubtaskInput>{{17, QStringLiteral("One"), false},
                                                  {17, QStringLiteral("Two"), false}};
    QCOMPARE(normalizeTaskInput(duplicateIds).category(), ErrorCategory::Validation);

    TaskInput excessiveTags;
    excessiveTags.title = QStringLiteral("Too many tags");
    QStringList tags;
    for (int i = 0; i < 33; ++i) tags.push_back(QStringLiteral("tag-%1").arg(i));
    excessiveTags.tags = tags;
    QCOMPARE(normalizeTaskInput(excessiveTags).category(), ErrorCategory::Validation);

    TaskInput longTag;
    longTag.title = QStringLiteral("Long tag");
    longTag.tags = QStringList{QString(65, QLatin1Char('x'))};
    QCOMPARE(normalizeTaskInput(longTag).category(), ErrorCategory::Validation);

    TaskInput excessiveSubtasks;
    excessiveSubtasks.title = QStringLiteral("Too many subtasks");
    excessiveSubtasks.difficulty = QStringLiteral("hard");
    QVector<SubtaskInput> subtasks;
    for (int i = 0; i < 101; ++i)
        subtasks.push_back({0, QStringLiteral("Step %1").arg(i), false});
    excessiveSubtasks.subtasks = subtasks;
    QCOMPARE(normalizeTaskInput(excessiveSubtasks).category(), ErrorCategory::Validation);
}

void TaskDomainTest::filterNormalizationAndValidation()
{
    TaskFilter filter;
    filter.query = QStringLiteral("  café  ");
    filter.tags = {QStringLiteral(" Build "), QStringLiteral("build"), QStringLiteral("QA")};
    filter.completion = QStringLiteral(" INCOMPLETE ");
    filter.dueFrom = QStringLiteral("2026-10-01");
    filter.dueTo = QStringLiteral("2026-10-31");
    filter.sort = QStringLiteral(" TITLE ");
    filter.direction = QStringLiteral("ASC");
    QVERIFY(!normalizeTaskFilter(filter).isError());
    QCOMPARE(filter.query, QStringLiteral("café"));
    QCOMPARE(filter.completion, QStringLiteral("incomplete"));
    QCOMPARE(filter.tags, QStringList({QStringLiteral("Build"), QStringLiteral("QA")}));
    QCOMPARE(filter.sort, QStringLiteral("title"));
    QCOMPARE(filter.direction, QStringLiteral("asc"));

    TaskFilter tooLong;
    tooLong.query = QString(257, QLatin1Char('x'));
    QCOMPARE(normalizeTaskFilter(tooLong).category(), ErrorCategory::Validation);

    TaskFilter incompatibleDates;
    incompatibleDates.dueDate = QStringLiteral("2026-10-18");
    incompatibleDates.overdue = true;
    QCOMPARE(normalizeTaskFilter(incompatibleDates).category(), ErrorCategory::Validation);

    TaskFilter invalidDate;
    invalidDate.dueFrom = QStringLiteral("2026-13-01");
    QCOMPARE(normalizeTaskFilter(invalidDate).category(), ErrorCategory::Validation);
}

void TaskDomainTest::taskAutomationValidationAndUrlNormalization()
{
    TaskAutomationInput automation;
    automation.actionType = QStringLiteral("open_url");
    automation.url = QStringLiteral("  HTTPS://example.com/path  ");
    automation.delayMs = 5000;
    QVERIFY(!normalizeTaskAutomation(automation).isError());
    QCOMPARE(automation.url, QStringLiteral("https://example.com/path"));

    automation.url = QStringLiteral("file:///C:/Windows/system.ini");
    QCOMPARE(normalizeTaskAutomation(automation).category(), ErrorCategory::Validation);
    automation.url = QStringLiteral("powershell:Start-Process");
    QCOMPARE(normalizeTaskAutomation(automation).category(), ErrorCategory::Validation);
    automation.url = QStringLiteral("https://example.com/");
    automation.delayMs = 1000;
    QCOMPARE(normalizeTaskAutomation(automation).category(), ErrorCategory::Validation);

    automation.actionType = QStringLiteral("show_notification");
    automation.url.clear();
    automation.delayMs = 0;
    automation.notificationMessage = QStringLiteral("Finished: {task.title} at {task.completedAt}");
    QVERIFY(!normalizeTaskAutomation(automation).isError());
    automation.notificationMessage = QStringLiteral("Unexpected: {steam.gameName}");
    QCOMPARE(normalizeTaskAutomation(automation).category(), ErrorCategory::Validation);
    automation.notificationMessage = QStringLiteral("Unknown: {task.missingField}");
    QCOMPARE(normalizeTaskAutomation(automation).category(), ErrorCategory::Validation);

    automation.configured = false;
    automation.actionType.clear();
    automation.delayMs = -1;
    QVERIFY(!normalizeTaskAutomation(automation).isError());
}

QTEST_GUILESS_MAIN(TaskDomainTest)
#include "task_domain_test.moc"
