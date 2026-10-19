#include "legacy_data_import_controller.h"

#include "../application/application_lifecycle.h"
#include "../services/legacy_data_import_service.h"
#include "../storage/repositories.h"

#include <QThread>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace something::controllers {
namespace {
QString failureReport(const QString &json, const QString &message)
{
    QJsonObject report = QJsonDocument::fromJson(json.toUtf8()).object();
    report.insert(QStringLiteral("committed"), false);
    QJsonArray failures = report.value(QStringLiteral("failures")).toArray();
    if (!failures.contains(message)) failures.append(message);
    report.insert(QStringLiteral("failures"), failures);
    return QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Compact));
}
}

LegacyDataImportController::LegacyDataImportController(
    application::ApplicationServiceGraph *graph, QObject *parent)
    : QObject(parent), m_graph(graph)
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &LegacyDataImportController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &LegacyDataImportController::onWorkFinished);
    connect(&m_graph->operations(), &application::OperationCoordinator::activeCountChanged,
            this, [this] {
        if (m_waitingForMaintenance && m_graph->operations().activeCount() == 0) {
            QTimer::singleShot(0, this, [this] {
                if (m_waitingForMaintenance && m_ready
                    && m_graph->operations().activeCount() == 0) startPreparation();
            });
        }
    });
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, [this](application::ApplicationLifecycle::State state) {
                if (state == application::ApplicationLifecycle::State::Running) {
                    m_ready = true;
                    emit stateChanged();
                    beginRecoveryInspection();
                } else {
                    m_importCancellation.requestCancellation();
                    m_ready = false;
                    m_canImport = false;
                    if (state == application::ApplicationLifecycle::State::Failed) {
                        m_errorMessage = QStringLiteral("Data import is unavailable because profile storage did not start.");
                    }
                    emit stateChanged();
                }
            });
}

void LegacyDataImportController::refreshStatus()
{
    if (!m_ready || m_busy) return;
    const auto operation = m_graph->beginOperation(QStringLiteral("check legacy import status"));
    if (!operation.isValid()) return;
    setBusy(true, QStringLiteral("Checking the Qt profile…"));
    m_storageActions.insert(operation.id.value, StorageAction::Status);
    m_activeOperationId = operation.id.value;
    const bool submitted = m_graph->submitStorage(operation,
        [](storage::StorageRepositories &repositories, const std::function<bool()> &) {
            storage::StorageReply reply;
            storage::DataImportStatus status;
            reply.error = repositories.dataImport.status(&status);
            if (!reply.error.isError()) {
                reply.values = {{QStringLiteral("empty"), status.empty},
                                {QStringLiteral("completed"), status.completed},
                                {QStringLiteral("completedAt"), status.completedAt},
                                {QStringLiteral("blockingReason"), status.blockingReason},
                                {QStringLiteral("reportJson"), status.reportJson}};
            }
            return reply;
        });
    if (!submitted) {
        m_storageActions.remove(operation.id.value);
        setBusy(false);
        setError(QStringLiteral("The Qt profile status could not be read."));
    }
}

void LegacyDataImportController::importProfile(const QUrl &databaseFile,
                                               bool sourceClosedConfirmed)
{
    if (!m_ready || m_busy || !m_canImport || !databaseFile.isLocalFile()) return;
    if (!sourceClosedConfirmed) {
        setError(QStringLiteral("Confirm that Something-v2 is fully closed before importing."));
        return;
    }
    const QString path = databaseFile.toLocalFile();
    if (path.isEmpty()) {
        setError(QStringLiteral("Choose the Something-v2 database.db file."));
        return;
    }
    beginImport(path);
}

void LegacyDataImportController::cancelImport()
{
    if (!m_canCancel) return;
    if (m_waitingForMaintenance) {
        leaveMaintenance();
        m_importInProgress = false;
        setBusy(false);
        setError(QStringLiteral("Import cancelled before source access."));
        return;
    }
    if (m_importCancellation.requestCancellation()) {
        m_canCancel = false;
        m_progressMessage = QStringLiteral("Cancelling and cleaning staged import files…");
        emit stateChanged();
    }
}

