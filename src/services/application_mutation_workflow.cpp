#include "application_mutation_workflow.h"

#include "task_service.h"
#include "preferences_service.h"
#include "../application/operation_coordinator.h"
#include "../domain/task_rules.h"

namespace something::services {
namespace {

QString codeFor(domain::ErrorCategory category)
{
    using C = domain::ErrorCategory;
    switch (category) {
    case C::Validation: return QStringLiteral("validation");
    case C::NotFound: return QStringLiteral("not-found");
    case C::Conflict: return QStringLiteral("conflict");
    case C::Cancelled: return QStringLiteral("cancelled");
    case C::Storage: return QStringLiteral("storage");
    case C::Busy: return QStringLiteral("busy");
    case C::CapabilityUnavailable: return QStringLiteral("unavailable");
    default: return QStringLiteral("operation-failed");
    }
}

MutationAdmission rejected(domain::AppError error)
{
    return {false, codeFor(error.category()), error.userMessage()};
}

} // namespace

ApplicationMutationWorkflow::ApplicationMutationWorkflow(
    application::ApplicationServiceGraph *graph, QObject *parent)
    : QObject(parent), m_graph(graph)
{
    Q_ASSERT(m_graph);
    qRegisterMetaType<MutationResult>();
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &ApplicationMutationWorkflow::onStorageFinished);
    connect(&m_graph->operations(), &application::OperationCoordinator::operationFinished,
            this, &ApplicationMutationWorkflow::onOperationFinished);
}

MutationAdmission ApplicationMutationWorkflow::addTask(
    QString requestId, storage::TaskInput input, std::function<void()> admitted)
{
    const domain::AppError validation = domain::normalizeTaskInput(input);
    if (validation.isError()) return rejected(validation);
    return dispatch(std::move(requestId), QStringLiteral("task-add"),
                    TaskService::create(std::move(input)), std::move(admitted));
}

MutationAdmission ApplicationMutationWorkflow::completeTask(
    QString requestId, qint64 id, qint64 revision, bool completed,
    std::function<void()> admitted)
{
    if (id <= 0 || revision <= 0)
        return rejected(domain::AppError(domain::ErrorCategory::Validation));
    return dispatch(std::move(requestId), QStringLiteral("task-complete"),
                    TaskService::setCompleted(id, revision, completed), std::move(admitted));
}

MutationAdmission ApplicationMutationWorkflow::setWeekStart(
    QString requestId, int sundayZero, std::function<void()> admitted)
{
    if (sundayZero < 0 || sundayZero > 6)
        return rejected(domain::AppError(domain::ErrorCategory::Validation));
    return dispatch(std::move(requestId), QStringLiteral("week-start"),
                    PreferencesService::setWeekStart(sundayZero), std::move(admitted));
}

MutationAdmission ApplicationMutationWorkflow::dispatch(
    QString requestId, QString kind, storage::StorageCommand command,
    std::function<void()> admitted)
{
    if (requestId.isEmpty())
        return {false, QStringLiteral("validation"), QStringLiteral("A mutation request ID is required.")};
    using State = application::ApplicationLifecycle::State;
    const State state = m_graph->lifecycle().state();
    if (state == State::Starting)
        return {false, QStringLiteral("starting"), QStringLiteral("Application startup is still in progress.")};
    if (state == State::Failed)
        return {false, QStringLiteral("unavailable"), QStringLiteral("Application startup failed; application data is unavailable.")};
    if (state == State::ShuttingDown || state == State::Stopped)
        return {false, QStringLiteral("shutdown"), QStringLiteral("The application is shutting down.")};
    if (m_graph->isProfileMaintenance())
        return {false, QStringLiteral("maintenance"), QStringLiteral("Mutations are unavailable during profile maintenance.")};
    if (!m_graph->capabilities().isAvailable(QStringLiteral("local-storage")))
        return rejected(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
    if (m_pending.size() >= MaximumPendingRequests)
        return rejected(domain::AppError(domain::ErrorCategory::Busy));
    if (m_pending.contains(requestId))
        return rejected(domain::AppError(domain::ErrorCategory::Busy));
    const auto operation = m_graph->beginOperation(QStringLiteral("application mutation: ") + kind);
    if (!operation.isValid()) return rejected(domain::AppError(domain::ErrorCategory::Busy));

    m_pending.insert(requestId, {std::move(kind), operation.id.value, false});
    m_byOperation.insert(operation.id.value, requestId);
    if (admitted) admitted();
    if (!m_graph->submitStorage(operation, std::move(command))) {
        // submitStorage may synchronously publish its rejection; finish() is
        // idempotent, so the exactly-once result invariant is retained.
        if (m_pending.contains(requestId))
            finish(requestId, QStringLiteral("failed"), domain::AppError(domain::ErrorCategory::Busy));
    }
    return {true, {}, {}};
}

void ApplicationMutationWorkflow::finish(QString requestId, QString state,
                                         domain::AppError error, const QVariantMap &values)
{
    auto it = m_pending.find(requestId);
    if (it == m_pending.end() || it->final) return;
    it->final = true;
    const QString kind = it->kind;
    const quint64 operationId = it->operationId;
    // Own the ID before removing the operation map: callers may pass its value.
    m_pending.erase(it);
    m_byOperation.remove(operationId);

    MutationResult result;
    result.requestId = requestId;
    result.kind = kind;
    result.state = std::move(state);
    result.errorCode = error.isError() || error.isCancelled() ? codeFor(error.category()) : QString();
    result.message = error.isError() ? error.userMessage()
        : (error.isCancelled() ? QStringLiteral("Cancellation was requested; the change may already have committed.") : QString());
    result.data = values;
    if (result.state == QStringLiteral("succeeded")) {
        if (kind.startsWith(QStringLiteral("task-"))) {
            const QVariantMap task = values.value(QStringLiteral("task")).toMap();
            result.data = {{QStringLiteral("task"), task}};
            emit taskCommitted(requestId, task, kind == QStringLiteral("task-add"));
        } else if (kind == QStringLiteral("week-start")) {
            emit weekStartCommitted(requestId, values.value(QStringLiteral("weekStart")).toInt());
            result.data = {{QStringLiteral("weekStart"), values.value(QStringLiteral("weekStart"))}};
        }
    }
    emit requestFinished(result);
}

void ApplicationMutationWorkflow::onStorageFinished(quint64 operationId,
                                                     const QVariantMap &values,
                                                     domain::AppError error)
{
    const auto found = m_byOperation.constFind(operationId);
    if (found == m_byOperation.cend()) return;
    finish(found.value(), error.isCancelled() ? QStringLiteral("cancelled")
          : (error.isError() ? QStringLiteral("failed") : QStringLiteral("succeeded")), error, values);
}

void ApplicationMutationWorkflow::onOperationFinished(
    quint64 operationId, application::OperationCoordinator::State state, domain::AppError error)
{
    Q_UNUSED(error);
    if (state != application::OperationCoordinator::State::Cancelled) return;
    const auto found = m_byOperation.constFind(operationId);
    if (found != m_byOperation.cend())
        finish(found.value(), QStringLiteral("cancelled"), domain::AppError::cancelled());
}

bool ApplicationMutationWorkflow::cancel(const QString &requestId)
{
    const auto it = m_pending.constFind(requestId);
    return it != m_pending.cend() && m_graph->cancelOperation({it->operationId});
}

} // namespace something::services
