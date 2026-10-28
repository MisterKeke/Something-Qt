#include "application/command_parser.h"
#include "application/command_registry.h"
#include "application/command_completion.h"
#include "application/application_service_graph.h"
#include "services/application_command_service.h"
#include "services/application_mutation_workflow.h"
#include "controllers/tasks_controller.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace something;

class TestClock final : public domain::Clock {
public:
    QDateTime nowUtc() const override { return QDateTime::currentDateTimeUtc(); }
    QTimeZone localTimeZone() const override { return QTimeZone::utc(); }
};

class ApplicationCommandTest final : public QObject {
    Q_OBJECT
private slots:
    void parserHandlesWordsQuotesAndEscapes();
    void parserRejectsUnsupportedSyntaxAndLimits();
    void registryRequiresExplicitValidUniqueDefinitions();
    void completionHandlesPartialTokensQuotesAndUnknownCommands();
    void serviceRejectsInvalidAndUnavailableCommands();
    void acceptedStorageRequestHasOneFinalResult();
    void sharedMutationWorkflowCorrelatesCommittedValuesAndConflicts();
    void guiAndConsoleUseSharedTaskWorkflowWithoutClosingAnotherDraft();
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
    QString many;
    for (int i = 0; i < 65; ++i) many += QStringLiteral("x ");
    QVERIFY(!application::CommandParser::parse(many).succeeded());
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
}

void ApplicationCommandTest::serviceRejectsInvalidAndUnavailableCommands()
{
    application::ApplicationServiceGraph graph;
    services::ApplicationCommandService service(&graph);
    QSignalSpy rejected(&service, &services::ApplicationCommandService::requestRejected);
    QSignalSpy accepted(&service, &services::ApplicationCommandService::requestAccepted);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);

    const QString badArgsId = service.submit(QStringLiteral("task list --page 0"));
    const QString startupId = service.submit(QStringLiteral("task list"));
    const QString invalidMutationId = service.submit(QStringLiteral("task complete 1 --revision 0"));
    const QString invalidSettingId = service.submit(QStringLiteral("settings week-start 8"));
    QVERIFY(!badArgsId.isEmpty());
    QVERIFY(!startupId.isEmpty());
    QVERIFY(!invalidMutationId.isEmpty());
    QVERIFY(!invalidSettingId.isEmpty());
    QCOMPARE(rejected.size(), 4);
    QCOMPARE(accepted.size(), 0);

    const QString helpId = service.submit(QStringLiteral("help"));
    QVERIFY(!helpId.isEmpty());
    QCOMPARE(accepted.size(), 1);
    QCOMPARE(finished.size(), 1);
    const auto finalResult = qvariant_cast<services::ConsoleRequestResult>(finished.constFirst().constFirst());
    QCOMPARE(finalResult.requestId, helpId);
    QCOMPARE(finalResult.state, QStringLiteral("succeeded"));
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
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(),
        application::ApplicationLifecycle::State::Running, 5000);

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
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(),
        application::ApplicationLifecycle::State::Running, 5000);
    const QString addId = service.submit(QStringLiteral("task add \"Workflow task\""));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    auto added = qvariant_cast<services::ConsoleRequestResult>(finished.at(0).at(0));
    QCOMPARE(added.requestId, addId);
    QCOMPARE(added.state, QStringLiteral("succeeded"));
    const QVariantMap committed = added.data.value(QStringLiteral("task")).toMap();
    QCOMPARE(committed.value(QStringLiteral("revision")).toLongLong(), 1);
    QCOMPARE(committed.value(QStringLiteral("priority")).toString(), QStringLiteral("medium"));
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

    service.submit(QStringLiteral("task complete %1 --revision 1")
        .arg(committed.value(QStringLiteral("id")).toLongLong()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 5000);
    const auto conflict = qvariant_cast<services::ConsoleRequestResult>(finished.at(2).at(0));
    QCOMPARE(conflict.state, QStringLiteral("failed"));
    QCOMPARE(conflict.data.value(QStringLiteral("errorCode")).toString(), QStringLiteral("conflict"));
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
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(),
        application::ApplicationLifecycle::State::Running, 5000);

    guiTasks.saveTask({{QStringLiteral("title"), QStringLiteral("GUI task")}});
    QTRY_COMPARE_WITH_TIMEOUT(editorClosed.size(), 1, 5000);
    QCOMPARE(viewEvents.size(), 1);

    const QString requestId = console.submit(QStringLiteral("task add \"Console task\""));
    QTRY_COMPARE_WITH_TIMEOUT(consoleResults.size(), 1, 5000);
    QCOMPARE(qvariant_cast<services::ConsoleRequestResult>(consoleResults.at(0).at(0)).requestId,
             requestId);
    QCOMPARE(editorClosed.size(), 1); // Console completion cannot close a separate GUI editor.
    QCOMPARE(viewEvents.size(), 2);   // Existing task mutation event reaches view consumers.
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

QTEST_MAIN(ApplicationCommandTest)
#include "application_command_test.moc"
