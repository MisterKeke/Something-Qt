#include "application/command_parser.h"
#include "application/command_registry.h"
#include "application/command_completion.h"
#include "application/application_service_graph.h"
#include "services/application_command_service.h"
#include "services/application_mutation_workflow.h"
#include "services/desktop_applications_service.h"
#include "services/task_automation_service.h"
#include "controllers/tasks_controller.h"
#include "controllers/terminal_controller.h"
#include "platform/desktop_applications_platform.h"

#include <QFile>
#include <QDir>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSignalSpy>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <functional>

using namespace something;

class TestClock final : public domain::Clock {
public:
    QDateTime nowUtc() const override { return QDateTime::currentDateTimeUtc(); }
    QTimeZone localTimeZone() const override { return QTimeZone::utc(); }
};

class FakeAutomationPlatform final : public platform::DesktopApplicationsPlatform {
public:
    explicit FakeAutomationPlatform(std::atomic_int *launchCount)
        : m_launchCount(launchCount) {}
    bool supported() const noexcept override { return true; }
    domain::AppError resolveExecutable(const QString &selectedPath,
                                       QString *canonicalPath) const override
    {
        if (!canonicalPath || selectedPath.isEmpty())
            return domain::AppError(domain::ErrorCategory::Validation);
        *canonicalPath = selectedPath;
        return domain::AppError::none();
    }
    QImage extractExecutableIcon(const QString &,
        const application::CancellationToken &) const override { return {}; }
    domain::AppError launchExecutable(const QString &,
        const application::CancellationToken &cancellation) const override
    {
        if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
        ++*m_launchCount;
        return domain::AppError::none();
    }

private:
    std::atomic_int *m_launchCount{nullptr};
};

class FakeAutomationScheduler final {
public:
    struct Entry final {
        int delayMs{0};
        std::function<void()> callback;
    };

    int schedule(int delayMs, std::function<void()> callback)
    {
        const int id = ++m_nextId;
        m_entries.insert(id, {delayMs, std::move(callback)});
        return id;
    }
    void cancel(int id) { m_entries.remove(id); }
    int count() const { return m_entries.size(); }
    int firstDelay() const { return m_entries.isEmpty() ? -1 : m_entries.cbegin()->delayMs; }
    bool fireFirst()
    {
        if (m_entries.isEmpty()) return false;
        auto it = m_entries.begin();
        std::function<void()> callback = std::move(it->callback);
        m_entries.erase(it);
        callback();
        return true;
    }

private:
    int m_nextId{0};
    QMap<int, Entry> m_entries;
};

class ApplicationCommandTest final : public QObject {
    Q_OBJECT
private slots:
    void parserHandlesWordsQuotesAndEscapes();
    void parserRejectsUnsupportedSyntaxAndLimits();
    void parserPreservesUnicodeAndRejectsShellGrammar();
    void registryRequiresExplicitValidUniqueDefinitions();
    void completionHandlesPartialTokensQuotesAndUnknownCommands();
    void serviceRejectsInvalidAndUnavailableCommands();
    void runFullIsDiscoverableAndOnlyRequestsTheFullWindow();
    void queryFailuresExposeStructuredErrorCodes();
    void acceptedStorageRequestHasOneFinalResult();
    void sharedMutationWorkflowCorrelatesCommittedValuesAndConflicts();
    void workflowReportsMaintenanceAndPendingBoundOutcomes();
    void failedTaskCompletionTransactionDoesNotEmitAutomationEvent();
    void guiAndConsoleUseSharedTaskWorkflowWithoutClosingAnotherDraft();
    void taskAutomationUsesFakeActionsAndCancelsStaleDelays();
    void terminalClearDuringPendingPreservesHistoryAndLateResult();
};

void ApplicationCommandTest::parserHandlesWordsQuotesAndEscapes()
{
    const auto parsed = application::CommandParser::parse(
        QStringLiteral("task list --query \"hello world\" '' one\\ two 'a\\'b'"));
    QVERIFY(parsed.succeeded());
    QCOMPARE(parsed.tokens, QStringList({QStringLiteral("task"), QStringLiteral("list"),
        QStringLiteral("--query"), QStringLiteral("hello world"), QString(),
        QStringLiteral("one two"), QStringLiteral("a'b")}));
}

void ApplicationCommandTest::parserRejectsUnsupportedSyntaxAndLimits()
{
    QVERIFY(!application::CommandParser::parse(QStringLiteral("task 'unfinished")).succeeded());
    QVERIFY(!application::CommandParser::parse(QStringLiteral("task list | status")).succeeded());
    QVERIFY(!application::CommandParser::parse(QStringLiteral("task \\q")).succeeded());
    QVERIFY(!application::CommandParser::parse(QString(4097, u'x')).succeeded());
    QCOMPARE(application::CommandParser::parse(QString(4096, u'x')).succeeded(), true);
    QString many;
    for (int i = 0; i < 65; ++i) many += QStringLiteral("x ");
    QVERIFY(!application::CommandParser::parse(many).succeeded());
    many.clear();
    for (int i = 0; i < 64; ++i) many += QStringLiteral("x ");
    QVERIFY(application::CommandParser::parse(many).succeeded());
}

void ApplicationCommandTest::parserPreservesUnicodeAndRejectsShellGrammar()
{
    const QString unicode = QString::fromUtf8("task add \"Купить чай 🍵\" '東京'");
    const auto parsed = application::CommandParser::parse(unicode);
    QVERIFY(parsed.succeeded());
    QCOMPARE(parsed.tokens, QStringList({QStringLiteral("task"), QStringLiteral("add"),
        QString::fromUtf8("Купить чай 🍵"), QString::fromUtf8("東京")}));
    const auto literalOperators = application::CommandParser::parse(
        QStringLiteral("task add \"title; $HOME | >\""));
    QVERIFY(literalOperators.succeeded()); // quoted characters are argument data, never syntax
    for (const QString &value : {QStringLiteral("x; status"), QStringLiteral("x && y"),
                                 QStringLiteral("x | y"), QStringLiteral("x > y"),
                                 QStringLiteral("$HOME"), QStringLiteral("x \\| y")})
        QVERIFY2(!application::CommandParser::parse(value).succeeded(), qPrintable(value));
}

