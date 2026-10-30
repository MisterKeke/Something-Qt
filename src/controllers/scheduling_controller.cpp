#include "scheduling_controller.h"

#include "../domain/scheduling_engine.h"
#include "../services/scheduling_service.h"
#include "../services/task_service.h"

#include <QThread>
#include <QDate>
#include <QTime>
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
    if (m_applying || m_preferencesSaving) return;
    m_previewVisible = true;
    m_errorMessage.clear();
    m_statusMessage.clear();
    m_warnings.clear();
    m_daySummaries.clear();
    m_todayDate.clear();
    m_previewPreferencesRevision = 0;
    m_recommendations.clear();
    m_previewAvailable = false;
    m_previewStale = false;
    m_preferencesAvailable = false;
    m_preferencesErrorMessage.clear();

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

void SchedulingController::applyAccepted()
{
    if (!canApply() || m_graph->lifecycle().state()
            != application::ApplicationLifecycle::State::Running) return;
    if (m_previewPreferencesRevision <= 0) {
        m_errorMessage = QStringLiteral("Scheduling settings are missing from this preview.");
        emit stateChanged();
        return;
    }

    QVector<domain::TaskScheduleSnapshot> taskSnapshot;
    const QVariantList recommendationRows = m_recommendations.recommendationRows();
    taskSnapshot.reserve(recommendationRows.size());
    for (const QVariant &value : recommendationRows) {
        const QVariantMap row = value.toMap();
        domain::TaskScheduleSnapshot snapshot;
        snapshot.taskId = row.value(QStringLiteral("taskId")).toLongLong();
        snapshot.taskRevision = row.value(QStringLiteral("taskRevision")).toLongLong();
        snapshot.scheduleRevision = row.value(QStringLiteral("scheduleRevision")).toLongLong();
        if (snapshot.taskId <= 0 || snapshot.taskRevision <= 0 || snapshot.scheduleRevision < 0) {
            m_errorMessage = QStringLiteral("The task snapshot is invalid. Refresh the preview.");
            emit stateChanged();
            return;
        }
        taskSnapshot.push_back(snapshot);
    }

    QVector<domain::TaskScheduleProposal> proposals;
    const QVariantList acceptedRows = m_recommendations.acceptedRecommendations();
    proposals.reserve(acceptedRows.size());
    for (const QVariant &value : acceptedRows) {
        const QVariantMap row = value.toMap();
        domain::TaskScheduleProposal proposal;
        domain::TaskSchedule &schedule = proposal.schedule;
        schedule.taskId = row.value(QStringLiteral("taskId")).toLongLong();
        const QString plannedDate = row.value(QStringLiteral("plannedDate")).toString();
        const QDate parsedDate = QDate::fromString(plannedDate, Qt::ISODate);
        if (!parsedDate.isValid() || parsedDate.toString(Qt::ISODate) != plannedDate) {
            m_errorMessage = QStringLiteral("An accepted recommendation has no valid planned date.");
            emit stateChanged();
            return;
        }
        schedule.plannedDate = parsedDate;
        const QString plannedStartTime = row.value(QStringLiteral("plannedStartTime")).toString();
        if (!plannedStartTime.isEmpty()) {
            const QTime parsedTime = QTime::fromString(plannedStartTime,
                                                        QStringLiteral("HH:mm"));
            if (!parsedTime.isValid()
                || parsedTime.toString(QStringLiteral("HH:mm")) != plannedStartTime) {
                m_errorMessage = QStringLiteral("An accepted recommendation has an invalid start time.");
                emit stateChanged();
                return;
            }
            schedule.plannedStartTime = parsedTime;
        }
        schedule.estimatedDurationMinutes =
            row.value(QStringLiteral("estimatedDurationMinutes")).toInt();
        schedule.revision = row.value(QStringLiteral("scheduleRevision")).toLongLong();
        proposal.expectedTaskRevision = row.value(QStringLiteral("taskRevision")).toLongLong();
        if (!row.value(QStringLiteral("canAccept")).toBool()
            || proposal.expectedTaskRevision <= 0 || schedule.revision < 0
            || domain::normalizeTaskSchedule(schedule).isError()) {
            m_errorMessage = QStringLiteral("An accepted recommendation is invalid. Refresh the preview.");
            emit stateChanged();
            return;
        }
        proposals.push_back(std::move(proposal));
    }
    if (proposals.isEmpty()) return;

    const auto operation = m_graph->beginOperation(
        QStringLiteral("apply accepted schedule recommendations"),
        QStringLiteral("smart-schedule-apply"));
    if (!operation.isValid()) {
        m_errorMessage = QStringLiteral("The accepted schedules could not be applied right now.");
        emit stateChanged();
        return;
    }

    const quint64 id = operation.id.value;
    m_currentOperationId = id;
    m_pending.insert(id, Action::ApplySchedules);
    m_applying = true;
    m_errorMessage.clear();
    m_statusMessage = QStringLiteral("Saving accepted schedules atomically…");
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::SchedulingService::applyAcceptedSchedules(
            std::move(proposals), std::move(taskSnapshot), m_previewPreferencesRevision))) {
        const bool wasPending = m_pending.remove(id) > 0;
        if (m_currentOperationId == id) m_currentOperationId = 0;
        if (wasPending) {
            m_applying = false;
            m_errorMessage = QStringLiteral("The accepted schedules could not be submitted.");
            m_statusMessage.clear();
            emit stateChanged();
        }
    }
}