bool LegacyDataImportController::submitStorage(
    StorageAction action, application::OperationContext operation,
    storage::StorageCommand command)
{
    if (!operation.isValid()) return false;
    m_storageActions.insert(operation.id.value, action);
    m_activeOperationId = operation.id.value;
    if (m_graph->submitStorage(operation, std::move(command))) return true;
    m_storageActions.remove(operation.id.value);
    return false;
}

bool LegacyDataImportController::submitWork(
    WorkAction action, application::OperationContext operation,
    services::BoundedWorkerPool::ValueWork work)
{
    if (!operation.isValid()) return false;
    m_workActions.insert(operation.id.value, action);
    m_activeOperationId = operation.id.value;
    if (m_graph->submitExpensiveValue(operation, std::move(work))) return true;
    m_workActions.remove(operation.id.value);
    return false;
}

void LegacyDataImportController::beginRecoveryInspection()
{
    if (!m_ready || m_busy) return;
    setBusy(true, QStringLiteral("Checking for an interrupted import…"));
    const auto operation = m_graph->beginOperation(QStringLiteral("inspect import recovery"));
    const QString profile = m_graph->profileDirectory();
    if (!submitWork(WorkAction::InspectRecovery, operation,
        [profile](const application::CancellationToken &) {
            services::BoundedWorkerPool::ValueResult result;
            QString importId;
            bool exists = false;
            result.error = services::LegacyDataImportService::journalImportId(
                profile, &importId, &exists);
            if (!result.error.isError()) {
                if (!exists) services::LegacyDataImportService::cleanupUnjournaledStages(profile);
                result.values = {{QStringLiteral("exists"), exists},
                                 {QStringLiteral("importId"), importId}};
            }
            return result;
        })) {
        setBusy(false);
        setError(QStringLiteral("Import recovery could not be started."));
    }
}

void LegacyDataImportController::beginImport(const QString &databasePath)
{
    if (!m_graph->enterProfileMaintenance()) {
        setError(QStringLiteral("Profile maintenance is already in progress."));
        return;
    }
    m_ownsMaintenance = true;
    m_waitingForMaintenance = true;
    m_importInProgress = true;
    m_importCancellation = application::CancellationSource{};
    m_sourceDatabasePath = databasePath;
    m_importId.clear();
    m_planPath.clear();
    m_reportJson.clear();
    setError({});
    setBusy(true, QStringLiteral("Waiting for background work to finish…"));
    if (m_graph->operations().activeCount() == 0) startPreparation();
}

void LegacyDataImportController::startPreparation()
{
    m_waitingForMaintenance = false;
    setBusy(true, QStringLiteral("Checking that the Qt profile is empty…"));
    const auto operation = m_graph->beginProfileMaintenanceOperation(QStringLiteral("preflight target profile"));
    if (!submitStorage(StorageAction::Preflight, operation,
        [](storage::StorageRepositories &repositories, const std::function<bool()> &) {
            storage::StorageReply reply;
            storage::DataImportStatus status;
            reply.error = repositories.dataImport.status(&status);
            if (!reply.error.isError()) {
                reply.values = {{QStringLiteral("empty"), status.empty},
                                {QStringLiteral("completed"), status.completed},
                                {QStringLiteral("blockingReason"), status.blockingReason}};
            }
            return reply;
        })) {
        leaveMaintenance();
        m_importInProgress = false;
        setBusy(false);
        setError(QStringLiteral("The empty-profile check could not be started."));
    }
}

