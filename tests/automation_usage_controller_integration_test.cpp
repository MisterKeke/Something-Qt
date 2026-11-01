#include "application/application_service_graph.h"
#include "controllers/automation_controller.h"
#include "controllers/application_usage_controller.h"
#include "platform/desktop_applications_platform.h"
#include "services/application_usage_service.h"
#include "storage/repositories.h"

#include <QDate>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <utility>

using namespace something;

namespace {

class FixedClock final : public domain::Clock {
public:
    QDateTime nowUtc() const override { return now; }
    QTimeZone localTimeZone() const override { return QTimeZone::utc(); }

    QDateTime now;
};

class FakeUsagePlatform final : public platform::ApplicationUsagePlatform {
public:
    using ApplicationUsagePlatform::ApplicationUsagePlatform;
    bool supported() const noexcept override { return true; }
    void start() override { active = true; }
    void stop() override { active = false; }

    void publish(const QVector<platform::ApplicationUsageObservation> &items)
    {
        emit observationsReady(items, true, {});
    }

    bool active{false};
};

class FakeDesktopApplicationsPlatform final : public platform::DesktopApplicationsPlatform {
public:
    bool supported() const noexcept override { return true; }
    domain::AppError resolveExecutable(const QString &selectedPath,
                                       QString *canonicalPath) const override
    {
        if (!canonicalPath || selectedPath.isEmpty())
            return domain::AppError(domain::ErrorCategory::Validation);
        *canonicalPath = selectedPath;
        return {};
    }
    QImage extractExecutableIcon(const QString &canonicalPath,
        const application::CancellationToken &cancellation) const override
    {
        Q_UNUSED(canonicalPath);
        Q_UNUSED(cancellation);
        return {};
    }
    domain::AppError launchExecutable(const QString &canonicalPath,
        const application::CancellationToken &cancellation) const override
    {
        if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
        QMutexLocker lock(&m_mutex);
        m_launchedPaths.push_back(canonicalPath);
        return {};
    }
    QStringList launchedPaths() const
    {
        QMutexLocker lock(&m_mutex);
        return m_launchedPaths;
    }

private:
    mutable QMutex m_mutex;
    mutable QStringList m_launchedPaths;
};

domain::AutomationRule urlRule(QString id, QString trigger, QString url,
                                QString applicationId = {})
{
    domain::AutomationRule rule;
    rule.id = std::move(id);
    rule.name = QStringLiteral("Integration rule");
    rule.triggerType = std::move(trigger);
    if (rule.triggerType == QStringLiteral("schedule.time_reached")) {
        rule.triggerConfig = {{QStringLiteral("time"), QStringLiteral("09:00")},
                              {QStringLiteral("weekdays"), QVariantList{1}}};
    } else if (rule.triggerType.startsWith(QStringLiteral("application."))) {
        rule.triggerConfig = {{QStringLiteral("applicationId"), std::move(applicationId)}};
    } else {
        rule.triggerConfig = {{QStringLiteral("taskId"), 0}};
    }
    domain::AutomationAction action;
    action.type = QStringLiteral("open_url");
    action.url = std::move(url);
    rule.actions.push_back(std::move(action));
    return rule;
}

bool seedRule(application::ApplicationServiceGraph &graph,
              const domain::AutomationRule &rule)
{
    const auto operation = graph.beginOperation(QStringLiteral("seed automation integration rule"));
    if (!operation.isValid()) return false;
    QSignalSpy spy(&graph, &application::ApplicationServiceGraph::storageOperationFinished);
    if (!graph.submitStorage(operation, [rule](storage::StorageRepositories &repositories,
                                               const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto saved = repositories.automations.saveRule(rule, 0);
        if (!saved.succeeded()) return storage::StorageReply{{}, {}, saved.error};
        return storage::StorageReply{};
    })) {
        return false;
    }
    if (spy.count() == 0 && !spy.wait(5000)) return false;
    return !spy.last().at(2).value<domain::AppError>().isError();
}

bool waitForStorage(application::ApplicationServiceGraph &graph,
                    const std::function<storage::StorageReply(
                        storage::StorageRepositories &, const std::function<bool()> &)> &command,
                    QVariantMap *values)
{
    const auto operation = graph.beginOperation(QStringLiteral("automation integration fixture"));
    if (!operation.isValid()) return false;
    QSignalSpy finished(&graph, &application::ApplicationServiceGraph::storageOperationFinished);
    if (!graph.submitStorage(operation, command)) return false;
    if (finished.count() == 0 && !finished.wait(5000)) return false;
    if (finished.last().at(2).value<domain::AppError>().isError()) return false;
    if (values) *values = finished.last().at(1).toMap();
    return true;
}

} // namespace

