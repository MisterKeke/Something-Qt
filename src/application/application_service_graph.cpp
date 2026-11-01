#include "application_service_graph.h"

#include <QDir>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <utility>

namespace something::application {
namespace {
Q_LOGGING_CATEGORY(lcPerformance, "something.performance", QtWarningMsg)
}

ApplicationServiceGraph::ApplicationServiceGraph(QString profileId,
                                                 QString dataRootOverride,
                                                 QObject *parent)
    : QObject(parent)
    , m_workerPool(4)
    , m_profileId(std::move(profileId))
    , m_dataRoot(resolveDataRoot(dataRootOverride))
    , m_profileDirectory(QDir(m_dataRoot).filePath(
          QStringLiteral("profiles/") + m_profileId))
    , m_profileLock(m_profileDirectory)
{
    qRegisterMetaType<something::domain::AppError>();
    m_storageThread.setObjectName(QStringLiteral("SomethingStorage"));
    m_networkThread.setObjectName(QStringLiteral("SomethingNetwork"));

    connect(&m_operations, &OperationCoordinator::activeCountChanged,
            this, &ApplicationServiceGraph::maybeAdvanceShutdown);
    connect(&m_operations, &OperationCoordinator::cancellationRequested,
            this, [this](quint64 operationId) {
                if (!m_networkWorker || !m_networkThread.isRunning()) {
                    return;
                }
                const auto worker = m_networkWorker;
                QMetaObject::invokeMethod(worker.data(), [worker, operationId] {
                    if (worker) {
                        worker->cancel(operationId);
                    }
                }, Qt::QueuedConnection);
            });
    connect(&m_workerPool, &services::BoundedWorkerPool::jobFinished,
            this, [this](quint64 operationId, something::domain::AppError error) {
                m_cpuOperationIds.remove(operationId);
                const OperationId id{operationId};
                const bool publish = m_operations.acceptsResult(id);
                if (!publish && !error.isCancelled()) {
                    error = something::domain::AppError::cancelled();
                }
                m_operations.finish(OperationId{operationId}, error);
                if (publish) {
                    emit expensiveJobFinished(operationId, error);
                }
            });
    connect(&m_workerPool, &services::BoundedWorkerPool::valueJobFinished,
            this, [this](quint64 operationId, QVariantMap values,
                         something::domain::AppError error) {
                m_cpuOperationIds.remove(operationId);
                const OperationId id{operationId};
                const bool publish = m_operations.acceptsResult(id);
                if (!publish && !error.isCancelled()) {
                    values.clear();
                    error = something::domain::AppError::cancelled();
                }
                m_operations.finish(id, error);
                if (publish) emit expensiveValueJobFinished(operationId, values, error);
            });
    connect(&m_workerPool, &services::BoundedWorkerPool::drained,
            this, &ApplicationServiceGraph::maybeAdvanceShutdown);
}

ApplicationServiceGraph::~ApplicationServiceGraph()
{
    if (!m_shutdownComplete) {
        QEventLoop drainLoop;
        connect(this, &ApplicationServiceGraph::shutdownComplete,
                &drainLoop, &QEventLoop::quit);
        shutdown();
        if (!m_shutdownComplete) {
            drainLoop.exec(QEventLoop::ExcludeUserInputEvents);
        }
    }
    m_workerPool.beginShutdown();
}

void ApplicationServiceGraph::start()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_lifecycle.state() != ApplicationLifecycle::State::Starting || m_shutdownStarted) {
        return;
    }
    m_startupTimer.start();

    if (!isValidProfileId(m_profileId) || m_dataRoot.isEmpty()) {
        failStartup(something::domain::AppError(something::domain::ErrorCategory::Startup));
        return;
    }

    something::domain::AppError lockError;
    if (!m_profileLock.acquire(&lockError)) {
        failStartup(lockError);
        return;
    }
    m_profileLockHeld = true;

    if (!QDir().mkpath(m_profileDirectory)) {
        failStartup(something::domain::AppError(something::domain::ErrorCategory::Storage));
        return;
    }

    createWorkers();
    m_storageThreadStarted = true;
    m_networkThreadStarted = true;
    m_storageThread.start();
    m_networkThread.start();

    const auto storageWorker = m_storageWorker;
    const QString connectionName = makeConnectionName();
    const QString databasePath = QDir(m_profileDirectory).filePath(QStringLiteral("something.sqlite"));
    const bool storageQueued = QMetaObject::invokeMethod(
        storageWorker.data(), [storageWorker, connectionName, databasePath] {
        if (storageWorker) {
            storageWorker->open(connectionName, databasePath);
        }
    }, Qt::QueuedConnection);
    if (!storageQueued) {
        m_storageOpenCompleted = true;
        failStartup(something::domain::AppError(
            something::domain::ErrorCategory::Storage));
        return;
    }

    const auto networkWorker = m_networkWorker;
    const bool networkQueued = QMetaObject::invokeMethod(networkWorker.data(), [networkWorker] {
        if (networkWorker) {
            networkWorker->initialize();
        }
    }, Qt::QueuedConnection);
    if (!networkQueued) {
        m_capabilities.markUnavailable(
            QStringLiteral("external-network"),
            something::domain::AppError(
                something::domain::ErrorCategory::CapabilityUnavailable));
        m_networkDrained = true;
        m_networkQuitRequested = true;
        m_networkThread.quit();
    }
}