void LegacyDataImportController::prepareSource()
{
    setBusy(true, QStringLiteral("Creating a consistent source snapshot…"));
    const auto operation = m_graph->beginProfileMaintenanceOperation(QStringLiteral("prepare legacy profile import"));
    const QString databasePath = m_sourceDatabasePath;
    const QString profile = m_graph->profileDirectory();
    const auto importCancellation = m_importCancellation.token();
    if (!submitWork(WorkAction::Prepare, operation,
        [databasePath, profile, importCancellation](const application::CancellationToken &) {
            services::BoundedWorkerPool::ValueResult result;
            result.error = services::LegacyDataImportService::prepare(
                databasePath, profile, true, importCancellation, &result.values);
            return result;
        })) {
        leaveMaintenance();
        m_importInProgress = false;
        setBusy(false);
        setError(QStringLiteral("Profile import could not be started."));
    }
}

void LegacyDataImportController::leaveMaintenance()
{
    m_waitingForMaintenance = false;
    if (m_ownsMaintenance) m_graph->leaveProfileMaintenance();
    m_ownsMaintenance = false;
}

void LegacyDataImportController::beginDatabaseImport()
{
    setBusy(true, QStringLiteral("Writing retained data in one transaction…"));
    const auto operation = m_graph->beginProfileMaintenanceOperation(QStringLiteral("import retained profile data"));
    const QString planPath = m_planPath;
    const QString importId = m_importId;
    const QString profile = m_graph->profileDirectory();
    const auto importCancellation = m_importCancellation.token();
    if (!submitStorage(StorageAction::Import, operation,
        [planPath, importId, profile, importCancellation](storage::StorageRepositories &repositories,
                             const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            QVariantMap report;
            reply.error = services::LegacyDataImportService::installAssets(
                profile, importId, planPath, importCancellation);
            if (!reply.error.isError() && !reply.error.isCancelled()) {
                reply.error = repositories.dataImport.importPlan(planPath, importId, &report,
                    [cancelled, importCancellation] {
                        return (cancelled && cancelled()) || importCancellation.isCancellationRequested();
                    });
            }
            if (reply.error.isError() || reply.error.isCancelled())
                services::LegacyDataImportService::rollback(profile, importId);
            reply.values = std::move(report);
            return reply;
        })) {
        beginRollback(QStringLiteral("The data import could not be queued."));
    }
}

void LegacyDataImportController::beginRollback(QString safeMessage)
{
    m_errorMessage = std::move(safeMessage);
    if (m_importId.isEmpty()) {
        leaveMaintenance();
        m_importInProgress = false;
        setBusy(false);
        emit stateChanged();
        return;
    }
    setBusy(true, QStringLiteral("Removing staged files…"));
    m_canCancel = false;
    emit stateChanged();
    const auto operation = m_graph->beginProfileMaintenanceOperation(QStringLiteral("rollback profile import files"));
    const QString profile = m_graph->profileDirectory();
    const QString importId = m_importId;
    const QString reportJson = m_reportJson;
    if (!submitWork(WorkAction::Rollback, operation,
        [profile, importId, reportJson](const application::CancellationToken &) {
            services::BoundedWorkerPool::ValueResult result;
            const auto reportError = services::LegacyDataImportService::saveReport(profile, importId, reportJson);
            result.error = services::LegacyDataImportService::rollback(profile, importId);
            if (!result.error.isError()) result.error = reportError;
            return result;
        })) {
        leaveMaintenance();
        m_importInProgress = false;
        setBusy(false);
        emit stateChanged();
    }
}

void LegacyDataImportController::setBusy(bool busy, QString progress)
{
    m_busy = busy;
    m_progressMessage = std::move(progress);
    m_canCancel = busy && m_importInProgress && !m_imported;
    emit stateChanged();
}

void LegacyDataImportController::setError(QString message)
{
    m_errorMessage = std::move(message);
    emit stateChanged();
}

