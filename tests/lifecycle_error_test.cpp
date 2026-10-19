#include "application/application_lifecycle.h"
#include "application/application_service_graph.h"
#include "application/capability_registry.h"
#include "application/operation_coordinator.h"
#include "domain/app_error.h"
#include "platform/profile_instance_lock.h"
#include "services/bounded_worker_pool.h"

#include <QSignalSpy>
#include <QSemaphore>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <utility>

using namespace something;

class LifecycleErrorTest final : public QObject {
    Q_OBJECT

private slots:
    void safeMessagesAreStableAndCancellationIsQuiet();
    void newerOperationCancelsAndRejectsOlderResult();
    void shutdownRejectsNewWorkAndWaitsForCompletion();
    void optionalCapabilityFailureDoesNotFailApplication();
    void profileLockIsExclusiveOnlyWithinProfile();
    void duplicateProfileLockFailsStartupSafely();
    void rejectedCpuDispatchCompletesItsOperation();
    void asyncOperationHasOneCompletionOwner();
    void storageCommandsReturnDetachedValuesAndJoinShutdownDrain();
    void graphDrainsOperationsBeforeClosingStorage();
    void profileMaintenanceDrainsAndRejectsFeatureAdmission();
    void boundedWorkerPoolRejectsExcessQueuedJobs();
};

void LifecycleErrorTest::safeMessagesAreStableAndCancellationIsQuiet()
{
    QCOMPARE(domain::AppError(domain::ErrorCategory::Network).userMessage(),
             QStringLiteral("The network request could not be completed."));
    QCOMPARE(domain::AppError(domain::ErrorCategory::Storage).userMessage(),
             QStringLiteral("Local data could not be opened or saved."));
    QCOMPARE(domain::AppError(domain::ErrorCategory::Busy).userMessage(),
             QStringLiteral("Background work is busy. Try again shortly."));
    QCOMPARE(domain::AppError(domain::ErrorCategory::Timeout).userMessage(),
             QStringLiteral("The service did not respond in time."));
    QCOMPARE(domain::AppError(domain::ErrorCategory::Connection).userMessage(),
             QStringLiteral("Could not connect to the service."));
    QCOMPARE(domain::AppError(domain::ErrorCategory::Http).userMessage(),
             QStringLiteral("The service returned an error response."));
    QVERIFY(domain::AppError::cancelled().userMessage().isEmpty());
    QCOMPARE(domain::AppError(domain::ErrorCategory::InstanceAlreadyRunning).userMessage(),
             QStringLiteral("Something is already running for this profile."));
}

void LifecycleErrorTest::newerOperationCancelsAndRejectsOlderResult()
{
    application::OperationCoordinator operations;
    QSignalSpy cancellationSpy(&operations, &application::OperationCoordinator::cancellationRequested);

    const auto older = operations.begin(QStringLiteral("city search"), QStringLiteral("weather-search"));
    const auto newer = operations.begin(QStringLiteral("city search"), QStringLiteral("weather-search"));

    QVERIFY(older.isValid());
    QVERIFY(newer.isValid());
    QVERIFY(older.cancellation.isCancellationRequested());
    QVERIFY(!operations.acceptsResult(older.id));
    QVERIFY(operations.acceptsResult(newer.id));
    QCOMPARE(cancellationSpy.count(), 1);

    QVERIFY(operations.finish(older.id));
    QVERIFY(operations.acceptsResult(newer.id));
    QVERIFY(operations.finish(newer.id));
    QCOMPARE(operations.activeCount(), 0);
}

void LifecycleErrorTest::shutdownRejectsNewWorkAndWaitsForCompletion()
{
    application::OperationCoordinator operations;
    QSignalSpy drainedSpy(&operations, &application::OperationCoordinator::drained);
    const auto inFlight = operations.begin(QStringLiteral("refresh"));

    operations.beginShutdown();

    QVERIFY(!operations.acceptingOperations());
    QVERIFY(inFlight.cancellation.isCancellationRequested());
    QVERIFY(!operations.begin(QStringLiteral("late request")).isValid());
    QCOMPARE(operations.activeCount(), 1);
    QCOMPARE(drainedSpy.count(), 0);

    QVERIFY(operations.finish(inFlight.id, domain::AppError::cancelled()));
    QCOMPARE(operations.activeCount(), 0);
    QCOMPARE(drainedSpy.count(), 1);
}