void ApplicationServiceGraph::createWorkers()
{
    auto *storageWorker = new storage::StorageWorker;
    auto *networkWorker = new providers::NetworkWorker;
    m_storageWorker = storageWorker;
    m_networkWorker = networkWorker;
    storageWorker->moveToThread(&m_storageThread);
    networkWorker->moveToThread(&m_networkThread);

    connect(&m_storageThread, &QThread::finished,
            storageWorker, &QObject::deleteLater);
    connect(storageWorker, &storage::StorageWorker::opened,
            this, &ApplicationServiceGraph::onStorageOpened, Qt::QueuedConnection);
    connect(storageWorker, &storage::StorageWorker::closed,
            this, &ApplicationServiceGraph::onStorageClosed, Qt::QueuedConnection);
    connect(storageWorker, &storage::StorageWorker::operationFinished,
            this, &ApplicationServiceGraph::onStorageOperationFinished, Qt::QueuedConnection);
    connect(&m_storageThread, &QThread::finished,
            this, &ApplicationServiceGraph::onStorageThreadFinished, Qt::QueuedConnection);

    connect(&m_networkThread, &QThread::finished,
            networkWorker, &QObject::deleteLater);
    connect(networkWorker, &providers::NetworkWorker::ready,
            this, [this] { m_capabilities.markAvailable(QStringLiteral("external-network")); },
            Qt::QueuedConnection);
    connect(networkWorker, &providers::NetworkWorker::initializationFailed,
            this, [this](something::domain::AppError error) {
                m_capabilities.markUnavailable(QStringLiteral("external-network"), error);
            }, Qt::QueuedConnection);
    connect(networkWorker, &providers::NetworkWorker::requestFinished,
            this, &ApplicationServiceGraph::onNetworkRequestFinished, Qt::QueuedConnection);
    connect(networkWorker, &providers::NetworkWorker::drained,
            this, &ApplicationServiceGraph::onNetworkDrained, Qt::QueuedConnection);
    connect(&m_networkThread, &QThread::finished,
            this, &ApplicationServiceGraph::onNetworkThreadFinished, Qt::QueuedConnection);
}

void ApplicationServiceGraph::failStartup(something::domain::AppError error)
{
    if (!m_lifecycle.failStartup(error)) {
        return;
    }
    shutdown();
}

OperationContext ApplicationServiceGraph::beginOperation(QString purpose, QString freshnessKey)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_lifecycle.state() != ApplicationLifecycle::State::Running || m_profileMaintenance) {
        return {};
    }
    return m_operations.begin(std::move(purpose), std::move(freshnessKey));
}