void ApplicationCommandTest::registryRequiresExplicitValidUniqueDefinitions()
{
    application::CommandRegistry registry;
    application::CommandDefinition valid{
        QStringLiteral("status"), QStringLiteral("Show state."), QStringLiteral("status"),
        [](const QStringList &args) { return args.isEmpty() ? QString() : QStringLiteral("No arguments."); },
        [](const QStringList &) { return application::CommandExecution{}; }};
    QString error;
    QVERIFY(registry.registerCommand(valid, &error));
    QVERIFY(registry.find(QStringLiteral("status")));
    const auto *registered = registry.find(QStringLiteral("status"));
    QVERIFY(registry.validate(*registered, {}).isEmpty());
    QCOMPARE(registry.validate(*registered, {QStringLiteral("extra")}),
             QStringLiteral("No arguments."));
    QVERIFY(!registry.validate(valid, {}).isEmpty()); // A copied definition is not registered.
    QCOMPARE(registry.names(), QStringList{QStringLiteral("status")});
    QVERIFY(!registry.registerCommand(valid, &error));
    valid.name = QStringLiteral("Status");
    QVERIFY(!registry.registerCommand(valid, &error));
    valid.name = QStringLiteral("other");
    valid.handler = {};
    QVERIFY(!registry.registerCommand(valid, &error));
}

void ApplicationCommandTest::completionHandlesPartialTokensQuotesAndUnknownCommands()
{
    application::CommandRegistry registry;
    int invocations = 0;
    QVERIFY(registry.registerCommand({QStringLiteral("page"), QStringLiteral("Pages."),
        QStringLiteral("page open <route>"), [](const QStringList &) { return QString(); },
        [&invocations](const QStringList &) { ++invocations; return application::CommandExecution{}; },
        [](const QStringList &complete, const QString &) {
            if (complete == QStringList{QStringLiteral("open")})
                return QStringList{QStringLiteral("dashboard"), QStringLiteral("terminal")};
            return QStringList{QStringLiteral("open")};
        }}));
    QVERIFY(registry.registerCommand({QStringLiteral("status"), QStringLiteral("State."),
        QStringLiteral("status"), [](const QStringList &) { return QString(); },
        [&invocations](const QStringList &) { ++invocations; return application::CommandExecution{}; }}));

    auto result = application::CommandCompletion::complete(registry, QStringLiteral("pa"), 2);
    const QVariantList commandItems = result.value(QStringLiteral("suggestions")).toList();
    QCOMPARE(commandItems.size(), 1);
    QCOMPARE(commandItems.first().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("page"));

    result = application::CommandCompletion::complete(registry, QStringLiteral("page open ter"), 13);
    QCOMPARE(result.value(QStringLiteral("suggestions")).toList().size(), 1);
    result = application::CommandCompletion::complete(registry, QStringLiteral("page open \"ter"), 14);
    QCOMPARE(result.value(QStringLiteral("suggestions")).toList().size(), 1);
    const auto suggestion = result.value(QStringLiteral("suggestions")).toList().first().toMap();
    QCOMPARE(application::CommandCompletion::apply(QStringLiteral("page open \"ter"), suggestion),
             QStringLiteral("page open \"terminal\""));

    result = application::CommandCompletion::complete(registry, QStringLiteral("staus"), 5);
    QCOMPARE(result.value(QStringLiteral("suggestions")).toList().size(), 1);
    QVERIFY(application::CommandCompletion::complete(registry, QStringLiteral("sta;"), 4)
                .value(QStringLiteral("suggestions")).toList().isEmpty());
    QCOMPARE(invocations, 0); // suggestions never invoke a registered handler

    application::ApplicationServiceGraph graph;
    services::ApplicationCommandService service(&graph);
    const auto suggestionsFor = [&service](const QString &input) {
        QStringList texts;
        const auto result = application::CommandCompletion::complete(service.registry(), input, input.size());
        for (const QVariant &value : result.value(QStringLiteral("suggestions")).toList())
            texts.push_back(value.toMap().value(QStringLiteral("text")).toString());
        return texts;
    };
    QVERIFY(suggestionsFor(QStringLiteral("task add ")).isEmpty());
    QCOMPARE(suggestionsFor(QStringLiteral("task add title --priority ")),
             QStringList({QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}));
    QCOMPARE(suggestionsFor(QStringLiteral("task add title --priority high ")),
             QStringList({QStringLiteral("--due")}));
    QVERIFY(suggestionsFor(QStringLiteral("task add title --due ")).isEmpty());
    QCOMPARE(suggestionsFor(QStringLiteral("task add \"--priority\" ")),
             QStringList({QStringLiteral("--priority"), QStringLiteral("--due")}));
    QVERIFY(suggestionsFor(QStringLiteral("settings week-start 4 ")).isEmpty());
}