void LegacyDataImportController::onStorageFinished(
    quint64 operationId, const QVariantMap &values, domain::AppError error)
{
    const auto found = m_storageActions.find(operationId);
    if (found == m_storageActions.end()) return;
    const StorageAction action = found.value();
    m_storageActions.erase(found);
    if (m_activeOperationId == operationId) m_activeOperationId = 0;

    if (action == StorageAction::Preflight) {
        const bool cancelled = m_importCancellation.token().isCancellationRequested() || error.isCancelled();
        if (error.isError() || cancelled || !values.value(QStringLiteral("empty")).toBool()
            || values.value(QStringLiteral("completed")).toBool()) {
            leaveMaintenance();
            m_importInProgress = false;
            m_canImport = !error.isError() && values.value(QStringLiteral("empty")).toBool()
                && !values.value(QStringLiteral("completed")).toBool();
            setBusy(false);
            if (cancelled) {
                setError(QStringLiteral("Import cancelled before source access. Recheck the profile to try again."));
            } else if (error.isError()) {
                m_profileCheckFailed = true;
                m_eligibilityMessage.clear();
                setError(QStringLiteral("Could not check the Qt profile before import. %1 Recheck to try again; the source has not been read.")
                    .arg(error.userMessage()));
            } else {
                m_eligibilityMessage = values.value(QStringLiteral("completed")).toBool()
                    ? QStringLiteral("This Qt profile already has a completed import. Its existing data will not be overwritten.")
                    : values.value(QStringLiteral("blockingReason")).toString();
                setError({});
            }
            return;
        }
        prepareSource();
        return;
    }

    if (action == StorageAction::RecoveryCheck) {
        if (error.isError() || error.isCancelled()) {
            setBusy(false);
            setError(QStringLiteral("The interrupted import could not be recovered safely. Its journal and any changed files were retained."));
            return;
        }
        m_recoveryId.clear();
        setBusy(false);
        refreshStatus();
        return;
    }

    if (action == StorageAction::Import) {
        if (error.isError() || error.isCancelled()) {
            const QString message = error.isCancelled()
                ? QStringLiteral("Import cancelled. Staged changes are being removed.")
                : QStringLiteral("Import failed. No partial database records were committed.");
            if (!values.value(QStringLiteral("json")).toString().isEmpty()) m_reportJson = values.value(QStringLiteral("json")).toString();
            m_reportJson = failureReport(m_reportJson, message);
            beginRollback(message);
            return;
        }
        m_reportJson = values.value(QStringLiteral("json")).toString();
        m_imported = true;
        m_restartRequired = true;
        m_canImport = false;
        m_eligibilityMessage = QStringLiteral("Import completed. Restart Something to load the imported profile.");
        setBusy(true, QStringLiteral("Import complete; cleaning temporary files…"));
        const auto operation = m_graph->beginProfileMaintenanceOperation(QStringLiteral("finish profile import"));
        const QString profile = m_graph->profileDirectory();
        const QString importId = m_importId;
        const QString reportJson = m_reportJson;
        if (!submitWork(WorkAction::Finish, operation,
            [profile, importId, reportJson](const application::CancellationToken &) {
                services::BoundedWorkerPool::ValueResult result;
                const auto reportError = services::LegacyDataImportService::saveReport(profile, importId, reportJson);
                result.error = services::LegacyDataImportService::finish(profile, importId);
                if (!result.error.isError()) result.error = reportError;
                return result;
            })) {
            setBusy(false);
            setError(QStringLiteral("Data was imported. The completion report is saved in the database; file cleanup remains pending."));
        }
        return;
    }

    if (action == StorageAction::Status) {
        if (error.isError() || error.isCancelled()) {
            setBusy(false);
            m_canImport = false;
            m_profileCheckFailed = true;
            m_eligibilityMessage.clear();
            emit stateChanged();
            setError(QStringLiteral("The Qt profile status could not be read."));
            return;
        }
        m_imported = values.value(QStringLiteral("completed")).toBool();
        m_canImport = !m_imported && values.value(QStringLiteral("empty")).toBool();
        m_eligibilityMessage = m_imported
            ? QStringLiteral("This Qt profile already has a completed import.")
            : m_canImport ? QStringLiteral("Ready to import into this Qt profile.")
                          : values.value(QStringLiteral("blockingReason")).toString();
        if (m_imported) m_reportJson = values.value(QStringLiteral("reportJson")).toString();
        setBusy(false);
        if (m_profileCheckFailed || m_imported || m_canImport) setError({});
        m_profileCheckFailed = false;
    }
}

