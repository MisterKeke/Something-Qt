#include "scheduling_controller.h"

#include "../domain/scheduling_engine.h"
#include "../services/scheduling_service.h"
#include "../services/task_service.h"

#include <QThread>
#include <QVariantMap>

#include <utility>

namespace something::controllers {

SchedulingController::SchedulingController(application::ApplicationServiceGraph *graph,
                                           const domain::Clock *clock,
                                           QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_clock(clock)
    , m_recommendations(this)
{
    Q_ASSERT(m_graph && m_clock);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &SchedulingController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &SchedulingController::onCalculationFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &SchedulingController::onLifecycleChanged);
}

void SchedulingController::generatePreview()
{
    m_previewVisible = true;
    m_errorMessage.clear();
    m_statusMessage.clear();
    m_warnings.clear();
    m_daySummaries.clear();
    m_todayDate.clear();
    m_recommendations.clear();
    m_previewAvailable = false;
    m_previewStale = false;

    if (m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running) {
        m_loading = false;
        m_errorMessage = QStringLiteral("Task storage is not ready yet. Try again shortly.");
        emit stateChanged();
        return;
    }

    cancelCurrentOperation();
    m_previewNowUtc = m_clock->nowUtc().toUTC();
    m_previewTimeZone = m_clock->localTimeZone();
    m_loading = true;
    emit stateChanged();

    const auto operation = m_graph->beginOperation(
        QStringLiteral("load smart schedule preview snapshot"),
        QStringLiteral("smart-schedule-preview"));
    if (!operation.isValid()) {
        m_loading = false;
        m_errorMessage = QStringLiteral("A schedule preview could not be started.");
        emit stateChanged();
        return;
    }

    m_currentOperationId = operation.id.value;
    m_pending.insert(m_currentOperationId, Action::LoadSnapshot);
    if (!m_graph->submitStorage(operation, services::SchedulingService::loadPreviewSnapshot())
        && m_pending.remove(m_currentOperationId) > 0) {
        m_currentOperationId = 0;
        m_loading = false;
        m_errorMessage = QStringLiteral("The task snapshot could not be loaded.");
        emit stateChanged();
    }
}

void SchedulingController::setRecommendationAccepted(qint64 taskId, bool accepted)
{
    if (!canStage()) return;
    if (m_recommendations.setAccepted(taskId, accepted)) emit stateChanged();
}

void SchedulingController::acceptAllValid()
{
    if (!canStage()) return;
    m_recommendations.acceptAllValid();
    emit stateChanged();
}

void SchedulingController::rejectProposal()
{
    cancelCurrentOperation();
    m_recommendations.clear();
    m_daySummaries.clear();
    m_warnings.clear();
    m_todayDate.clear();
    m_errorMessage.clear();
    m_statusMessage.clear();
    m_loading = false;
    m_previewVisible = false;
    m_previewAvailable = false;
    m_previewStale = false;
    emit stateChanged();
}

void SchedulingController::onTaskMutationCommitted(qint64 taskId,
                                                   const QVariantMap &currentTask,
                                                   bool deleted)
{
    Q_UNUSED(taskId);
    Q_UNUSED(currentTask);
    Q_UNUSED(deleted);
    invalidatePreview();
}

void SchedulingController::onTaskMutationMayHaveCommitted()
{
    invalidatePreview();
}

void SchedulingController::onStorageFinished(quint64 operationId,
                                             const QVariantMap &values,
                                             domain::AppError error)
{
    const auto found = m_pending.find(operationId);
    if (found == m_pending.end() || found.value() != Action::LoadSnapshot
        || operationId != m_currentOperationId) return;
    m_pending.erase(found);
    m_currentOperationId = 0;
    if (error.isError()) {
        finishWithError(error, QStringLiteral("Task and schedule data could not be loaded."));
        return;
    }
    if (error.isCancelled()) {
        m_loading = false;
        emit stateChanged();
        return;
    }
    if (!submitCalculation(values)) {
        m_loading = false;
        if (m_errorMessage.isEmpty())
            m_errorMessage = QStringLiteral("The schedule preview could not be calculated.");
        emit stateChanged();
    }
}

bool SchedulingController::submitCalculation(const QVariantMap &snapshot)
{
    domain::SchedulingInput input;
    input.nowUtc = m_previewNowUtc;
    input.localTimeZone = m_previewTimeZone;

    const QVariantList taskRows = snapshot.value(QStringLiteral("tasks")).toList();
    input.incompleteTasks.reserve(taskRows.size());
    for (const QVariant &row : taskRows)
        input.incompleteTasks.push_back(services::TaskService::fromVariantMap(row.toMap()));

    const QVariantList scheduleRows = snapshot.value(QStringLiteral("schedules")).toList();
    input.existingSchedules.reserve(scheduleRows.size());
    for (const QVariant &row : scheduleRows) {
        domain::TaskSchedule schedule;
        const domain::AppError error = services::SchedulingService::taskScheduleFromVariantMap(
            row.toMap(), &schedule);
        if (error.isError()) {
            m_errorMessage = QStringLiteral("Saved schedule metadata could not be read.");
            return false;
        }
        input.existingSchedules.push_back(std::move(schedule));
    }

    const domain::AppError preferencesError =
        services::SchedulingService::preferencesFromVariantMap(
            snapshot.value(QStringLiteral("preferences")).toMap(), &input.preferences);
    if (preferencesError.isError()) {
        m_errorMessage = QStringLiteral("Scheduling preferences could not be read.");
        return false;
    }

    const auto operation = m_graph->beginOperation(
        QStringLiteral("calculate smart schedule preview"),
        QStringLiteral("smart-schedule-preview"));
    if (!operation.isValid()) return false;
    const quint64 id = operation.id.value;
    m_currentOperationId = id;
    m_pending.insert(id, Action::Calculate);
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [input = std::move(input)](const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult job;
            if (cancellation.isCancellationRequested()) {
                job.error = domain::AppError::cancelled();
                return job;
            }
            domain::SchedulingResult result;
            job.error = domain::calculateScheduleRecommendations(input, &result);
            if (!job.error.isError()) {
                if (cancellation.isCancellationRequested()) {
                    job.error = domain::AppError::cancelled();
                } else {
                    job.values = services::SchedulingService::toVariantMap(result);
                }
            }
            return job;
        });
    if (!submitted) {
        const bool wasPending = m_pending.remove(id) > 0;
        if (m_currentOperationId == id) m_currentOperationId = 0;
        return !wasPending; // A rejected submission may publish its result synchronously.
    }
    return true;
}

