#include "application/command_parser.h"
#include "application/command_registry.h"
#include "application/application_service_graph.h"
#include "services/application_command_service.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace something;

class ApplicationCommandTest final : public QObject {
    Q_OBJECT
private slots:
    void parserHandlesWordsQuotesAndEscapes();
    void parserRejectsUnsupportedSyntaxAndLimits();
    void registryRequiresExplicitValidUniqueDefinitions();
    void serviceRejectsInvalidAndUnavailableCommands();
    void acceptedStorageRequestHasOneFinalResult();
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

void ApplicationCommandTest::serviceRejectsInvalidAndUnavailableCommands()
{
    application::ApplicationServiceGraph graph;
    services::ApplicationCommandService service(&graph);
    QSignalSpy rejected(&service, &services::ApplicationCommandService::requestRejected);
    QSignalSpy accepted(&service, &services::ApplicationCommandService::requestAccepted);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);

    const QString badArgsId = service.submit(QStringLiteral("task list --page 0"));
    const QString startupId = service.submit(QStringLiteral("task list"));
    QVERIFY(!badArgsId.isEmpty());
    QVERIFY(!startupId.isEmpty());
    QCOMPARE(rejected.size(), 2);
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

QTEST_MAIN(ApplicationCommandTest)
#include "application_command_test.moc"