bool ApplicationServiceGraph::enterProfileMaintenance()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_profileMaintenance || m_lifecycle.state() != ApplicationLifecycle::State::Running) return false;
    m_profileMaintenance = true;
    emit profileMaintenanceChanged(true);
    return true;
}

void ApplicationServiceGraph::leaveProfileMaintenance()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_profileMaintenance) return;
    m_profileMaintenance = false;
    emit profileMaintenanceChanged(false);
}

OperationContext ApplicationServiceGraph::beginProfileMaintenanceOperation(QString purpose)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_profileMaintenance || m_lifecycle.state() != ApplicationLifecycle::State::Running
        || m_operations.activeCount() != 0) return {};
    return m_operations.begin(std::move(purpose));
}

bool ApplicationServiceGraph::cancelOperation(OperationId id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    return m_operations.cancel(id);
}

bool ApplicationServiceGraph::completeOperation(OperationId id,
                                                something::domain::AppError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    return m_operations.finish(id, error);
}

bool ApplicationServiceGraph::acceptsResult(OperationId id) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return m_operations.acceptsResult(id);
}

bool ApplicationServiceGraph::submitExpensive(const OperationContext &operation,
                                              services::BoundedWorkerPool::Work work)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!operation.isValid()) {
        return false;
    }
    if (m_cpuOperationIds.contains(operation.id.value) ||
        m_networkOperationIds.contains(operation.id.value) ||
        m_storageOperationIds.contains(operation.id.value)) {
        return false;
    }
    if (m_lifecycle.state() != ApplicationLifecycle::State::Running ||
        !m_operations.acceptsResult(operation.id)) {
        m_operations.finish(operation.id, something::domain::AppError::cancelled());
        return false;
    }
    if (!work) {
        const auto error = something::domain::AppError(
            something::domain::ErrorCategory::Validation);
        m_operations.finish(operation.id, error);
        emit expensiveJobFinished(operation.id.value, error);
        return false;
    }
    m_cpuOperationIds.insert(operation.id.value);
    if (!m_workerPool.submit(operation, std::move(work))) {
        m_cpuOperationIds.remove(operation.id.value);
        const auto error = something::domain::AppError(
            something::domain::ErrorCategory::Busy);
        m_operations.finish(operation.id, error);
        emit expensiveJobFinished(operation.id.value, error);
        return false;
    }
    return true;
}

bool ApplicationServiceGraph::submitExpensiveValue(const OperationContext &operation,
                                                   services::BoundedWorkerPool::ValueWork work)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!operation.isValid()) return false;
    if (m_cpuOperationIds.contains(operation.id.value) ||
        m_networkOperationIds.contains(operation.id.value) ||
        m_storageOperationIds.contains(operation.id.value)) return false;
    if (m_lifecycle.state() != ApplicationLifecycle::State::Running ||
        !m_operations.acceptsResult(operation.id)) {
        m_operations.finish(operation.id, something::domain::AppError::cancelled());
        return false;
    }
    if (!work) {
        const auto error = something::domain::AppError(something::domain::ErrorCategory::Validation);
        m_operations.finish(operation.id, error);
        emit expensiveValueJobFinished(operation.id.value, {}, error);
        return false;
    }
    m_cpuOperationIds.insert(operation.id.value);
    if (!m_workerPool.submitValue(operation, std::move(work))) {
        m_cpuOperationIds.remove(operation.id.value);
        const auto error = something::domain::AppError(something::domain::ErrorCategory::Busy);
        m_operations.finish(operation.id, error);
        emit expensiveValueJobFinished(operation.id.value, {}, error);
        return false;
    }
    return true;
}

