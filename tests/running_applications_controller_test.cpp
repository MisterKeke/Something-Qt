#include "application/application_service_graph.h"
#include "controllers/running_applications_controller.h"
#include "platform/desktop_applications_platform.h"

#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

using namespace something;

namespace {

class FakeRunningWindowsPlatform final : public platform::RunningWindowsPlatform {
public:
    bool supported() const noexcept override { return true; }

    QVector<platform::RunningWindow> list(const application::CancellationToken &cancellation,
                                          domain::AppError *error) override
    {
        listCalls.fetch_add(1, std::memory_order_relaxed);
        if (cancellation.isCancellationRequested()) {
            if (error) *error = domain::AppError::cancelled();
            return {};
        }
        if (error) *error = domain::AppError::none();
        return {{QStringLiteral("100:200"), QStringLiteral("Editor"),
                 QStringLiteral("editor.exe"), {}, false, true}};
    }

    platform::WindowActionResult activate(const QString &,
                                          const application::CancellationToken &) override
    {
        return platform::WindowActionResult::Activated;
    }

    std::atomic_int listCalls{0};
};

} // namespace

class RunningApplicationsControllerTest final : public QObject {
    Q_OBJECT

private slots:
    void dashboardUsesOneSnapshotAndVisibleScreenPollingStopsWhenHidden();
};

void RunningApplicationsControllerTest::dashboardUsesOneSnapshotAndVisibleScreenPollingStopsWhenHidden()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    application::ApplicationServiceGraph graph(QStringLiteral("running-windows-test"),
                                                temporary.path());
    FakeRunningWindowsPlatform platform;
    controllers::RunningApplicationsController controller(&graph, &platform);
    controller.setDashboardVisible(true);
    graph.start();

    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.windows()->count(), 1, 5000);
    QCOMPARE(platform.listCalls.load(std::memory_order_relaxed), 1);
    QVERIFY(controller.hasSnapshot());

    QTest::qWait(3000);
    QCOMPARE(platform.listCalls.load(std::memory_order_relaxed), 1);

    controller.setDashboardVisible(false);
    controller.setPageVisible(true);
    QTRY_VERIFY_WITH_TIMEOUT(platform.listCalls.load(std::memory_order_relaxed) >= 2, 1000);
    controller.setPageVisible(false);
    const int callsAfterHide = platform.listCalls.load(std::memory_order_relaxed);
    QTest::qWait(3000);
    QCOMPARE(platform.listCalls.load(std::memory_order_relaxed), callsAfterHide);

    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

QTEST_GUILESS_MAIN(RunningApplicationsControllerTest)
#include "running_applications_controller_test.moc"