void SchedulingController::savePreferences(QVariantMap values)
{
    if (!m_preferencesAvailable || m_preferencesSaving || m_loading || m_applying
        || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running)
        return;

    values.insert(QStringLiteral("revision"), m_preferences.value(QStringLiteral("revision")));
    values.insert(QStringLiteral("updatedAt"), m_preferences.value(QStringLiteral("updatedAt")));
    domain::SchedulingPreferences preferences;
    const domain::AppError validationError = services::SchedulingService::preferencesFromVariantMap(
        values, &preferences);
    if (validationError.isError()) {
        m_preferencesErrorMessage = QStringLiteral(
            "Check the working days, HH:mm work hours, daily capacity, estimate, and break values.");
        emit stateChanged();
        return;
    }

    const qint64 expectedRevision = preferences.revision;
    const auto operation = m_graph->beginOperation(
        QStringLiteral("save smart scheduling preferences"),
        QStringLiteral("smart-schedule-save-preferences"));
    if (!operation.isValid()) {
        m_preferencesErrorMessage = QStringLiteral("Scheduling settings could not be saved right now.");
        emit stateChanged();
        return;
    }

    const quint64 id = operation.id.value;
    m_currentOperationId = id;
    m_pending.insert(id, Action::SavePreferences);
    m_preferencesSaving = true;
    m_preferencesErrorMessage.clear();
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::SchedulingService::savePreferences(
            std::move(preferences), expectedRevision))) {
        const bool wasPending = m_pending.remove(id) > 0;
        if (m_currentOperationId == id) m_currentOperationId = 0;
        if (wasPending) {
            m_preferencesSaving = false;
            m_preferencesErrorMessage = QStringLiteral("Scheduling settings could not be submitted.");
            emit stateChanged();
        }
    }
}

