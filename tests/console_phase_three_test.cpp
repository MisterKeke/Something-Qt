#include "application/application_service_graph.h"
#include "application/route_catalog.h"
#include "services/application_command_service.h"
#include "storage/repositories.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace something;

class ConsolePhaseThreeTest final : public QObject {
    Q_OBJECT
private slots:
    void authoritativeRoutesAreRegisteredAndOpenable();
    void invalidContractsRejectBeforeDispatch();
    void unavailableSteamSnapshotIsExplicit();
    void freshTaskOutputIsBoundedAndErrorsAreSafe();
};

void ConsolePhaseThreeTest::authoritativeRoutesAreRegisteredAndOpenable()
{
    const QVariantList routes = application::RouteCatalog::routes();
    QVERIFY(!routes.isEmpty());
    QVERIFY(application::RouteCatalog::contains(QStringLiteral("terminal")));
    QVERIFY(application::RouteCatalog::contains(QStringLiteral("settings")));
    QVERIFY(!application::RouteCatalog::contains(QStringLiteral("not-a-route")));
    application::ApplicationServiceGraph graph;
    services::ApplicationCommandService service(&graph);
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);
    QSignalSpy routeRequested(&service, &services::ApplicationCommandService::routeRequested);
    service.submit(QStringLiteral("page list"));
    QCOMPARE(finished.size(), 1);
    const auto listResult = qvariant_cast<services::ConsoleRequestResult>(finished.constFirst().constFirst());
    QCOMPARE(listResult.data.value(QStringLiteral("routes")).toList(), routes);
    finished.clear();
    const QString id = service.submit(QStringLiteral("page open settings"));
    QCOMPARE(finished.size(), 1);
    const auto result = qvariant_cast<services::ConsoleRequestResult>(finished.constFirst().constFirst());
    QCOMPARE(result.requestId, id);
    QVERIFY(result.data.value(QStringLiteral("routeSelected")).toBool());
    QVERIFY(!result.data.value(QStringLiteral("pageLoadConfirmed")).toBool());
    QCOMPARE(routeRequested.size(), 1);
    QCOMPARE(routeRequested.constFirst().constFirst().toString(), QStringLiteral("settings"));
}

void ConsolePhaseThreeTest::invalidContractsRejectBeforeDispatch()
{
    application::ApplicationServiceGraph graph;
    services::ApplicationCommandService service(&graph);
    QSignalSpy rejected(&service, &services::ApplicationCommandService::requestRejected);
    const QStringList invalid{
        QStringLiteral("page open arbitrary"),
        QStringLiteral("settings set weekStart 1"),
        QStringLiteral("wallpaper delete custom:x"),
        QStringLiteral("calendar tasks 2026-02-30"),
        QStringLiteral("dashboard tasks tomorrow"),
        QStringLiteral("favorites list bookmarks"),
        QStringLiteral("news refresh"),
        QStringLiteral("apps launch 1"),
        QStringLiteral("setups launch 1"),
        QStringLiteral("steam refresh"),
    };
    for (const QString &command : invalid) service.submit(command);
    QCOMPARE(rejected.size(), invalid.size());
    for (const auto &row : rejected) {
        const auto result = qvariant_cast<services::ConsoleRequestResult>(row.constFirst());
        QVERIFY(!result.message.isEmpty());
        QCOMPARE(result.state, QStringLiteral("rejected"));
    }
}

void ConsolePhaseThreeTest::unavailableSteamSnapshotIsExplicit()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("console-steam-test"), profile.path());
    services::ApplicationCommandService service(&graph);
    service.setSnapshotProvider(QStringLiteral("steam"), [] {
        return QVariantMap{{QStringLiteral("available"), false},
                           {QStringLiteral("source"), QStringLiteral("loaded-snapshot")},
                           {QStringLiteral("message"), QStringLiteral("Steam snapshot is not loaded.")}};
    });
    QSignalSpy finished(&service, &services::ApplicationCommandService::requestFinished);
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(), application::ApplicationLifecycle::State::Running, 5000);
    const QString id = service.submit(QStringLiteral("steam list"));
    QCOMPARE(finished.size(), 1);
    const auto result = qvariant_cast<services::ConsoleRequestResult>(finished.constFirst().constFirst());
    QCOMPARE(result.requestId, id);
    QVERIFY(!result.data.value(QStringLiteral("available")).toBool());
    QCOMPARE(result.data.value(QStringLiteral("source")).toString(), QStringLiteral("loaded-snapshot"));
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void ConsolePhaseThreeTest::freshTaskOutputIsBoundedAndErrorsAreSafe()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("console-page-test"), profile.path());
    services::ApplicationCommandService service(&graph);
    QSignalSpy storageFinished(&graph, &application::ApplicationServiceGraph::storageOperationFinished);
    QSignalSpy commandFinished(&service, &services::ApplicationCommandService::requestFinished);
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(), application::ApplicationLifecycle::State::Running, 5000);

    const auto seed = graph.beginOperation(QStringLiteral("console test fixture"));
    QVERIFY(seed.isValid());
    QVERIFY(graph.submitStorage(seed, [](storage::StorageRepositories &repositories,
                                        const std::function<bool()> &cancelled) {
        storage::StorageReply reply;
        for (int i = 0; i < 55; ++i) {
            if (cancelled()) { reply.error = domain::AppError::cancelled(); return reply; }
            storage::TaskInput input;
            input.title = QStringLiteral("Console fixture %1").arg(i);
            input.priority = QStringLiteral("medium");
            const auto created = repositories.tasks.create(input);
            if (!created.succeeded()) { reply.error = created.error; return reply; }
        }
        return reply;
    }));
    QTRY_COMPARE_WITH_TIMEOUT(storageFinished.size(), 1, 5000);

    const QString listId = service.submit(QStringLiteral("task list --page 1"));
    QTRY_COMPARE_WITH_TIMEOUT(commandFinished.size(), 1, 5000);
    auto listResult = qvariant_cast<services::ConsoleRequestResult>(commandFinished.takeFirst().constFirst());
    QCOMPARE(listResult.requestId, listId);
    QCOMPARE(listResult.state, QStringLiteral("succeeded"));
    QCOMPARE(listResult.data.value(QStringLiteral("tasks")).toList().size(), 50);
    QVERIFY(listResult.data.value(QStringLiteral("hasMore")).toBool());

    const QString missingId = service.submit(QStringLiteral("task show 999999"));
    QTRY_COMPARE_WITH_TIMEOUT(commandFinished.size(), 1, 5000);
    const auto missingResult = qvariant_cast<services::ConsoleRequestResult>(commandFinished.constFirst().constFirst());
    QCOMPARE(missingResult.requestId, missingId);
    QCOMPARE(missingResult.state, QStringLiteral("failed"));
    QVERIFY(!missingResult.message.isEmpty());
    QVERIFY(!missingResult.message.contains(QStringLiteral("SELECT"), Qt::CaseInsensitive));
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

QTEST_MAIN(ConsolePhaseThreeTest)
#include "console_phase_three_test.moc"