class AutomationUsageControllerIntegrationTest final : public QObject {
    Q_OBJECT

private slots:
    void scheduledStartupAndMissedSlotsAreNotReplayed();
    void productionSchedulerClaimsAndExecutesOneAction();
    void taskCompletionWaitsForTheLatestRuleReload();
    void globalTaskRuleCanLaunchAFakeSavedApplication();
    void foregroundTransitionsRunOnlyOnEntry();
    void usageShutdownFlushPersistsTheActiveSession();
    void usageStatisticsRequestsKeepOverviewAndDetailIndependent();
};

void AutomationUsageControllerIntegrationTest::scheduledStartupAndMissedSlotsAreNotReplayed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("schedule-skip-integration"),
                                               temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    QVERIFY(seedRule(graph, urlRule(QStringLiteral("schedule-rule"),
        QStringLiteral("schedule.time_reached"), QStringLiteral("https://scheduled.example/"))));

    FixedClock clock;
    clock.now = QDateTime(QDate(2026, 6, 15), QTime(9, 0, 37, 842), QTimeZone::utc());
    QStringList opened;
    controllers::AutomationController controller(&graph, nullptr, nullptr, nullptr, nullptr,
        nullptr, [&opened](const QUrl &url) { opened.push_back(url.toString()); return true; }, &clock);
    QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
    QSignalSpy execution(&controller, &controllers::AutomationController::executionChanged);
    QVERIFY(QMetaObject::invokeMethod(&controller, "checkScheduledRules", Qt::DirectConnection));
    clock.now = QDateTime(QDate(2026, 6, 15), QTime(9, 1, 15, 300), QTimeZone::utc());
    QVERIFY(QMetaObject::invokeMethod(&controller, "checkScheduledRules", Qt::DirectConnection));
    QCOMPARE(execution.count(), 0);
    QVERIFY(opened.isEmpty());

    QSignalSpy prepared(&controller,
        &controllers::AutomationController::shutdownPreparationFinished);
    controller.prepareShutdown();
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 5000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void AutomationUsageControllerIntegrationTest::productionSchedulerClaimsAndExecutesOneAction()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("automation-integration"), temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    QVERIFY(seedRule(graph, urlRule(QStringLiteral("schedule-rule"),
        QStringLiteral("schedule.time_reached"), QStringLiteral("https://scheduled.example/"))));

    FixedClock clock;
    clock.now = QDateTime(QDate(2026, 6, 15), QTime(8, 59, 53, 123), QTimeZone::utc());
    QStringList opened;
    controllers::AutomationController controller(&graph, nullptr, nullptr, nullptr, nullptr,
        nullptr, [&opened](const QUrl &url) { opened.push_back(url.toString()); return true; }, &clock);
    QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
    QSignalSpy execution(&controller, &controllers::AutomationController::executionChanged);

    QVERIFY(QMetaObject::invokeMethod(&controller, "checkScheduledRules", Qt::DirectConnection));
    clock.now = QDateTime(QDate(2026, 6, 15), QTime(9, 0, 41, 842), QTimeZone::utc());
    QVERIFY(QMetaObject::invokeMethod(&controller, "checkScheduledRules", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(execution.count(), 2, 5000);
    QCOMPARE(opened, QStringList{QStringLiteral("https://scheduled.example/")});
    QCOMPARE(execution.constLast().at(1).toString(), QStringLiteral("succeeded"));

    QSignalSpy prepared(&controller,
        &controllers::AutomationController::shutdownPreparationFinished);
    controller.prepareShutdown();
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 5000);
    QVERIFY(prepared.constFirst().at(0).toBool());
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void AutomationUsageControllerIntegrationTest::taskCompletionWaitsForTheLatestRuleReload()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("automation-reload-race"), temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    const domain::AutomationRule original = urlRule(QStringLiteral("task-rule"),
        QStringLiteral("task.completed"), QStringLiteral("https://old.example/"));
    QVERIFY(seedRule(graph, original));

    QStringList opened;
    controllers::AutomationController controller(&graph, nullptr, nullptr, nullptr, nullptr,
        nullptr, [&opened](const QUrl &url) { opened.push_back(url.toString()); return true; });
    QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
    QSignalSpy execution(&controller, &controllers::AutomationController::executionChanged);

    domain::AutomationRule updated = original;
    updated.actions[0].url = QStringLiteral("https://new.example/");
    QVERIFY(waitForStorage(graph,
        [updated](storage::StorageRepositories &repositories,
                  const std::function<bool()> &cancelled) {
            if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
            const auto saved = repositories.automations.saveRule(updated, 1);
            if (!saved.succeeded()) return storage::StorageReply{{}, {}, saved.error};
            return storage::StorageReply{};
        }, nullptr));

    controller.reload();
    controller.onTaskCompletionCommitted(QStringLiteral("task:completed:41:revision:2"),
        {{QStringLiteral("id"), 41}, {QStringLiteral("title"), QStringLiteral("Integration task")},
         {QStringLiteral("priority"), QStringLiteral("medium")},
         {QStringLiteral("tags"), QStringList{}}, {QStringLiteral("dueDate"), QString()},
         {QStringLiteral("completed"), true},
         {QStringLiteral("completedAtUtc"), QStringLiteral("2026-06-15T09:00:00.000Z")}},
        false, true);

    QTRY_COMPARE_WITH_TIMEOUT(execution.count(), 2, 5000);
    QCOMPARE(opened, QStringList{QStringLiteral("https://new.example/")});
    QCOMPARE(execution.constLast().at(1).toString(), QStringLiteral("succeeded"));

    QVERIFY(graph.enterProfileMaintenance());
    controller.onTaskCompletionCommitted(QStringLiteral("task:completed:41:revision:3"),
        {{QStringLiteral("id"), 41}, {QStringLiteral("title"), QStringLiteral("Integration task")},
         {QStringLiteral("priority"), QStringLiteral("medium")},
         {QStringLiteral("tags"), QStringList{}}, {QStringLiteral("dueDate"), QString()},
         {QStringLiteral("completed"), true},
         {QStringLiteral("completedAtUtc"), QStringLiteral("2026-06-15T09:00:00.000Z")}},
        false, true);
    QCOMPARE(opened.size(), 1);
    graph.leaveProfileMaintenance();
    QTRY_COMPARE_WITH_TIMEOUT(execution.count(), 4, 5000);
    QCOMPARE(opened, QStringList({QStringLiteral("https://new.example/"),
                                  QStringLiteral("https://new.example/")}));
    QCOMPARE(execution.constLast().at(1).toString(), QStringLiteral("succeeded"));

    QSignalSpy prepared(&controller,
        &controllers::AutomationController::shutdownPreparationFinished);
    controller.prepareShutdown();
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 5000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void AutomationUsageControllerIntegrationTest::globalTaskRuleCanLaunchAFakeSavedApplication()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("global-task-rule-integration"),
                                               temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);

    QVariantMap applicationValues;
    QVERIFY(waitForStorage(graph,
        [](storage::StorageRepositories &repositories,
           const std::function<bool()> &cancelled) {
            if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
            const auto saved = repositories.applications.add(
                QStringLiteral("Discord test target"), QStringLiteral("C:/Fake/Discord.exe"));
            if (!saved.succeeded()) return storage::StorageReply{{}, {}, saved.error};
            storage::StorageReply reply;
            reply.values.insert(QStringLiteral("applicationId"), saved.value.id);
            return reply;
        }, &applicationValues));
    const qint64 applicationId = applicationValues.value(QStringLiteral("applicationId")).toLongLong();
    QVERIFY(applicationId > 0);

    FakeDesktopApplicationsPlatform desktopPlatform;
    controllers::AutomationController controller(&graph, nullptr, nullptr, &desktopPlatform, nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
    QSignalSpy saved(&controller, &controllers::AutomationController::ruleSaved);
    QSignalSpy execution(&controller, &controllers::AutomationController::executionChanged);
    QVariantMap rule{
        {QStringLiteral("name"), QStringLiteral("Finish work -> Open Discord")},
        {QStringLiteral("description"), QStringLiteral("Open Discord after the selected task is completed.")},
        {QStringLiteral("triggerType"), QStringLiteral("task.completed")},
        {QStringLiteral("triggerConfig"), QVariantMap{{QStringLiteral("taskId"), 41}}},
        {QStringLiteral("conditions"), QVariantList{}},
        {QStringLiteral("actions"), QVariantList{QVariantMap{
            {QStringLiteral("type"), QStringLiteral("launch_application")},
            {QStringLiteral("targetId"), applicationId},
            {QStringLiteral("delayMs"), 0},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("order"), 0}}}}
    };
    controller.saveRule(rule);
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 5000);
    QCOMPARE(controller.rules().size(), 1);
    QVariantMap savedRule = controller.rules().constFirst().toMap();
    const QString ruleId = savedRule.value(QStringLiteral("id")).toString();
    QVERIFY(!ruleId.isEmpty());
    QCOMPARE(savedRule.value(QStringLiteral("name")).toString(),
             QStringLiteral("Finish work -> Open Discord"));

    savedRule.insert(QStringLiteral("description"),
                     QStringLiteral("Updated from the rule editor."));
    controller.saveRule(savedRule, savedRule.value(QStringLiteral("revision")).toLongLong());
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 5000);
    controller.setRuleEnabled(ruleId, false);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.rules().constFirst().toMap()
                                  .value(QStringLiteral("enabled")).toBool(), 5000);

    const QVariantMap task{
        {QStringLiteral("id"), 41},
        {QStringLiteral("title"), QStringLiteral("Finish work")},
        {QStringLiteral("priority"), QStringLiteral("medium")},
        {QStringLiteral("tags"), QStringList{}},
        {QStringLiteral("dueDate"), QString()},
        {QStringLiteral("completed"), true},
        {QStringLiteral("completedAtUtc"), QStringLiteral("2026-11-01T09:00:00.000Z")}
    };
    controller.onTaskCompletionCommitted(QStringLiteral("41:revision:2"), task, false, true);
    QTest::qWait(25);
    QVERIFY(desktopPlatform.launchedPaths().isEmpty());

    controller.setRuleEnabled(ruleId, true);
    QTRY_VERIFY_WITH_TIMEOUT(controller.rules().constFirst().toMap()
                                  .value(QStringLiteral("enabled")).toBool(), 5000);
    controller.onTaskCompletionCommitted(QStringLiteral("41:revision:3"), task, false, true);
    QTRY_COMPARE_WITH_TIMEOUT(execution.count(), 2, 5000);
    QCOMPARE(execution.constLast().at(1).toString(), QStringLiteral("succeeded"));
    QCOMPARE(desktopPlatform.launchedPaths(), QStringList{QStringLiteral("C:/Fake/Discord.exe")});

    controller.loadHistory();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.history().isEmpty(), 5000);
    QCOMPARE(controller.history().constFirst().toMap().value(QStringLiteral("status")).toString(),
             QStringLiteral("succeeded"));

    QSignalSpy prepared(&controller,
        &controllers::AutomationController::shutdownPreparationFinished);
    controller.prepareShutdown();
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 5000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void AutomationUsageControllerIntegrationTest::usageShutdownFlushPersistsTheActiveSession()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("usage-shutdown-integration"), temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);

    FakeUsagePlatform platform;
    services::ApplicationUsageService usage(&graph, &platform);
    QTRY_VERIFY_WITH_TIMEOUT(usage.ready() && usage.trackingActive() && platform.active, 5000);
    const QString applicationId = QStringLiteral("path:c:/apps/integration.exe");
    platform.publish({{applicationId, QStringLiteral("C:/Apps/integration.exe"),
        QStringLiteral("integration.exe"), QStringLiteral("Integration App"),
        QStringLiteral("process-1"), QStringLiteral("ForegroundWindowed")}});
    QCOMPARE(usage.liveApplications().size(), 1);
    QTest::qWait(75);

    QVERIFY(graph.enterProfileMaintenance());
    usage.setTrackingEnabled(false);
    QVERIFY(usage.trackingEnabled());
    QVERIFY(!usage.trackingActive());
    QVERIFY(!platform.active);
    graph.leaveProfileMaintenance();
    QTRY_VERIFY_WITH_TIMEOUT(usage.trackingActive() && platform.active, 5000);
    platform.publish({{applicationId, QStringLiteral("C:/Apps/integration.exe"),
        QStringLiteral("integration.exe"), QStringLiteral("Integration App"),
        QStringLiteral("process-2"), QStringLiteral("ForegroundWindowed")}});
    QCOMPARE(usage.liveApplications().size(), 1);
    QTest::qWait(75);

    QSignalSpy flushed(&usage, &services::ApplicationUsageService::shutdownFlushFinished);
    usage.prepareShutdown();
    QTRY_COMPARE_WITH_TIMEOUT(flushed.count(), 1, 5000);
    QVERIFY(flushed.constFirst().at(0).toBool());

    QVariantMap values;
    QVERIFY(waitForStorage(graph,
        [applicationId](storage::StorageRepositories &repositories,
                        const std::function<bool()> &cancelled) {
            if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
            QString diagnostic;
            const QVariantMap detail = repositories.applicationUsage.queryApplication(
                applicationId, QStringLiteral("0001-01-01"), QStringLiteral("9999-12-31"),
                {}, {}, &diagnostic);
            if (!diagnostic.isEmpty())
                return storage::StorageReply{{}, {}, domain::AppError(domain::ErrorCategory::Storage)};
            storage::StorageReply reply;
            reply.values.insert(QStringLiteral("detail"), detail);
            return reply;
        }, &values));
    const QVariantMap detail = values.value(QStringLiteral("detail")).toMap();
    QCOMPARE(detail.value(QStringLiteral("applicationId")).toString(), applicationId);
    QVERIFY(!detail.value(QStringLiteral("running")).toBool());
    QCOMPARE(detail.value(QStringLiteral("sessionCount")).toInt(), 2);
    QVERIFY(detail.value(QStringLiteral("totalMs")).toLongLong() > 0);

    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void AutomationUsageControllerIntegrationTest::usageStatisticsRequestsKeepOverviewAndDetailIndependent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("usage-statistics-race"),
                                               temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);

    FakeUsagePlatform platform;
    services::ApplicationUsageService usage(&graph, &platform);
    QTRY_VERIFY_WITH_TIMEOUT(usage.ready() && usage.trackingActive(), 5000);
    const QString localDay = QDate::currentDate().toString(Qt::ISODate);
    const QString backgroundId = QStringLiteral("path:c:/apps/background-only.exe");
    const QString foregroundId = QStringLiteral("path:c:/apps/foreground.exe");
    storage::ApplicationUsageCheckpoint batch;
    batch.applications.push_back({backgroundId, QStringLiteral("C:/Apps/background-only.exe"),
        QStringLiteral("background-only.exe"), QStringLiteral("Eight minute app"),
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:08:00.000Z")});
    batch.sessions.push_back({QStringLiteral("background-session"), backgroundId,
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:08:00.000Z"),
        QStringLiteral("2026-11-01T08:08:00.000Z"), QStringLiteral("closed"), 480000, false});
    batch.segments.push_back({QStringLiteral("background-segment"),
        QStringLiteral("background-session"), backgroundId, QStringLiteral("BackgroundVisible"),
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:08:00.000Z"),
        localDay, 11, 480000});
    batch.applications.push_back({foregroundId, QStringLiteral("C:/Apps/foreground.exe"),
        QStringLiteral("foreground.exe"), QStringLiteral("Foreground app"),
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:02:00.000Z")});
    batch.sessions.push_back({QStringLiteral("foreground-session"), foregroundId,
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:02:00.000Z"),
        QStringLiteral("2026-11-01T08:02:00.000Z"), QStringLiteral("closed"), 120000, false});
    batch.segments.push_back({QStringLiteral("foreground-segment"),
        QStringLiteral("foreground-session"), foregroundId, QStringLiteral("ForegroundFullscreen"),
        QStringLiteral("2026-11-01T08:00:00.000Z"), QStringLiteral("2026-11-01T08:02:00.000Z"),
        localDay, 11, 120000});
    QVERIFY(waitForStorage(graph,
        [batch](storage::StorageRepositories &repositories,
                const std::function<bool()> &cancelled) {
            if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
            if (!repositories.applicationUsage.recordCheckpoint(batch, 0))
                return storage::StorageReply{{}, {}, domain::AppError(domain::ErrorCategory::Storage)};
            return storage::StorageReply{};
        }, nullptr));

    controllers::ApplicationUsageController controller(&graph, &usage);
    controller.setPageVisible(true);
    controller.loadApplicationChoices();
    controller.selectApplication(backgroundId);
    controller.selectApplication(foregroundId);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.loading(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 5000);

    QCOMPARE(controller.applicationChoices().size(), 2);
    QCOMPARE(controller.overview().value(QStringLiteral("totalMs")).toLongLong(), 600000);
    QCOMPARE(controller.overview().value(QStringLiteral("foregroundMs")).toLongLong(), 120000);
    QCOMPARE(controller.overview().value(QStringLiteral("applicationCount")).toLongLong(), 2);
    QCOMPARE(controller.overview().value(QStringLiteral("mostUsed")).toMap()
                 .value(QStringLiteral("name")).toString(), QStringLiteral("Eight minute app"));
    QCOMPARE(controller.detail().value(QStringLiteral("applicationId")).toString(), foregroundId);
    QCOMPARE(controller.detail().value(QStringLiteral("totalMs")).toLongLong(), 120000);

    controller.selectApplication(backgroundId);
    controller.clearSelection();
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 5000);
    QVERIFY(controller.detail().isEmpty());
    QCOMPARE(controller.overview().value(QStringLiteral("totalMs")).toLongLong(), 600000);

    controller.selectApplication(backgroundId);
    controller.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.loading(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(graph.operations().activeCount(), 0, 5000);
    QCOMPARE(controller.detail().value(QStringLiteral("totalMs")).toLongLong(), 480000);
    QCOMPARE(controller.overview().value(QStringLiteral("totalMs")).toLongLong(), 600000);

    QSignalSpy flushed(&usage, &services::ApplicationUsageService::shutdownFlushFinished);
    usage.prepareShutdown();
    QTRY_COMPARE_WITH_TIMEOUT(flushed.count(), 1, 5000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void AutomationUsageControllerIntegrationTest::foregroundTransitionsRunOnlyOnEntry()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("foreground-entry-integration"),
                                               temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    const QString applicationId = QStringLiteral("path:c:/apps/integration.exe");
    QVERIFY(seedRule(graph, urlRule(QStringLiteral("foreground-rule"),
        QStringLiteral("application.foreground_entered"), QStringLiteral("https://foreground.example/"),
        applicationId)));

    FakeUsagePlatform platform;
    services::ApplicationUsageService usage(&graph, &platform);
    QStringList opened;
    controllers::AutomationController controller(&graph, nullptr, &usage, nullptr, nullptr,
        nullptr, [&opened](const QUrl &url) { opened.push_back(url.toString()); return true; });
    QTRY_VERIFY_WITH_TIMEOUT(usage.ready() && usage.trackingActive() && controller.ready(), 5000);
    QSignalSpy execution(&controller, &controllers::AutomationController::executionChanged);
    const auto publishState = [&platform, &applicationId](const QString &state) {
        platform.publish({{applicationId, QStringLiteral("C:/Apps/integration.exe"),
            QStringLiteral("integration.exe"), QStringLiteral("Integration App"),
            QStringLiteral("process-1"), state}});
    };

    publishState(QStringLiteral("ForegroundWindowed"));
    QTRY_COMPARE_WITH_TIMEOUT(execution.count(), 2, 5000);
    QTest::qWait(10);
    publishState(QStringLiteral("ForegroundFullscreen"));
    QCOMPARE(execution.count(), 2);
    QTest::qWait(10);
    publishState(QStringLiteral("Minimized"));
    QTest::qWait(5100); // Let the production foreground cooldown expire.
    publishState(QStringLiteral("ForegroundWindowed"));
    QTRY_COMPARE_WITH_TIMEOUT(execution.count(), 4, 5000);
    QCOMPARE(opened, QStringList({QStringLiteral("https://foreground.example/"),
                                  QStringLiteral("https://foreground.example/")}));
    QCOMPARE(execution.constLast().at(1).toString(), QStringLiteral("succeeded"));

    QSignalSpy prepared(&controller,
        &controllers::AutomationController::shutdownPreparationFinished);
    QSignalSpy flushed(&usage, &services::ApplicationUsageService::shutdownFlushFinished);
    controller.prepareShutdown();
    usage.prepareShutdown();
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(flushed.count(), 1, 5000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

QTEST_GUILESS_MAIN(AutomationUsageControllerIntegrationTest)
#include "automation_usage_controller_integration_test.moc"