void LifecycleErrorTest::optionalCapabilityFailureDoesNotFailApplication()
{
    application::ApplicationLifecycle lifecycle;
    application::CapabilityRegistry capabilities;

    QVERIFY(lifecycle.markRunning());
    capabilities.markUnavailable(
        QStringLiteral("location"),
        domain::AppError(domain::ErrorCategory::Platform));

    QCOMPARE(lifecycle.state(), application::ApplicationLifecycle::State::Running);
    QVERIFY(!capabilities.isAvailable(QStringLiteral("location")));
    QCOMPARE(capabilities.unavailableCapabilities(),
             QStringList{QStringLiteral("location: This action is unavailable on this device.")});
}

void LifecycleErrorTest::profileLockIsExclusiveOnlyWithinProfile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    platform::ProfileInstanceLock first(directory.filePath(QStringLiteral("profiles/alpha")));
    platform::ProfileInstanceLock duplicate(directory.filePath(QStringLiteral("profiles/alpha")));
    platform::ProfileInstanceLock otherProfile(directory.filePath(QStringLiteral("profiles/beta")));

    domain::AppError error;
    QVERIFY(first.acquire(&error));
    QCOMPARE(error.category(), domain::ErrorCategory::None);
    QVERIFY(!duplicate.acquire(&error));
    QCOMPARE(error.category(), domain::ErrorCategory::InstanceAlreadyRunning);
    QVERIFY(otherProfile.acquire(&error));

    first.release();
    QVERIFY(duplicate.acquire(&error));
    duplicate.release();
    otherProfile.release();
}

void LifecycleErrorTest::duplicateProfileLockFailsStartupSafely()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    platform::ProfileInstanceLock held(
        directory.filePath(QStringLiteral("profiles/duplicate-startup")));
    domain::AppError lockError;
    QVERIFY(held.acquire(&lockError));

    application::ApplicationServiceGraph graph(QStringLiteral("duplicate-startup"), directory.path());
    graph.start();

    QCOMPARE(graph.lifecycle().state(), application::ApplicationLifecycle::State::Stopped);
    QCOMPARE(graph.lifecycle().startupError().category(),
             domain::ErrorCategory::InstanceAlreadyRunning);
    QCOMPARE(graph.lifecycle().startupError().userMessage(),
             QStringLiteral("Something is already running for this profile."));
    QVERIFY(graph.isShutdownComplete());
    held.release();
}