void ApplicationCommandTest::runFullIsDiscoverableAndOnlyRequestsTheFullWindow()
{
    application::ApplicationServiceGraph graph;
    services::ApplicationCommandService service(&graph);
    QSignalSpy accepted(&service, &services::ApplicationCommandService::requestAccepted);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);
    QSignalSpy rejected(&service, &services::ApplicationCommandService::requestRejected);
    QSignalSpy fullWindow(&service, &services::ApplicationCommandService::fullWindowRequested);
    QSignalSpy route(&service, &services::ApplicationCommandService::routeRequested);
    QSignalSpy storage(&graph, &application::ApplicationServiceGraph::storageOperationFinished);

    service.submit(QStringLiteral("help run"));
    QCOMPARE(finished.size(), 1);
    const auto help = qvariant_cast<services::ConsoleRequestResult>(finished.takeFirst().at(0));
    QCOMPARE(help.data.value(QStringLiteral("usage")).toString(), QStringLiteral("run full"));
    accepted.clear();
    const auto completion = application::CommandCompletion::complete(
        service.registry(), QStringLiteral("run f"), 5);
    const auto suggestions = completion.value(QStringLiteral("suggestions")).toList();
    QCOMPARE(suggestions.size(), 1);
    QCOMPARE(suggestions.first().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("full"));

    for (const QString &invalid : {QStringLiteral("run"), QStringLiteral("run full extra"),
                                  QStringLiteral("run calc.exe"), QStringLiteral("run full --page 1")})
        service.submit(invalid);
    QCOMPARE(rejected.size(), 4);
    for (const auto &row : rejected) {
        const auto result = qvariant_cast<services::ConsoleRequestResult>(row.at(0));
        QCOMPARE(result.data.value(QStringLiteral("errorCode")).toString(), QStringLiteral("validation"));
    }
    QVERIFY(fullWindow.isEmpty());
    QVERIFY(accepted.isEmpty());

    // Showing the UI must work during startup and after startup failure, without
    // querying storage or changing the user's selected main-page route.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const QString id = service.submit(QStringLiteral("run full"));
        QCOMPARE(accepted.size(), attempt + 1);
        QCOMPARE(finished.size(), attempt + 1);
        QCOMPARE(fullWindow.size(), attempt + 1);
        const auto result = qvariant_cast<services::ConsoleRequestResult>(finished.last().at(0));
        QCOMPARE(result.requestId, id);
        QCOMPARE(result.state, QStringLiteral("succeeded"));
        QVERIFY(result.data.value(QStringLiteral("openFullWindow")).toBool());
        if (attempt == 0)
            QVERIFY(graph.lifecycle().failStartup(domain::AppError(domain::ErrorCategory::Storage)));
    }
    QVERIFY(route.isEmpty());
    QVERIFY(storage.isEmpty());
    graph.shutdown();
    service.submit(QStringLiteral("run full"));
    QCOMPARE(fullWindow.size(), 2);
    QCOMPARE(finished.size(), 2);
    QCOMPARE(rejected.size(), 5);
    const auto shutdown = qvariant_cast<services::ConsoleRequestResult>(rejected.last().at(0));
    QCOMPARE(shutdown.data.value(QStringLiteral("errorCode")).toString(), QStringLiteral("shutdown"));
}

void ApplicationCommandTest::serviceRejectsInvalidAndUnavailableCommands()
{
    application::ApplicationServiceGraph graph;
    services::ApplicationMutationWorkflow mutations(&graph);
    services::ApplicationCommandService service(&graph);
    service.setMutationWorkflow(&mutations);
    QSignalSpy rejected(&service, &services::ApplicationCommandService::requestRejected);
    QSignalSpy accepted(&service, &services::ApplicationCommandService::requestAccepted);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);

    const QString badArgsId = service.submit(QStringLiteral("task list --page 0"));
    const QString startupId = service.submit(QStringLiteral("task list"));
    const QString invalidMutationId = service.submit(QStringLiteral("task complete 1 --revision 0"));
    const QString invalidSettingId = service.submit(QStringLiteral("settings week-start 8"));
    const QString emptyTitleId = service.submit(QStringLiteral("task add \"   \""));
    const QString invalidOptionId = service.submit(QStringLiteral("task add item --unknown value"));
    const QString startupMutationId = service.submit(QStringLiteral("task add \"startup\""));
    const QString unknownId = service.submit(QStringLiteral("not-registered"));
    const QString shellLikeId = service.submit(QStringLiteral("task list; status"));
    const QString syntaxId = service.submit(QStringLiteral("task list | status"));
    QVERIFY(!badArgsId.isEmpty());
    QVERIFY(!startupId.isEmpty());
    QVERIFY(!invalidMutationId.isEmpty());
    QVERIFY(!invalidSettingId.isEmpty());
    QVERIFY(!emptyTitleId.isEmpty());
    QVERIFY(!invalidOptionId.isEmpty());
    QVERIFY(!startupMutationId.isEmpty());
    QVERIFY(!unknownId.isEmpty());
    QVERIFY(!shellLikeId.isEmpty());
    QVERIFY(!syntaxId.isEmpty());
    QCOMPARE(rejected.size(), 10);
    bool sawValidationCode = false;
    bool sawStartupCode = false;
    bool sawUnknownCode = false;
    bool sawSyntaxCode = false;
    for (const auto &row : rejected) {
        const auto result = qvariant_cast<services::ConsoleRequestResult>(row.at(0));
        sawValidationCode |= result.requestId == emptyTitleId
            && result.data.value(QStringLiteral("errorCode")).toString() == QStringLiteral("validation");
        sawStartupCode |= result.requestId == startupMutationId
            && result.data.value(QStringLiteral("errorCode")).toString() == QStringLiteral("starting");
        sawUnknownCode |= result.requestId == unknownId
            && result.data.value(QStringLiteral("errorCode")).toString() == QStringLiteral("unknown-command");
        sawSyntaxCode |= result.requestId == syntaxId
            && result.data.value(QStringLiteral("errorCode")).toString() == QStringLiteral("syntax");
    }
    QVERIFY(sawValidationCode);
    QVERIFY(sawStartupCode);
    QVERIFY(sawUnknownCode);
    QVERIFY(sawSyntaxCode);
    QCOMPARE(accepted.size(), 0);

    const QString helpId = service.submit(QStringLiteral("help"));
    QVERIFY(!helpId.isEmpty());
    QCOMPARE(accepted.size(), 1);
    QCOMPARE(finished.size(), 1);
    const auto finalResult = qvariant_cast<services::ConsoleRequestResult>(finished.constFirst().constFirst());
    QCOMPARE(finalResult.requestId, helpId);
    QCOMPARE(finalResult.state, QStringLiteral("succeeded"));
}