bool ApplicationServiceGraph::submitStorage(const OperationContext &operation,
                                            storage::StorageCommand command)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!operation.isValid()) {
        return false;
    }
    if (m_cpuOperationIds.contains(operation.id.value)
        || m_networkOperationIds.contains(operation.id.value)
        || m_storageOperationIds.contains(operation.id.value)) {
        return false;
    }
    if (m_lifecycle.state() != ApplicationLifecycle::State::Running
        || !m_operations.acceptsResult(operation.id)) {
        m_operations.finish(operation.id, something::domain::AppError::cancelled());
        return false;
    }
    if (!command) {
        const auto error = something::domain::AppError(something::domain::ErrorCategory::Validation);
        m_operations.finish(operation.id, error);
        emit storageOperationFinished(operation.id.value, {}, error);
        return false;
    }
    const auto worker = m_storageWorker;
    if (!worker || !m_storageThread.isRunning()) {
        const auto error = something::domain::AppError(
            something::domain::ErrorCategory::CapabilityUnavailable);
        m_operations.finish(operation.id, error);
        emit storageOperationFinished(operation.id.value, {}, error);
        return false;
    }

    m_storageOperationIds.insert(operation.id.value);
    const auto cancellation = operation.cancellation;
    const auto submitResult = worker->submit(operation.id.value, std::move(command),
        [cancellation] { return cancellation.isCancellationRequested(); });
    if (submitResult != storage::StorageWorker::SubmitResult::Accepted) {
        m_storageOperationIds.remove(operation.id.value);
        const auto error = something::domain::AppError(
            !m_operations.acceptsResult(operation.id)
                ? something::domain::ErrorCategory::Cancelled
                : (submitResult == storage::StorageWorker::SubmitResult::QueueFull
                       ? something::domain::ErrorCategory::Busy
                       : something::domain::ErrorCategory::Storage));
        const bool publish = m_operations.acceptsResult(operation.id);
        m_operations.finish(operation.id, error);
        if (publish) {
            emit storageOperationFinished(operation.id.value, {}, error);
        }
        maybeAdvanceShutdown();
        return false;
    }
    return true;
}

bool ApplicationServiceGraph::requestHttps(const OperationContext &operation,
                                           providers::ProviderRequest request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!operation.isValid()) {
        return false;
    }
    if (m_cpuOperationIds.contains(operation.id.value) ||
        m_networkOperationIds.contains(operation.id.value) ||
        m_storageOperationIds.contains(operation.id.value)) {
        return false;
    }
    if (m_lifecycle.state() != ApplicationLifecycle::State::Running ||
        !m_operations.acceptsResult(operation.id)) {
        m_operations.finish(operation.id, something::domain::AppError::cancelled());
        return false;
    }
    if (!m_networkWorker || !m_networkThread.isRunning() ||
        !m_capabilities.isAvailable(QStringLiteral("external-network"))) {
        const auto error = something::domain::AppError(
            something::domain::ErrorCategory::CapabilityUnavailable);
        m_operations.finish(operation.id, error);
        emit httpsFinished(operation.id.value, {}, error);
        return false;
    }
    const auto worker = m_networkWorker;
    m_networkOperationIds.insert(operation.id.value);
    const bool requestQueued = QMetaObject::invokeMethod(worker.data(),
                              [worker, operation, request = std::move(request)]() mutable {
                                  if (worker) {
                                      worker->get(operation, std::move(request));
                                  }
                              },
                              Qt::QueuedConnection);
    if (!requestQueued) {
        m_networkOperationIds.remove(operation.id.value);
        const auto error = something::domain::AppError(
            something::domain::ErrorCategory::CapabilityUnavailable);
        m_operations.finish(operation.id, error);
        emit httpsFinished(operation.id.value, {}, error);
        return false;
    }
    return true;
}

void ApplicationServiceGraph::reportCapabilityAvailable(const QString &name)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_capabilities.markAvailable(name);
}

void ApplicationServiceGraph::reportCapabilityUnavailable(const QString &name,
                                                           something::domain::AppError reason)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_capabilities.markUnavailable(name, reason);
}