void SchedulingController::onCalculationFinished(quint64 operationId,
                                                 const QVariantMap &values,
                                                 domain::AppError error)
{
    const auto found = m_pending.find(operationId);
    if (found == m_pending.end() || found.value() != Action::Calculate
        || operationId != m_currentOperationId) return;
    m_pending.erase(found);
    m_currentOperationId = 0;
    if (error.isError()) {
        finishWithError(error, QStringLiteral("The schedule recommendations could not be calculated."));
        return;
    }
    if (error.isCancelled()) {
        m_loading = false;
        emit stateChanged();
        return;
    }

    m_recommendations.setRecommendations(values.value(QStringLiteral("recommendations")).toList());
    m_daySummaries = values.value(QStringLiteral("days")).toList();
    m_warnings = values.value(QStringLiteral("warnings")).toStringList();
    m_todayDate = values.value(QStringLiteral("today")).toString();
    m_loading = false;
    m_previewAvailable = true;
    m_statusMessage = m_recommendations.rowCount() == 0
        ? QStringLiteral("There are no incomplete tasks to schedule. Nothing has been saved.")
        : QStringLiteral("Accepted recommendations are staged in memory. No schedules have been saved.");
    emit stateChanged();
}

void SchedulingController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    if (state != application::ApplicationLifecycle::State::Failed
        && state != application::ApplicationLifecycle::State::ShuttingDown
        && state != application::ApplicationLifecycle::State::Stopped) return;
    cancelCurrentOperation();
    if (m_loading) {
        m_loading = false;
        m_previewAvailable = false;
        if (state == application::ApplicationLifecycle::State::Failed)
            m_errorMessage = m_graph->lifecycle().startupError().userMessage();
        emit stateChanged();
    }
}

void SchedulingController::cancelCurrentOperation()
{
    if (!m_currentOperationId) return;
    const quint64 operationId = m_currentOperationId;
    m_currentOperationId = 0;
    m_pending.remove(operationId);
    m_graph->cancelOperation({operationId});
}

void SchedulingController::invalidatePreview()
{
    if (!m_previewVisible) return;
    const bool wasLoading = m_loading;
    cancelCurrentOperation();
    m_loading = false;
    m_previewStale = true;
    m_recommendations.clearAcceptance();
    if (wasLoading) m_previewAvailable = false;
    m_statusMessage = QStringLiteral(
        "Task data changed. Generate a fresh preview before staging these recommendations.");
    emit stateChanged();
}

void SchedulingController::finishWithError(domain::AppError error, const QString &fallback)
{
    m_loading = false;
    m_previewAvailable = false;
    m_errorMessage = error.userMessage().isEmpty() ? fallback : error.userMessage();
    emit stateChanged();
}

} // namespace something::controllers