void ApplicationCommandTest::queryFailuresExposeStructuredErrorCodes()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("console-query-error-test"), profile.path());
    services::ApplicationCommandService service(&graph);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY2(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running,
             qPrintable(graph.lifecycle().startupError().userMessage()));

    const QString requestId = service.submit(QStringLiteral("task show 999999"));
    QVERIFY(!requestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    const auto result = qvariant_cast<services::ConsoleRequestResult>(finished.constFirst().constFirst());
    QCOMPARE(result.requestId, requestId);
    QCOMPARE(result.state, QStringLiteral("failed"));
    QCOMPARE(result.data.value(QStringLiteral("errorCode")).toString(), QStringLiteral("not-found"));
    QVERIFY(!result.message.isEmpty());
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ApplicationCommandTest::acceptedStorageRequestHasOneFinalResult()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("console-test"), profile.path());
    services::ApplicationCommandService service(&graph);
    QSignalSpy accepted(&service, &services::ApplicationCommandService::requestAccepted);
    QSignalSpy pending(&service, &services::ApplicationCommandService::requestPending);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);
    QSignalSpy cancelled(&service, &services::ApplicationCommandService::requestCancelled);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY2(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running,
             qPrintable(graph.lifecycle().startupError().userMessage()));

    const QString requestId = service.submit(QStringLiteral("task list --page 1"));
    QVERIFY(!requestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size() + cancelled.size(), 1, 5000);
    QCOMPARE(accepted.size(), 1);
    QCOMPARE(pending.size(), 1);
    QCOMPARE(finished.size() + cancelled.size(), 1);
    if (finished.size() == 1) {
        const auto result = qvariant_cast<services::ConsoleRequestResult>(finished.constFirst().constFirst());
        QCOMPARE(result.requestId, requestId);
        QCOMPARE(result.state, QStringLiteral("succeeded"));
        QCOMPARE(result.data.value(QStringLiteral("page")).toInt(), 1);
        QVERIFY(result.data.contains(QStringLiteral("tasks")));
    }
    const QString cancelledId = service.submit(QStringLiteral("task list"));
    QVERIFY(service.cancel(cancelledId));
    QTRY_COMPARE_WITH_TIMEOUT(cancelled.size(), 1, 5000);
    QCOMPARE(finished.size(), 1);
    const auto cancelledResult = qvariant_cast<services::ConsoleRequestResult>(
        cancelled.constFirst().constFirst());
    QCOMPARE(cancelledResult.requestId, cancelledId);
    QCOMPARE(cancelledResult.state, QStringLiteral("cancelled"));
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ApplicationCommandTest::sharedMutationWorkflowCorrelatesCommittedValuesAndConflicts()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("console-mutation-test"), profile.path());
    services::ApplicationMutationWorkflow workflow(&graph);
    services::ApplicationCommandService service(&graph);
    service.setMutationWorkflow(&workflow);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);
    QSignalSpy cancelledResults(&service, &services::ApplicationCommandService::requestCancelled);
    QSignalSpy commits(&workflow, &services::ApplicationMutationWorkflow::taskCommitted);
    QSignalSpy completionEvents(&workflow,
        &services::ApplicationMutationWorkflow::taskCompletionCommitted);
    int dependentViewsNotified = 0;
    connect(&workflow, &services::ApplicationMutationWorkflow::taskCommitted, &workflow,
            [&dependentViewsNotified](const QString &, const QVariantMap &, bool) {
        dependentViewsNotified += 3; // Tasks, Calendar, and Dashboard event consumers
    });

    storage::TaskInput invalid;
    auto admission = workflow.addTask(QStringLiteral("invalid-request"), invalid);
    QVERIFY(!admission.accepted);
    QCOMPARE(admission.errorCode, QStringLiteral("validation"));

    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY2(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running,
             qPrintable(graph.lifecycle().startupError().userMessage()));
    const QString addId = service.submit(QStringLiteral("task add \"Workflow task\""));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    auto added = qvariant_cast<services::ConsoleRequestResult>(finished.at(0).at(0));
    QCOMPARE(added.requestId, addId);
    QVERIFY2(added.state == QStringLiteral("succeeded"), qPrintable(added.message));
    const QVariantMap committed = added.data.value(QStringLiteral("task")).toMap();
    QCOMPARE(committed.value(QStringLiteral("revision")).toLongLong(), 1);
    QCOMPARE(committed.value(QStringLiteral("priority")).toString(), QStringLiteral("medium"));
    QCOMPARE(committed.value(QStringLiteral("description")).toString(), QStringLiteral(""));
    QCOMPARE(committed.value(QStringLiteral("dueDate")).toString(), QString());
    QCOMPARE(commits.size(), 1);
    QCOMPARE(dependentViewsNotified, 3);

    const QString completeId = service.submit(QStringLiteral("task complete %1 --revision 1")
        .arg(committed.value(QStringLiteral("id")).toLongLong()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    const auto completed = qvariant_cast<services::ConsoleRequestResult>(finished.at(1).at(0));
    QCOMPARE(completed.requestId, completeId);
    QCOMPARE(completed.data.value(QStringLiteral("task")).toMap()
                 .value(QStringLiteral("revision")).toLongLong(), 2);
    QCOMPARE(completionEvents.size(), 1);
    QCOMPARE(completionEvents.at(0).at(2).toBool(), false);
    QCOMPARE(completionEvents.at(0).at(3).toBool(), true);

    service.submit(QStringLiteral("task complete %1 --revision 1")
        .arg(committed.value(QStringLiteral("id")).toLongLong()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 5000);
    const auto conflict = qvariant_cast<services::ConsoleRequestResult>(finished.at(2).at(0));
    QCOMPARE(conflict.state, QStringLiteral("failed"));
    QCOMPARE(conflict.data.value(QStringLiteral("errorCode")).toString(), QStringLiteral("conflict"));
    QCOMPARE(completionEvents.size(), 1); // Failed stale revision emits no completion trigger.
    const QString weekStartId = service.submit(QStringLiteral("settings week-start 4"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 5000);
    const auto settingResult = qvariant_cast<services::ConsoleRequestResult>(finished.at(3).at(0));
    QCOMPARE(settingResult.requestId, weekStartId);
    QCOMPARE(settingResult.data.value(QStringLiteral("weekStart")).toInt(), 4);
    const QString cancelId = service.submit(QStringLiteral("task add \"Possibly committed\""));
    QVERIFY(service.cancel(cancelId));
    QTRY_COMPARE_WITH_TIMEOUT(cancelledResults.size(), 1, 5000);
    const auto cancelled = qvariant_cast<services::ConsoleRequestResult>(cancelledResults.at(0).at(0));
    QCOMPARE(cancelled.requestId, cancelId);
    QCOMPARE(cancelled.state, QStringLiteral("cancelled"));
    QVERIFY(cancelled.message.contains(QStringLiteral("may already have committed")));
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ApplicationCommandTest::workflowReportsMaintenanceAndPendingBoundOutcomes()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("workflow-admission-test"), profile.path());
    services::ApplicationMutationWorkflow workflow(&graph);
    QSignalSpy finished(&workflow, &services::ApplicationMutationWorkflow::requestFinished);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY2(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running,
             qPrintable(graph.lifecycle().startupError().userMessage()));

    QVERIFY(graph.enterProfileMaintenance());
    auto blocked = workflow.addTask(QStringLiteral("maintenance-request"),
        storage::TaskInput{QStringLiteral("Maintenance task")});
    QVERIFY(!blocked.accepted);
    QCOMPARE(blocked.errorCode, QStringLiteral("maintenance"));
    graph.leaveProfileMaintenance();

    QStringList ids;
    for (int index = 0; index < services::ApplicationMutationWorkflow::MaximumPendingRequests; ++index) {
        const QString id = QStringLiteral("bounded-%1").arg(index);
        const auto admission = workflow.addTask(id, storage::TaskInput{QStringLiteral("Task %1").arg(index)});
        QVERIFY(admission.accepted);
        ids.push_back(id);
    }
    const auto saturated = workflow.addTask(QStringLiteral("over-bound"),
        storage::TaskInput{QStringLiteral("Over bound")});
    QVERIFY(!saturated.accepted);
    QCOMPARE(saturated.errorCode, QStringLiteral("busy"));
    for (const QString &id : ids) QVERIFY(workflow.cancel(id));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), ids.size(), 5000);
    QSet<QString> finals;
    for (const auto &row : finished) {
        const auto result = qvariant_cast<services::MutationResult>(row.at(0));
        QVERIFY(ids.contains(result.requestId));
        QVERIFY(!finals.contains(result.requestId));
        finals.insert(result.requestId);
        QCOMPARE(result.state, QStringLiteral("cancelled"));
        QVERIFY(result.message.contains(QStringLiteral("may already have committed")));
    }
    QCOMPARE(finals.size(), ids.size());
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ApplicationCommandTest::failedTaskCompletionTransactionDoesNotEmitAutomationEvent()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("failed-task-completion-test"), profile.path());
    services::ApplicationMutationWorkflow workflow(&graph);
    QSignalSpy taskCommits(&workflow, &services::ApplicationMutationWorkflow::taskCommitted);
    QSignalSpy completionEvents(&workflow,
        &services::ApplicationMutationWorkflow::taskCompletionCommitted);
    QSignalSpy results(&workflow, &services::ApplicationMutationWorkflow::requestFinished);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running);

    storage::TaskInput input;
    input.title = QStringLiteral("Rollback task completion");
    QVERIFY(workflow.addTask(QStringLiteral("failed-completion-add"), input).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(taskCommits.size(), 1, 5000);
    const QVariantMap task = taskCommits.first().at(1).toMap();
    results.clear();

    const QString connectionName = QStringLiteral("failed-task-completion-trigger");
    {
        QSqlDatabase external = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        external.setDatabaseName(QDir(graph.profileDirectory()).filePath(QStringLiteral("something.sqlite")));
        QVERIFY(external.open());
        QSqlQuery trigger(external);
        QVERIFY(trigger.exec(QStringLiteral(
            "CREATE TRIGGER reject_task_completion BEFORE INSERT ON task_history "
            "WHEN NEW.event_type='task_completed' "
            "BEGIN SELECT RAISE(ABORT,'test completion rejection'); END")));
    }
    QSqlDatabase::removeDatabase(connectionName);

    QVERIFY(workflow.completeTask(QStringLiteral("failed-completion-attempt"),
        task.value(QStringLiteral("id")).toLongLong(),
        task.value(QStringLiteral("revision")).toLongLong(), true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results.first().at(0).value<services::MutationResult>().state,
             QStringLiteral("failed"));
    QVERIFY(completionEvents.isEmpty());

    {
        QSqlDatabase cleanup = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        cleanup.setDatabaseName(QDir(graph.profileDirectory()).filePath(QStringLiteral("something.sqlite")));
        QVERIFY(cleanup.open());
        QSqlQuery removeTrigger(cleanup);
        QVERIFY(removeTrigger.exec(QStringLiteral("DROP TRIGGER reject_task_completion")));
    }
    QSqlDatabase::removeDatabase(connectionName);

    QVERIFY(workflow.completeTask(QStringLiteral("successful-completion-retry"),
        task.value(QStringLiteral("id")).toLongLong(),
        task.value(QStringLiteral("revision")).toLongLong(), true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 1, 5000);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ApplicationCommandTest::guiAndConsoleUseSharedTaskWorkflowWithoutClosingAnotherDraft()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("shared-mutation-test"), profile.path());
    services::ApplicationMutationWorkflow workflow(&graph);
    services::ApplicationCommandService console(&graph);
    console.setMutationWorkflow(&workflow);
    TestClock clock;
    controllers::TasksController guiTasks(&graph, &clock, &workflow);
    QSignalSpy editorClosed(&guiTasks, &controllers::TasksController::editorSaveSucceeded);
    QSignalSpy viewEvents(&guiTasks, &controllers::TasksController::taskMutationCommitted);
    QSignalSpy consoleResults(&console, &services::ApplicationCommandService::requestFinished);
    QSignalSpy consoleCancelled(&console, &services::ApplicationCommandService::requestCancelled);
    QSignalSpy uncertainEvents(&guiTasks, &controllers::TasksController::taskMutationMayHaveCommitted);
    QSignalSpy completionEvents(&workflow,
        &services::ApplicationMutationWorkflow::taskCompletionCommitted);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY2(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running,
             qPrintable(graph.lifecycle().startupError().userMessage()));

    guiTasks.saveTask({{QStringLiteral("title"), QStringLiteral("GUI task")}});
    QTRY_COMPARE_WITH_TIMEOUT(editorClosed.size(), 1, 5000);
    QCOMPARE(viewEvents.size(), 1);

    const QString requestId = console.submit(QStringLiteral("task add \"Console task\""));
    QTRY_COMPARE_WITH_TIMEOUT(consoleResults.size(), 1, 5000);
    QCOMPARE(qvariant_cast<services::ConsoleRequestResult>(consoleResults.at(0).at(0)).requestId,
             requestId);
    QCOMPARE(editorClosed.size(), 1); // Console completion cannot close a separate GUI editor.
    QCOMPARE(viewEvents.size(), 2);   // Existing task mutation event reaches view consumers.

    const QString cancelledId = console.submit(QStringLiteral("task add \"May have committed\""));
    QVERIFY(console.cancel(cancelledId));
    QTRY_COMPARE_WITH_TIMEOUT(consoleCancelled.size(), 1, 5000);
    QCOMPARE(qvariant_cast<services::ConsoleRequestResult>(consoleCancelled.at(0).at(0)).requestId,
             cancelledId);
    QCOMPARE(uncertainEvents.size(), 1); // A stale/suppressed mutation result triggers read reconciliation.
    QCOMPARE(editorClosed.size(), 1);

    const QVariantMap guiTask = viewEvents.at(0).at(1).toMap();
    const qint64 guiTaskId = guiTask.value(QStringLiteral("id")).toLongLong();
    guiTasks.toggleTaskFromView(guiTaskId, guiTask.value(QStringLiteral("revision")).toLongLong(), true);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 1, 5000);
    QCOMPARE(completionEvents.at(0).at(2).toBool(), false);
    QCOMPARE(completionEvents.at(0).at(3).toBool(), true);

    const int beforeRepeatedComplete = viewEvents.size();
    const QVariantMap completedTask = viewEvents.last().at(1).toMap();
    guiTasks.toggleTaskFromView(guiTaskId,
        completedTask.value(QStringLiteral("revision")).toLongLong(), true);
    QTRY_COMPARE_WITH_TIMEOUT(viewEvents.size(), beforeRepeatedComplete + 1, 5000);
    QCOMPARE(completionEvents.size(), 1);

    const QVariantMap repeatedTask = viewEvents.last().at(1).toMap();
    guiTasks.toggleTaskFromView(guiTaskId,
        repeatedTask.value(QStringLiteral("revision")).toLongLong(), false);
    QTRY_COMPARE_WITH_TIMEOUT(viewEvents.size(), beforeRepeatedComplete + 2, 5000);
    const QVariantMap reopenedTask = viewEvents.last().at(1).toMap();
    guiTasks.toggleTaskFromView(guiTaskId,
        reopenedTask.value(QStringLiteral("revision")).toLongLong(), true);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 2, 5000);

    const QVariantMap consoleTask = viewEvents.at(1).at(1).toMap();
    const QString consoleCompletion = console.submit(QStringLiteral("task complete %1 --revision %2")
        .arg(consoleTask.value(QStringLiteral("id")).toLongLong())
        .arg(consoleTask.value(QStringLiteral("revision")).toLongLong()));
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 3, 5000);
    QCOMPARE(completionEvents.last().at(0).toString(),
        QStringLiteral("task:%1:revision:%2")
            .arg(consoleTask.value(QStringLiteral("id")).toLongLong())
            .arg(consoleTask.value(QStringLiteral("revision")).toLongLong() + 1));
    QVERIFY(!consoleCompletion.isEmpty());
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ApplicationCommandTest::taskAutomationUsesFakeActionsAndCancelsStaleDelays()
{
    QTemporaryDir profile;
    QTemporaryDir applicationDirectory;
    QVERIFY(profile.isValid());
    QVERIFY(applicationDirectory.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("task-automation-test"), profile.path());
    services::ApplicationMutationWorkflow workflow(&graph);
    std::atomic_int launchCount{0};
    FakeAutomationPlatform platform(&launchCount);
    FakeAutomationScheduler scheduler;
    QStringList openedUrls;
    QStringList notifications;
    bool urlAccepted = true;
    services::TaskAutomationAdapters adapters;
    adapters.utcNow = [] { return QStringLiteral("2026-11-01T12:00:00.000Z"); };
    adapters.schedule = [&scheduler](int delayMs, std::function<void()> callback) {
        const int id = scheduler.schedule(delayMs, std::move(callback));
        return [&scheduler, id] { scheduler.cancel(id); };
    };
    adapters.openUrl = [&openedUrls, &urlAccepted](const QUrl &url) {
        openedUrls.push_back(url.toString(QUrl::FullyEncoded));
        return urlAccepted;
    };
    adapters.showNotification = [&notifications](const QString &title, const QString &message) {
        notifications.push_back(title + QStringLiteral("\n") + message);
        return true;
    };
    services::TaskAutomationService automationService(
        &graph, &workflow, &platform, nullptr, nullptr, std::move(adapters));
    QSignalSpy taskCommits(&workflow, &services::ApplicationMutationWorkflow::taskCommitted);
    QSignalSpy completionEvents(&workflow,
        &services::ApplicationMutationWorkflow::taskCompletionCommitted);
    QSignalSpy executionUpdates(&automationService,
        &services::TaskAutomationService::executionChanged);

    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running);

    const QString executablePath = applicationDirectory.filePath(QStringLiteral("FakeApp.exe"));
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    QCOMPARE(executable.write("test executable"), qint64(15));
    executable.close();
    const auto addApplicationOperation = graph.beginOperation(QStringLiteral("add fake application"));
    QVERIFY(addApplicationOperation.isValid());
    bool applicationAdded = false;
    domain::AppError applicationError;
    QVariantMap applicationRecord;
    connect(&graph, &application::ApplicationServiceGraph::storageOperationFinished, &graph,
        [&](quint64 id, const QVariantMap &values, domain::AppError error) {
            if (id != addApplicationOperation.id.value) return;
            applicationAdded = true;
            applicationError = error;
            applicationRecord = values.value(QStringLiteral("application")).toMap();
        });
    QVERIFY(graph.submitStorage(addApplicationOperation,
        services::DesktopApplicationsService::add(QStringLiteral("Fake App"), executablePath)));
    QTRY_VERIFY_WITH_TIMEOUT(applicationAdded, 5000);
    QVERIFY(!applicationError.isError());
    const qint64 applicationId = applicationRecord.value(QStringLiteral("id")).toLongLong();
    QVERIFY(applicationId > 0);

    storage::TaskInput launchInput;
    launchInput.title = QStringLiteral("Launch fake app");
    domain::TaskAutomationInput launchAutomation;
    launchAutomation.enabled = true;
    launchAutomation.actionType = QStringLiteral("launch_application");
    launchAutomation.targetId = applicationId;
    launchAutomation.delayMs = 5000;
    launchInput.automation = launchAutomation;
    QVERIFY(workflow.addTask(QStringLiteral("automation-add-launch"), launchInput).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(taskCommits.size(), 1, 5000);
    const QVariantMap launchTask = taskCommits.at(0).at(1).toMap();
    const qint64 launchTaskId = launchTask.value(QStringLiteral("id")).toLongLong();
    QVERIFY(workflow.completeTask(QStringLiteral("automation-complete-launch"), launchTaskId,
        launchTask.value(QStringLiteral("revision")).toLongLong(), true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(scheduler.count(), 1, 5000);
    QCOMPARE(scheduler.firstDelay(), 5000);
    const QString launchEventId = completionEvents.at(0).at(0).toString();
    const QVariantMap launchEventTask = completionEvents.at(0).at(1).toMap();
    workflow.taskCompletionCommitted(launchEventId, launchEventTask, false, true);
    QTRY_VERIFY_WITH_TIMEOUT(graph.operations().activeCount() == 0, 5000);
    QCOMPARE(scheduler.count(), 1); // Duplicate completion identity inserts only one execution.
    QVERIFY(scheduler.fireFirst());
    QTRY_COMPARE_WITH_TIMEOUT(launchCount.load(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto &row : executionUpdates)
            if (row.at(0).toLongLong() == launchTaskId
                && row.at(1).toString() == QStringLiteral("succeeded")) return true;
        return false;
    }(), 5000);

    storage::TaskInput delayedInput;
    delayedInput.title = QStringLiteral("Canceled notification");
    domain::TaskAutomationInput delayedAutomation;
    delayedAutomation.enabled = true;
    delayedAutomation.actionType = QStringLiteral("show_notification");
    delayedAutomation.notificationMessage = QStringLiteral("Should not display");
    delayedAutomation.delayMs = 30000;
    delayedInput.automation = delayedAutomation;
    QVERIFY(workflow.addTask(QStringLiteral("automation-add-cancel"), delayedInput).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(taskCommits.size(), 3, 5000); // create and completion each publish a task commit
    const QVariantMap delayedTask = taskCommits.last().at(1).toMap();
    const qint64 delayedTaskId = delayedTask.value(QStringLiteral("id")).toLongLong();
    QVERIFY(workflow.completeTask(QStringLiteral("automation-complete-cancel"), delayedTaskId,
        delayedTask.value(QStringLiteral("revision")).toLongLong(), true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 3, 5000); // includes the duplicate signal above
    QTRY_COMPARE_WITH_TIMEOUT(scheduler.count(), 1, 5000);
    QCOMPARE(scheduler.firstDelay(), 30000);
    QVariantMap changedTask = completionEvents.last().at(1).toMap();
    QVariantMap changedAutomation = changedTask.value(QStringLiteral("automation")).toMap();
    changedAutomation.insert(QStringLiteral("enabled"), false);
    changedAutomation.insert(QStringLiteral("revision"),
        changedAutomation.value(QStringLiteral("revision")).toLongLong() + 1);
    changedTask.insert(QStringLiteral("automation"), changedAutomation);
    automationService.observeTaskMutation(delayedTaskId, changedTask, false);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto &row : executionUpdates)
            if (row.at(0).toLongLong() == delayedTaskId
                && row.at(1).toString() == QStringLiteral("canceled")) return true;
        return false;
    }(), 5000);
    QCOMPARE(scheduler.count(), 0);
    QVERIFY(notifications.isEmpty());

    storage::TaskInput missingApplicationInput;
    missingApplicationInput.title = QStringLiteral("Missing application target");
    domain::TaskAutomationInput missingApplicationAutomation;
    missingApplicationAutomation.enabled = true;
    missingApplicationAutomation.actionType = QStringLiteral("launch_application");
    missingApplicationAutomation.targetId = 999999;
    missingApplicationInput.automation = missingApplicationAutomation;
    QVERIFY(workflow.addTask(QStringLiteral("automation-add-missing-app"),
        missingApplicationInput).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(taskCommits.size(), 5, 5000);
    const QVariantMap missingApplicationTask = taskCommits.last().at(1).toMap();
    const qint64 missingApplicationTaskId = missingApplicationTask.value(QStringLiteral("id")).toLongLong();
    QVERIFY(workflow.completeTask(QStringLiteral("automation-complete-missing-app"),
        missingApplicationTaskId, missingApplicationTask.value(QStringLiteral("revision")).toLongLong(),
        true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 4, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(scheduler.count(), 1, 5000);
    QCOMPARE(scheduler.firstDelay(), 0);
    QVERIFY(scheduler.fireFirst());
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto &row : executionUpdates)
            if (row.at(0).toLongLong() == missingApplicationTaskId
                && row.at(1).toString() == QStringLiteral("failed")
                && row.at(2).toString().contains(QStringLiteral("no longer available"))) return true;
        return false;
    }(), 5000);
    QCOMPARE(launchCount.load(), 1); // Missing IDs never fall back to another saved application.

    storage::TaskInput missingSetupInput;
    missingSetupInput.title = QStringLiteral("Missing Setup target");
    domain::TaskAutomationInput missingSetupAutomation;
    missingSetupAutomation.enabled = true;
    missingSetupAutomation.actionType = QStringLiteral("launch_setup");
    missingSetupAutomation.targetId = 999999;
    missingSetupInput.automation = missingSetupAutomation;
    QVERIFY(workflow.addTask(QStringLiteral("automation-add-missing-setup"),
        missingSetupInput).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(taskCommits.size(), 7, 5000);
    const QVariantMap missingSetupTask = taskCommits.last().at(1).toMap();
    const qint64 missingSetupTaskId = missingSetupTask.value(QStringLiteral("id")).toLongLong();
    QVERIFY(workflow.completeTask(QStringLiteral("automation-complete-missing-setup"),
        missingSetupTaskId, missingSetupTask.value(QStringLiteral("revision")).toLongLong(),
        true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 5, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(scheduler.count(), 1, 5000);
    QVERIFY(scheduler.fireFirst());
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto &row : executionUpdates)
            if (row.at(0).toLongLong() == missingSetupTaskId
                && row.at(1).toString() == QStringLiteral("failed")
                && row.at(2).toString().contains(QStringLiteral("Setup is no longer available"))) return true;
        return false;
    }(), 5000);
    QCOMPARE(launchCount.load(), 1);

    storage::TaskInput urlInput;
    urlInput.title = QStringLiteral("Open URL");
    domain::TaskAutomationInput urlAutomation;
    urlAutomation.enabled = true;
    urlAutomation.actionType = QStringLiteral("open_url");
    urlAutomation.url = QStringLiteral(" HTTPS://example.com/path ");
    urlInput.automation = urlAutomation;
    urlAccepted = false;
    QVERIFY(workflow.addTask(QStringLiteral("automation-add-url"), urlInput).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(taskCommits.size(), 9, 5000);
    const QVariantMap urlTask = taskCommits.last().at(1).toMap();
    const qint64 urlTaskId = urlTask.value(QStringLiteral("id")).toLongLong();
    QVERIFY(workflow.completeTask(QStringLiteral("automation-complete-url"), urlTaskId,
        urlTask.value(QStringLiteral("revision")).toLongLong(), true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 6, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(scheduler.count(), 1, 5000);
    QCOMPARE(scheduler.firstDelay(), 0);
    QVERIFY(scheduler.fireFirst());
    QTRY_COMPARE_WITH_TIMEOUT(openedUrls.size(), 1, 5000);
    QCOMPARE(openedUrls.first(), QStringLiteral("https://example.com/path"));
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto &row : executionUpdates)
            if (row.at(0).toLongLong() == urlTaskId
                && row.at(1).toString() == QStringLiteral("failed")
                && row.at(2).toString().contains(QStringLiteral("could not open"))) return true;
        return false;
    }(), 5000);

    storage::TaskInput notificationInput;
    notificationInput.title = QStringLiteral("Show notification");
    domain::TaskAutomationInput notificationAutomation;
    notificationAutomation.enabled = true;
    notificationAutomation.actionType = QStringLiteral("show_notification");
    notificationAutomation.notificationMessage = QStringLiteral("Done: {task.title} #{task.id}");
    notificationInput.automation = notificationAutomation;
    QVERIFY(workflow.addTask(QStringLiteral("automation-add-notification"), notificationInput).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(taskCommits.size(), 11, 5000);
    const QVariantMap notificationTask = taskCommits.last().at(1).toMap();
    const qint64 notificationTaskId = notificationTask.value(QStringLiteral("id")).toLongLong();
    QVERIFY(workflow.completeTask(QStringLiteral("automation-complete-notification"), notificationTaskId,
        notificationTask.value(QStringLiteral("revision")).toLongLong(), true).accepted);
    QTRY_COMPARE_WITH_TIMEOUT(completionEvents.size(), 7, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(scheduler.count(), 1, 5000);
    QVERIFY(scheduler.fireFirst());
    QTRY_COMPARE_WITH_TIMEOUT(notifications.size(), 1, 5000);
    QVERIFY(notifications.first().contains(QStringLiteral("Done: Show notification #%1")
        .arg(notificationTaskId)));
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const auto &row : executionUpdates)
            if (row.at(0).toLongLong() == notificationTaskId
                && row.at(1).toString() == QStringLiteral("succeeded")) return true;
        return false;
    }(), 5000);

    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ApplicationCommandTest::terminalClearDuringPendingPreservesHistoryAndLateResult()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("terminal-clear-test"), profile.path());
    services::ApplicationCommandService service(&graph);
    controllers::TerminalController terminal(&service);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() != application::ApplicationLifecycle::State::Starting, 5000);
    QVERIFY2(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running,
             qPrintable(graph.lifecycle().startupError().userMessage()));

    terminal.submit(QStringLiteral("task list"));
    QCOMPARE(terminal.pendingCount(), 1);
    terminal.clearOutput();
    QCOMPARE(terminal.output()->count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(terminal.pendingCount(), 0, 5000);
    QVERIFY(terminal.output()->count() > 0); // A later result safely appends after Clear.
    QCOMPARE(terminal.historyUp(QStringLiteral("unfinished draft")), QStringLiteral("task list"));
    QCOMPARE(terminal.historyDown(QStringLiteral("task list")), QStringLiteral("unfinished draft"));
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

QTEST_MAIN(ApplicationCommandTest)
#include "application_command_test.moc"
