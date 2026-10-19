#include "application/application_service_graph.h"
#include "controllers/setups_controller.h"
#include "providers/setup_icon_image_provider.h"

#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

using namespace something;

class FakeSetupLauncher final : public platform::DesktopApplicationsPlatform {
public:
    bool supported() const noexcept override { return true; }
    domain::AppError resolveExecutable(const QString &selectedPath, QString *canonicalPath) const override
    {
        if (selectedPath.isEmpty()) return domain::AppError(domain::ErrorCategory::NotFound);
        *canonicalPath = selectedPath;
        return domain::AppError::none();
    }
    QImage extractExecutableIcon(const QString &, const application::CancellationToken &) const override
    {
        return {};
    }
    domain::AppError launchExecutable(const QString &canonicalPath,
        const application::CancellationToken &cancellation) const override
    {
        if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
        calls.push_back(canonicalPath);
        return canonicalPath.contains(QStringLiteral("fails"))
            ? domain::AppError(domain::ErrorCategory::Platform) : domain::AppError::none();
    }
    mutable QStringList calls;
};

class SetupsControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void repeatedStartClickRunsOneOrderedSequenceAndPublishesSummary();
};

void SetupsControllerTest::repeatedStartClickRunsOneOrderedSequenceAndPublishesSummary()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("setups-controller"), temporary.path());
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);

    const auto seedOperation = graph.beginOperation(QStringLiteral("seed setup controller fixture"));
    QSignalSpy seeded(&graph, &application::ApplicationServiceGraph::storageOperationFinished);
    QVERIFY(graph.submitStorage(seedOperation, [](storage::StorageRepositories &repositories,
                                                  const std::function<bool()> &cancelled) {
        storage::StorageReply reply;
        if (cancelled()) { reply.error = domain::AppError::cancelled(); return reply; }
        const auto first = repositories.applications.add(QStringLiteral("First"), QStringLiteral("first.exe"));
        const auto failing = repositories.applications.add(QStringLiteral("Fails"), QStringLiteral("fails.exe"));
        const auto third = repositories.applications.add(QStringLiteral("Third"), QStringLiteral("third.exe"));
        if (!first.succeeded() || !failing.succeeded() || !third.succeeded()) {
            reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        }
        storage::Setup setup;
        setup.name = QStringLiteral("Test setup");
        setup.applicationIds = {first.value.id, failing.value.id, third.value.id};
        const auto saved = repositories.setups.save(setup);
        if (!saved.succeeded()) { reply.error = saved.error; return reply; }
        reply.values.insert(QStringLiteral("setupId"), saved.value.id);
        return reply;
    }));
    QTRY_COMPARE_WITH_TIMEOUT(seeded.count(), 1, 5000);
    const qint64 setupId = seeded.takeFirst().at(1).toMap().value(QStringLiteral("setupId")).toLongLong();
    QVERIFY(setupId > 0);

    FakeSetupLauncher fake;
    providers::SetupIconImageProvider images(graph.profileDirectory());
    controllers::SetupsController controller(&graph, &fake, &images);
    QTRY_COMPARE_WITH_TIMEOUT(controller.setups()->count(), 1, 5000);
    QSignalSpy finished(&controller, &controllers::SetupsController::launchFinished);
    controller.startSetup(setupId);
    controller.startSetup(setupId); // The busy gate rejects the repeated activation.
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);

    const QVariantMap summary = finished.takeFirst().at(0).toMap();
    QCOMPARE(summary.value(QStringLiteral("attempted")).toInt(), 3);
    QCOMPARE(summary.value(QStringLiteral("launched")).toInt(), 2);
    QCOMPARE(summary.value(QStringLiteral("failures")).toList().size(), 1);
    QCOMPARE(fake.calls, QStringList({QStringLiteral("first.exe"), QStringLiteral("fails.exe"),
                                      QStringLiteral("third.exe")}));
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

QTEST_GUILESS_MAIN(SetupsControllerTest)
#include "setups_controller_test.moc"