void SchedulingController::rejectProposal()
{
    if (m_applying || m_preferencesSaving) return;
    cancelCurrentOperation();
    m_recommendations.clear();
    m_daySummaries.clear();
    m_warnings.clear();
    m_todayDate.clear();
    m_previewPreferencesRevision = 0;
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
    if (found == m_pending.end() || operationId != m_currentOperationId) return;
    const Action action = found.value();
    m_pending.erase(found);
    m_currentOperationId = 0;
    if (action == Action::SavePreferences) {
        m_preferencesSaving = false;
        if (error.isCancelled()) {
            m_preferencesErrorMessage = QStringLiteral("Saving settings was cancelled; no changes were saved.");
            emit stateChanged();
            return;
        }
        if (error.isError()) {
            m_preferencesErrorMessage = error.category() == domain::ErrorCategory::Conflict
                ? QStringLiteral("Scheduling settings changed since they were loaded. Discard changes to reload them, then try again.")
                : (error.userMessage().isEmpty()
                       ? QStringLiteral("Scheduling settings could not be saved.")
                       : error.userMessage());
            emit stateChanged();
            return;
        }
        domain::SchedulingPreferences savedPreferences;
        const domain::AppError parseError = services::SchedulingService::preferencesFromVariantMap(
            values.value(QStringLiteral("preferences")).toMap(), &savedPreferences);
        if (parseError.isError()) {
            m_preferencesErrorMessage = QStringLiteral("Saved settings could not be read back.");
            emit stateChanged();
            return;
        }
        m_preferences = services::SchedulingService::toVariantMap(savedPreferences);
        m_preferencesAvailable = true;
        m_preferencesErrorMessage.clear();
        emit preferencesSaved();
        emit stateChanged();
        generatePreview();
        return;
    }
    if (action == Action::ApplySchedules) {
        finishApply(values, error);
        return;
    }
    if (action != Action::LoadSnapshot) return;
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
        m_preferencesAvailable = false;
        m_preferencesErrorMessage = QStringLiteral("Scheduling preferences could not be read.");
        m_errorMessage = QStringLiteral("Scheduling preferences could not be read.");
        return false;
    }
    m_preferences = services::SchedulingService::toVariantMap(input.preferences);
    m_preferencesAvailable = true;
    m_preferencesErrorMessage.clear();
    m_previewPreferencesRevision = input.preferences.revision;
    emit preferencesLoaded();

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
    if (m_loading || m_applying || m_preferencesSaving) {
        const bool wasLoading = m_loading;
        m_loading = false;
        m_applying = false;
        m_preferencesSaving = false;
        if (wasLoading) m_previewAvailable = false;
        if (state == application::ApplicationLifecycle::State::Failed)
            m_errorMessage = m_graph->lifecycle().startupError().userMessage();
        else
            m_statusMessage = QStringLiteral("Scheduling stopped before completion.");
        emit stateChanged();
    }
}

void SchedulingController::finishApply(const QVariantMap &values, domain::AppError error)
{
    m_applying = false;
    if (error.isCancelled()) {
        m_statusMessage = QStringLiteral("The schedule batch was cancelled before it was saved.");
        emit stateChanged();
        return;
    }
    if (error.isError()) {
        if (error.category() == domain::ErrorCategory::Conflict
            || error.category() == domain::ErrorCategory::NotFound) {
            m_previewStale = true;
            m_recommendations.clearAcceptance();
            m_errorMessage = QStringLiteral(
                "Tasks, scheduling preferences, or saved plans changed since this preview. "
                "No schedules were applied. Refresh the preview and accept them again.");
            m_statusMessage = QStringLiteral("The preview is stale.");
        } else {
            m_errorMessage = error.userMessage().isEmpty()
                ? QStringLiteral("No schedules were applied.") : error.userMessage();
            m_statusMessage = QStringLiteral("The batch was rolled back. You can retry or reject it.");
        }
        emit stateChanged();
        return;
    }

    const int acceptedCount = values.value(QStringLiteral("acceptedCount")).toInt();
    const int changedCount = values.value(QStringLiteral("changedCount")).toInt();
    m_recommendations.clear();
    m_daySummaries.clear();
    m_warnings.clear();
    m_previewAvailable = false;
    m_previewStale = false;
    m_previewPreferencesRevision = 0;
    m_errorMessage.clear();
    m_statusMessage = changedCount == 0
        ? QStringLiteral("No schedule records needed changes for the %1 accepted recommendation(s). Generate a fresh preview when ready.")
              .arg(acceptedCount)
        : QStringLiteral("Applied %1 accepted recommendation(s); %2 schedule record(s) changed. Generate a fresh preview when ready.")
              .arg(acceptedCount).arg(changedCount);
    emit schedulesSaved();
    emit stateChanged();
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
    if (m_applying) {
        m_statusMessage = QStringLiteral(
            "Task data changed while applying. The stored revisions are being checked.");
        emit stateChanged();
        return;
    }
    if (m_preferencesSaving) {
        m_previewStale = true;
        m_recommendations.clearAcceptance();
        m_statusMessage = QStringLiteral(
            "Task data changed while settings were saving. Recommendations will refresh after the save completes.");
        emit stateChanged();
        return;
    }
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