void ApplicationServiceGraph::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shutdownStarted) {
        return;
    }
    m_shutdownStarted = true;
    m_shutdownTimer.start();

    setShutdownPhase(QStringLiteral("reject-new-operations"));
    m_lifecycle.beginShutdown();
    m_operations.beginShutdown();

    setShutdownPhase(QStringLiteral("cancel-requests-and-jobs"));
    m_workerPool.beginShutdown();

    setShutdownPhase(QStringLiteral("drain-internal-work"));
    if (!m_networkThreadStarted) {
        m_networkDrained = true;
        m_networkThreadStopped = true;
    } else if (m_networkWorker) {
        const auto worker = m_networkWorker;
        const bool shutdownQueued = QMetaObject::invokeMethod(worker.data(), [worker] {
            if (worker) {
                worker->shutdown();
            }
        }, Qt::QueuedConnection);
        if (!shutdownQueued) {
            m_networkDrained = true;
            m_networkQuitRequested = true;
            m_networkThread.quit();
        }
    }

    if (!m_storageThreadStarted) {
        m_storageOpenCompleted = true;
        m_storageCloseCompleted = true;
        m_storageThreadStopped = true;
    }

    maybeAdvanceShutdown();
}

void ApplicationServiceGraph::stopAcceptingOperations()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_operations.stopAccepting();
}

void ApplicationServiceGraph::onStorageOpened(something::domain::AppError error)
{
    m_storageOpenCompleted = true;
    if (error.isError()) {
        if (m_lifecycle.state() == ApplicationLifecycle::State::Starting) {
            failStartup(error);
        }
        maybeAdvanceShutdown();
        return;
    }

    m_capabilities.markAvailable(QStringLiteral("local-storage"));
    if (m_lifecycle.state() == ApplicationLifecycle::State::Starting) {
        if (m_lifecycle.markRunning()) {
            qCInfo(lcPerformance) << "startup_ready elapsed_ms=" << m_startupTimer.elapsed();
        }
    }
    maybeAdvanceShutdown();
}

void ApplicationServiceGraph::onStorageClosed(something::domain::AppError error)
{
    Q_UNUSED(error);
    m_storageCloseCompleted = true;
    if (m_storageThread.isRunning()) {
        m_storageThread.quit();
    }
    maybeAdvanceShutdown();
}

void ApplicationServiceGraph::onNetworkRequestFinished(
    quint64 operationId,
    QByteArray body,
    something::domain::AppError error)
{
    m_networkOperationIds.remove(operationId);
    const OperationId id{operationId};
    const bool publish = m_operations.acceptsResult(id);
    m_operations.finish(id, error);
    if (publish) {
        emit httpsFinished(operationId, body, error);
    }
}

void ApplicationServiceGraph::onNetworkDrained()
{
    m_networkDrained = true;
    maybeAdvanceShutdown();
}

void ApplicationServiceGraph::onNetworkThreadFinished()
{
    m_networkThreadStopped = true;
    m_networkDrained = true;
    m_networkWorker = nullptr;
    if (!m_shutdownStarted) {
        m_capabilities.markUnavailable(
            QStringLiteral("external-network"),
            something::domain::AppError(something::domain::ErrorCategory::CapabilityUnavailable));
    }
    const auto operationIds = m_networkOperationIds;
    m_networkOperationIds.clear();
    for (quint64 operationId : operationIds) {
        const OperationId id{operationId};
        const auto error = something::domain::AppError(
            something::domain::ErrorCategory::CapabilityUnavailable);
        const bool publish = m_operations.acceptsResult(id);
        m_operations.finish(id, error);
        if (publish) {
            emit httpsFinished(operationId, {}, error);
        }
    }
    maybeAdvanceShutdown();
}