void LifecycleErrorTest::rejectedCpuDispatchCompletesItsOperation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    application::ApplicationServiceGraph graph(QStringLiteral("dispatch-test"), directory.path());
    QSignalSpy jobSpy(&graph, &application::ApplicationServiceGraph::expensiveJobFinished);
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(),
                              application::ApplicationLifecycle::State::Running,
                              5000);

    const auto operation = graph.beginOperation(QStringLiteral("invalid dispatch"));
    QVERIFY(operation.isValid());
    QVERIFY(!graph.submitExpensive(operation, {}));
    QCOMPARE(graph.operations().activeCount(), 0);
    QCOMPARE(jobSpy.count(), 1);
    QCOMPARE(qvariant_cast<domain::AppError>(jobSpy.takeFirst().at(1)).category(),
             domain::ErrorCategory::Validation);

    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void LifecycleErrorTest::asyncOperationHasOneCompletionOwner()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    application::ApplicationServiceGraph graph(QStringLiteral("single-owner-test"), directory.path());
    QSignalSpy jobSpy(&graph, &application::ApplicationServiceGraph::expensiveJobFinished);
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(),
                              application::ApplicationLifecycle::State::Running,
                              5000);

    const auto operation = graph.beginOperation(QStringLiteral("single completion owner"));
    QVERIFY(operation.isValid());
    const services::BoundedWorkerPool::Work work = [](const application::CancellationToken &) {
        return domain::AppError::none();
    };
    QVERIFY(graph.submitExpensive(operation, work));
    QVERIFY(!graph.submitExpensive(operation, work));
    QTRY_COMPARE_WITH_TIMEOUT(jobSpy.count(), 1, 5000);
    QCOMPARE(graph.operations().activeCount(), 0);

    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void LifecycleErrorTest::storageCommandsReturnDetachedValuesAndJoinShutdownDrain()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    application::ApplicationServiceGraph graph(QStringLiteral("storage-dispatch-test"), directory.path());
    QSignalSpy resultSpy(&graph, &application::ApplicationServiceGraph::storageOperationFinished);
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(),
                              application::ApplicationLifecycle::State::Running,
                              5000);

    const auto operation = graph.beginOperation(QStringLiteral("create task through storage worker"));
    QVERIFY(operation.isValid());
    storage::StorageCommand command = [](storage::StorageRepositories &repositories,
                                         const std::function<bool()> &isCancelled) {
        storage::StorageReply reply;
        if (isCancelled()) {
            reply.error = domain::AppError::cancelled();
            return reply;
        }
        storage::TaskInput input;
        input.title = QStringLiteral("Worker-owned task");
        const auto created = repositories.tasks.create(input);
        reply.error = created.error;
        if (created.succeeded()) {
            reply.values.insert(QStringLiteral("taskId"), created.value.id);
            reply.values.insert(QStringLiteral("title"), created.value.title);
        }
        return reply;
    };
    QVERIFY(graph.submitStorage(operation, std::move(command)));
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 1, 5000);
    const auto result = resultSpy.takeFirst();
    QCOMPARE(result.at(0).toULongLong(), operation.id.value);
    QCOMPARE(result.at(1).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Worker-owned task"));
    QCOMPARE(qvariant_cast<domain::AppError>(result.at(2)).category(), domain::ErrorCategory::None);
    QCOMPARE(graph.operations().activeCount(), 0);

    const auto invalidResultOperation = graph.beginOperation(QStringLiteral("reject live SQL result type"));
    storage::StorageCommand invalidResult = [](storage::StorageRepositories &,
                                               const std::function<bool()> &) {
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("query"), QVariant::fromValue(QSqlQuery{}));
        return reply;
    };
    QVERIFY(graph.submitStorage(invalidResultOperation, std::move(invalidResult)));
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 1, 5000);
    const auto rejectedResult = resultSpy.takeFirst();
    QCOMPARE(qvariant_cast<domain::AppError>(rejectedResult.at(2)).category(), domain::ErrorCategory::Internal);
    QVERIFY(rejectedResult.at(1).toMap().isEmpty());

    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void LifecycleErrorTest::graphDrainsOperationsBeforeClosingStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    application::ApplicationServiceGraph graph(QStringLiteral("lifecycle-test"), directory.path());
    QSignalSpy phaseSpy(&graph, &application::ApplicationServiceGraph::shutdownPhaseChanged);
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(),
                              application::ApplicationLifecycle::State::Running,
                              5000);

    const auto operation = graph.beginOperation(QStringLiteral("controlled operation"));
    QVERIFY(operation.isValid());

    graph.shutdown();
    QVERIFY(operation.cancellation.isCancellationRequested());
    QCOMPARE(graph.operations().activeCount(), 1);
    for (const auto &phase : phaseSpy) {
        QVERIFY(phase.at(0).toString() != QStringLiteral("close-storage"));
    }

    QVERIFY(graph.completeOperation(operation.id, domain::AppError::cancelled()));
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);

    QStringList phases;
    for (const auto &phase : phaseSpy) {
        phases.push_back(phase.at(0).toString());
    }
    const int drainIndex = phases.indexOf(QStringLiteral("drain-internal-work"));
    const int closeIndex = phases.indexOf(QStringLiteral("close-storage"));
    const int releaseIndex = phases.indexOf(QStringLiteral("release-profile-lock"));
    const int completeIndex = phases.indexOf(QStringLiteral("complete"));
    QVERIFY(drainIndex >= 0);
    QVERIFY(closeIndex > drainIndex);
    QVERIFY(releaseIndex > closeIndex);
    QVERIFY(completeIndex > releaseIndex);
    QCOMPARE(graph.lifecycle().state(), application::ApplicationLifecycle::State::Stopped);
}

