#pragma once

#include "../application/application_service_graph.h"

#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <QHash>

namespace something::controllers {

class LegacyDataImportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canImport READ canImport NOTIFY stateChanged)
    Q_PROPERTY(bool imported READ imported NOTIFY stateChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)
    Q_PROPERTY(QString progressMessage READ progressMessage NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString eligibilityMessage READ eligibilityMessage NOTIFY stateChanged)
    Q_PROPERTY(QString reportJson READ reportJson NOTIFY stateChanged)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY stateChanged)
    Q_PROPERTY(bool importActive READ importActive NOTIFY stateChanged)

public:
    explicit LegacyDataImportController(application::ApplicationServiceGraph *graph,
                                         QObject *parent = nullptr);

    bool ready() const noexcept { return m_ready; }
    bool busy() const noexcept { return m_busy; }
    bool canImport() const noexcept { return m_canImport; }
    bool imported() const noexcept { return m_imported; }
    bool canCancel() const noexcept { return m_canCancel; }
    QString progressMessage() const { return m_progressMessage; }
    QString errorMessage() const { return m_errorMessage; }
    QString eligibilityMessage() const { return m_eligibilityMessage; }
    QString reportJson() const { return m_reportJson; }
    bool restartRequired() const noexcept { return m_restartRequired; }
    bool importActive() const noexcept { return m_ownsMaintenance; }

    Q_INVOKABLE void refreshStatus();
    Q_INVOKABLE void importProfile(const QUrl &databaseFile, bool sourceClosedConfirmed);
    Q_INVOKABLE void cancelImport();

signals:
    void stateChanged();

private:
    enum class StorageAction { Status, Preflight, RecoveryCheck, Import };
    enum class WorkAction { InspectRecovery, Prepare, Rollback, Finish };

    bool submitStorage(StorageAction action, application::OperationContext operation,
                       storage::StorageCommand command);
    bool submitWork(WorkAction action, application::OperationContext operation,
                    services::BoundedWorkerPool::ValueWork work);
    void beginRecoveryInspection();
    void beginImport(const QString &databasePath);
    void startPreparation();
    void prepareSource();
    void leaveMaintenance();
    void beginDatabaseImport();
    void beginRollback(QString safeMessage);
    void setBusy(bool busy, QString progress = {});
    void setError(QString message);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onWorkFinished(quint64 operationId, const QVariantMap &values,
                        domain::AppError error);

    application::ApplicationServiceGraph *m_graph{nullptr};
    QHash<quint64, StorageAction> m_storageActions;
    QHash<quint64, WorkAction> m_workActions;
    quint64 m_activeOperationId{0};
    QString m_importId;
    QString m_planPath;
    QString m_sourceDatabasePath;
    QString m_recoveryId;
    bool m_ready{false};
    bool m_busy{false};
    bool m_canImport{false};
    bool m_imported{false};
    bool m_canCancel{false};
    bool m_importInProgress{false};
    bool m_restartRequired{false};
    bool m_waitingForMaintenance{false};
    bool m_ownsMaintenance{false};
    bool m_profileCheckFailed{false};
    application::CancellationSource m_importCancellation;
    QString m_progressMessage;
    QString m_errorMessage;
    QString m_eligibilityMessage;
    QString m_reportJson;
};

} // namespace something::controllers