void ApplicationServiceGraph::onStorageThreadFinished()
{
    m_storageThreadStopped = true;
    m_storageWorker = nullptr;
    const auto operationIds = m_storageOperationIds;
    m_storageOperationIds.clear();
    for (quint64 operationId : operationIds) {
        const OperationId id{operationId};
        const auto error = something::domain::AppError(
            something::domain::ErrorCategory::CapabilityUnavailable);
        const bool publish = m_operations.acceptsResult(id);
        m_operations.finish(id, error);
        if (publish) {
            emit storageOperationFinished(operationId, {}, error);
        }
    }
    maybeAdvanceShutdown();
}

void ApplicationServiceGraph::onStorageOperationFinished(storage::StorageReply reply)
{
    m_storageOperationIds.remove(reply.operationId);
    const OperationId id{reply.operationId};
    const bool publish = m_operations.acceptsResult(id);
    if (!publish && !reply.error.isCancelled()) {
        reply.error = something::domain::AppError::cancelled();
    }
    m_operations.finish(id, reply.error);
    if (publish) {
        emit storageOperationFinished(reply.operationId, std::move(reply.values), reply.error);
    }
    maybeAdvanceShutdown();
}

void ApplicationServiceGraph::maybeAdvanceShutdown()
{
    if (!m_shutdownStarted || m_shutdownComplete) {
        return;
    }

    if (m_operations.activeCount() != 0 || !m_workerPool.isDrained() || !m_networkDrained
        || !m_storageOperationIds.isEmpty()) {
        return;
    }

    if (m_networkThreadStarted && !m_networkQuitRequested) {
        setShutdownPhase(QStringLiteral("stop-network-thread"));
        m_networkQuitRequested = true;
        m_networkThread.quit();
    }
    if (!m_networkThreadStopped) {
        return;
    }

    if (m_storageThreadStarted) {
        if (!m_storageOpenCompleted) {
            return;
        }
        if (!m_storageCloseRequested) {
            setShutdownPhase(QStringLiteral("close-storage"));
            m_storageCloseRequested = true;
            const auto worker = m_storageWorker;
            if (!worker) {
                m_storageCloseCompleted = true;
                m_storageThread.quit();
            } else {
                const bool closeQueued = QMetaObject::invokeMethod(worker.data(), [worker] {
                    if (worker) {
                        worker->close();
                    }
                }, Qt::QueuedConnection);
                if (!closeQueued) {
                    m_storageCloseCompleted = true;
                    m_storageThread.quit();
                }
            }
            return;
        }
        if (!m_storageCloseCompleted) {
            return;
        }
        if (m_storageThread.isRunning()) {
            m_storageThread.quit();
        }
        if (!m_storageThreadStopped) {
            return;
        }
    }

    setShutdownPhase(QStringLiteral("release-profile-lock"));
    if (m_profileLockHeld) {
        m_profileLock.release();
        m_profileLockHeld = false;
    }

    m_lifecycle.markStopped();
    m_shutdownComplete = true;
    setShutdownPhase(QStringLiteral("complete"));
    qCInfo(lcPerformance) << "shutdown_complete elapsed_ms=" << m_shutdownTimer.elapsed();
    emit shutdownComplete();
}

void ApplicationServiceGraph::setShutdownPhase(QString phase)
{
    if (m_shutdownPhase == phase) {
        return;
    }
    m_shutdownPhase = std::move(phase);
    emit shutdownPhaseChanged(m_shutdownPhase);
}

QString ApplicationServiceGraph::makeConnectionName() const
{
    const QString nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QStringLiteral("something-%1-%2").arg(m_profileId, nonce);
}

bool ApplicationServiceGraph::isValidProfileId(const QString &profileId)
{
    static const QRegularExpression profilePattern(QStringLiteral("^[A-Za-z0-9_-]{1,48}$"));
    return profilePattern.match(profileId).hasMatch();
}

QString ApplicationServiceGraph::resolveDataRoot(const QString &overridePath)
{
    if (!overridePath.trimmed().isEmpty()) {
        return QDir::cleanPath(overridePath);
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

} // namespace something::application