void LegacyDataImportController::onWorkFinished(
    quint64 operationId, const QVariantMap &values, domain::AppError error)
{
    const auto found = m_workActions.find(operationId);
    if (found == m_workActions.end()) return;
    const WorkAction action = found.value();
    m_workActions.erase(found);
    if (m_activeOperationId == operationId) m_activeOperationId = 0;

    if (action == WorkAction::InspectRecovery) {
        if (error.isError() || error.isCancelled()) {
            setBusy(false);
            setError(QStringLiteral("Import recovery could not be inspected."));
            return;
        }
        if (!values.value(QStringLiteral("exists")).toBool()) {
            setBusy(false);
            refreshStatus();
            return;
        }
        m_recoveryId = values.value(QStringLiteral("importId")).toString();
        const auto operation = m_graph->beginOperation(QStringLiteral("check interrupted import marker"));
        const QString importId = m_recoveryId;
        const QString profile = m_graph->profileDirectory();
        if (!submitStorage(StorageAction::RecoveryCheck, operation,
            [importId, profile](storage::StorageRepositories &repositories,
                       const std::function<bool()> &) {
                storage::StorageReply reply;
                bool completed = false;
                reply.error = repositories.dataImport.hasCompletion(importId, &completed);
                if (!reply.error.isError() && !completed) {
                    storage::DataImportStatus status;
                    reply.error = repositories.dataImport.status(&status);
                    if (!reply.error.isError() && !status.empty)
                        reply.error = domain::AppError(domain::ErrorCategory::Conflict);
                }
                if (!reply.error.isError()) {
                    reply.error = services::LegacyDataImportService::recover(profile, importId, completed);
                    reply.values.insert(QStringLiteral("committed"), completed);
                }
                return reply;
            })) {
            setBusy(false);
            setError(QStringLiteral("Import recovery could not query profile storage."));
        }
        return;
    }

    if (action == WorkAction::Prepare) {
        if (error.isError() || error.isCancelled()) {
            m_importId = values.value(QStringLiteral("importId")).toString();
            m_reportJson = values.value(QStringLiteral("reportJson")).toString();
            const QString message = error.isCancelled()
                ? QStringLiteral("Import cancelled before any profile records were committed.")
                : QStringLiteral("Could not create a supported, consistent source snapshot. Check the source file and close Something-v2.");
            m_reportJson = failureReport(m_reportJson, message);
            beginRollback(message);
            return;
        }
        m_importId = values.value(QStringLiteral("importId")).toString();
        m_planPath = values.value(QStringLiteral("planPath")).toString();
        m_reportJson = values.value(QStringLiteral("reportJson")).toString();
        beginDatabaseImport();
        return;
    }

    if (action == WorkAction::Rollback) {
        leaveMaintenance();
        m_importInProgress = false;
        setBusy(false);
        m_importId.clear();
        m_planPath.clear();
        emit stateChanged();
        if (error.isError() || error.isCancelled()) {
            m_canImport = false;
            setError(QStringLiteral("Import did not commit. Cleanup or report saving was incomplete; restart to inspect recovery."));
            return;
        }
        refreshStatus();
        return;
    }

    if (action == WorkAction::Finish) {
        m_importInProgress = false;
        setBusy(false);
        m_importId.clear();
        m_planPath.clear();
        setError(error.isError() || error.isCancelled()
            ? QStringLiteral("Import committed. Its database report is saved; file cleanup or report export was incomplete.") : QString());
    }
}

} // namespace something::controllers
