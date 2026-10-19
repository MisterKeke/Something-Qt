#pragma once

#include "application_lifecycle.h"
#include "operation_coordinator.h"
#include "capability_registry.h"
#include "../platform/profile_instance_lock.h"
#include "../providers/network_worker.h"
#include "../providers/provider_response_cache.h"
#include "../services/bounded_worker_pool.h"
#include "../storage/storage_worker.h"

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QString>
#include <QVariantMap>

namespace something::application {

class ApplicationServiceGraph final : public QObject {
    Q_OBJECT

public:
    explicit ApplicationServiceGraph(QString profileId = QStringLiteral("default"),
                                     QString dataRootOverride = {},
                                     QObject *parent = nullptr);
    ~ApplicationServiceGraph() override;

    void start();
    void shutdown();

    ApplicationLifecycle &lifecycle() noexcept { return m_lifecycle; }
    const ApplicationLifecycle &lifecycle() const noexcept { return m_lifecycle; }
    OperationCoordinator &operations() noexcept { return m_operations; }
    CapabilityRegistry &capabilities() noexcept { return m_capabilities; }

    OperationContext beginOperation(QString purpose, QString freshnessKey = {});
    // Import is a maintenance operation: stop admitting feature work, drain
    // already accepted operations, then use only maintenance operation IDs.
    bool enterProfileMaintenance();
    void leaveProfileMaintenance();
    OperationContext beginProfileMaintenanceOperation(QString purpose);
    bool cancelOperation(OperationId id);
    bool completeOperation(OperationId id, something::domain::AppError error = {});
    bool acceptsResult(OperationId id) const;
    bool submitExpensive(const OperationContext &operation, services::BoundedWorkerPool::Work work);
    bool submitExpensiveValue(const OperationContext &operation,
                              services::BoundedWorkerPool::ValueWork work);
    bool submitStorage(const OperationContext &operation, storage::StorageCommand command);
    bool requestHttps(const OperationContext &operation,
                      providers::ProviderRequest request);
    providers::ProviderResponseCache &providerResponseCache() noexcept { return m_providerCache; }

    void reportCapabilityAvailable(const QString &name);
    void reportCapabilityUnavailable(const QString &name, something::domain::AppError reason);

    QString profileDirectory() const { return m_profileDirectory; }
    QString shutdownPhase() const { return m_shutdownPhase; }
    bool isShutdownComplete() const noexcept { return m_shutdownComplete; }

signals:
    void httpsFinished(quint64 operationId,
                       QByteArray body,
                       something::domain::AppError error);
    void expensiveJobFinished(quint64 operationId, something::domain::AppError error);
    void expensiveValueJobFinished(quint64 operationId, QVariantMap values,
                                   something::domain::AppError error);
    void storageOperationFinished(quint64 operationId,
                                  QVariantMap values,
                                  something::domain::AppError error);
    void shutdownPhaseChanged(const QString &phase);
    void shutdownComplete();

private:
    static bool isValidProfileId(const QString &profileId);
    static QString resolveDataRoot(const QString &overridePath);

    void failStartup(something::domain::AppError error);
    void createWorkers();
    void onStorageOpened(something::domain::AppError error);
    void onStorageClosed(something::domain::AppError error);
    void onNetworkRequestFinished(quint64 operationId,
                                  QByteArray body,
                                  something::domain::AppError error);
    void onNetworkDrained();
    void onNetworkThreadFinished();
    void onStorageThreadFinished();
    void onStorageOperationFinished(storage::StorageReply reply);
    void maybeAdvanceShutdown();
    void setShutdownPhase(QString phase);
    QString makeConnectionName() const;

    ApplicationLifecycle m_lifecycle;
    OperationCoordinator m_operations;
    CapabilityRegistry m_capabilities;
    services::BoundedWorkerPool m_workerPool;
    providers::ProviderResponseCache m_providerCache;

    QString m_profileId;
    QString m_dataRoot;
    QString m_profileDirectory;
    platform::ProfileInstanceLock m_profileLock;

    QThread m_storageThread;
    QThread m_networkThread;
    QPointer<storage::StorageWorker> m_storageWorker;
    QPointer<providers::NetworkWorker> m_networkWorker;
    QSet<quint64> m_cpuOperationIds;
    QSet<quint64> m_networkOperationIds;
    QSet<quint64> m_storageOperationIds;

    QString m_shutdownPhase{QStringLiteral("idle")};
    bool m_profileLockHeld{false};
    bool m_storageThreadStarted{false};
    bool m_storageOpenCompleted{false};
    bool m_storageCloseRequested{false};
    bool m_storageCloseCompleted{false};
    bool m_storageThreadStopped{false};
    bool m_networkThreadStarted{false};
    bool m_networkDrained{false};
    bool m_networkQuitRequested{false};
    bool m_networkThreadStopped{false};
    bool m_shutdownStarted{false};
    bool m_shutdownComplete{false};
    bool m_profileMaintenance{false};
    QElapsedTimer m_startupTimer;
    QElapsedTimer m_shutdownTimer;
};

} // namespace something::application