void LifecycleErrorTest::profileMaintenanceDrainsAndRejectsFeatureAdmission()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    application::ApplicationServiceGraph graph(QStringLiteral("maintenance-test"), directory.path());
    graph.start();
    QTRY_COMPARE_WITH_TIMEOUT(graph.lifecycle().state(), application::ApplicationLifecycle::State::Running, 5000);
    const auto existing = graph.beginOperation(QStringLiteral("existing feature work"));
    QVERIFY(existing.isValid());
    QVERIFY(graph.enterProfileMaintenance());
    QVERIFY(!graph.beginOperation(QStringLiteral("new feature work")).isValid());
    QVERIFY(!graph.beginProfileMaintenanceOperation(QStringLiteral("too early")).isValid());
    QVERIFY(graph.completeOperation(existing.id));
    const auto importOperation = graph.beginProfileMaintenanceOperation(QStringLiteral("import"));
    QVERIFY(importOperation.isValid());
    QVERIFY(!graph.enterProfileMaintenance());
    QVERIFY(graph.completeOperation(importOperation.id));
    graph.leaveProfileMaintenance();
    const auto next = graph.beginOperation(QStringLiteral("after cancelled import"));
    QVERIFY(next.isValid());
    QVERIFY(graph.completeOperation(next.id));
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void LifecycleErrorTest::boundedWorkerPoolRejectsExcessQueuedJobs()
{
    services::BoundedWorkerPool pool(1);
    application::OperationCoordinator operations;
    QSemaphore entered;
    QSemaphore release;
    QSignalSpy finished(&pool, &services::BoundedWorkerPool::jobFinished);
    QVERIFY(finished.isValid());
    connect(&pool, &services::BoundedWorkerPool::jobFinished, &operations,
            [&operations](quint64 id, domain::AppError error) {
        operations.finish(application::OperationId{id}, error);
    });

    const auto first = operations.begin(QStringLiteral("bounded queue fixture"));
    QVERIFY(pool.submit(first, [&entered, &release](const application::CancellationToken &) {
        entered.release();
        release.acquire();
        return domain::AppError::none();
    }));
    if (!entered.tryAcquire(1, 5000)) {
        // Always unblock the worker before returning from a failed assertion;
        // the pool destructor drains admitted work.
        release.release();
        QFAIL("The first worker did not start within the fixture timeout");
        return;
    }

    int acceptedCount = 1;
    for (int index = 1; index < services::BoundedWorkerPool::maximumOutstandingJobs(); ++index) {
        const auto operation = operations.begin(QStringLiteral("bounded queue fixture"));
        if (!pool.submit(operation, [](const application::CancellationToken &) {
            return domain::AppError::none();
        })) {
            release.release();
            QFAIL("The pool rejected a job before reaching its configured limit");
            return;
        }
        ++acceptedCount;
    }
    const auto rejected = operations.begin(QStringLiteral("bounded queue fixture"));
    const bool excessAccepted = pool.submit(rejected, [](const application::CancellationToken &) {
        return domain::AppError::none();
    });
    if (!excessAccepted)
        operations.finish(rejected.id, domain::AppError(domain::ErrorCategory::Busy));

    release.release();
    QCOMPARE(acceptedCount, services::BoundedWorkerPool::maximumOutstandingJobs());
    QVERIFY(!excessAccepted);
    QCOMPARE(pool.outstandingJobs(), acceptedCount);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), acceptedCount,
                              10000);
    QCOMPARE(pool.outstandingJobs(), 0);
    QCOMPARE(operations.activeCount(), 0);
}

QTEST_GUILESS_MAIN(LifecycleErrorTest)
#include "lifecycle_error_test.moc"
