#include "task_history_controller.h"

#include "../application/application_lifecycle.h"
#include "../services/task_history_service.h"

#include <QThread>

#include <utility>

namespace something::controllers {

TaskHistoryController::TaskHistoryController(application::ApplicationServiceGraph *graph,
                                             QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_model()
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &TaskHistoryController::onStorageFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &TaskHistoryController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

void TaskHistoryController::loadTaskHistory(qint64 taskId, const QString &eventType)
{
    if (taskId <= 0) {
        setError(QStringLiteral("Choose a valid task to load its history."));
        return;
    }
    if (!setEventType(eventType)) return;
    m_taskScoped = true;
    m_taskId = taskId;
    m_taskTitleQuery.clear();
    m_hasQuery = true;
    startLoad(false);
}

void TaskHistoryController::loadRecentHistory(const QString &taskTitleQuery,
                                              const QString &eventType)
{
    if (!setEventType(eventType)) return;
    m_taskScoped = false;
    m_taskId = 0;
    m_taskTitleQuery = taskTitleQuery.trimmed();
    m_hasQuery = true;
    startLoad(false);
}

void TaskHistoryController::loadOlder()
{
    if (!m_hasMore || m_loading || !m_hasQuery) return;
    startLoad(true);
}

void TaskHistoryController::setVisible(bool visible)
{
    if (m_visible == visible) return;
    m_visible = visible;
    if (!visible) {
        cancelCurrent();
        return;
    }
    if (m_ready && m_hasQuery) startLoad(false);
}

void TaskHistoryController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    const bool nowReady = state == application::ApplicationLifecycle::State::Running;
    if (m_ready != nowReady) {
        m_ready = nowReady;
        emit stateChanged();
    }
    if (!nowReady) {
        cancelCurrent();
        return;
    }
    if (m_visible && m_hasQuery) startLoad(false);
}

bool TaskHistoryController::setEventType(const QString &name)
{
    const QString normalized = name.trimmed();
    if (normalized.isEmpty()) {
        m_eventType.reset();
        return true;
    }
    const auto parsed = domain::taskHistoryEventTypeFromName(normalized);
    if (!parsed) {
        setError(QStringLiteral("Choose a supported task history event type."));
        return false;
    }
    m_eventType = *parsed;
    return true;
}

void TaskHistoryController::startLoad(bool append)
{
    if (!m_ready || !m_visible || !m_hasQuery) return;
    if (m_loading) {
        if (append) return;
        cancelCurrent();
    }
    if (!append) {
        m_cursor = {};
        m_hasMore = false;
        m_model.replace({});
        if (!m_warningMessage.isEmpty()) {
            m_warningMessage.clear();
            emit stateChanged();
        }
    }

    const auto operation = m_graph->beginOperation(
        QStringLiteral("load task history"), QStringLiteral("task-history-page"));
    if (!operation.isValid()) {
        setError(QStringLiteral("Task history is not available right now."));
        return;
    }
    m_operationId = operation.id.value;
    m_appendResult = append;
    m_loading = true;
    setError({});
    emit stateChanged();

    constexpr int pageSize = 50;
    const auto command = m_taskScoped
        ? services::TaskHistoryService::forTask(m_taskId, m_eventType, pageSize,
                                                append ? m_cursor : storage::TaskHistoryCursor{})
        : services::TaskHistoryService::recent(
              m_taskTitleQuery, m_eventType, pageSize,
              append ? m_cursor : storage::TaskHistoryCursor{});
    if (!m_graph->submitStorage(operation, command) && m_operationId == operation.id.value) {
        m_operationId = 0;
        m_loading = false;
        setError(QStringLiteral("Task history could not be loaded."));
        emit stateChanged();
    }
}

void TaskHistoryController::cancelCurrent()
{
    if (m_operationId == 0) return;
    const quint64 operationId = m_operationId;
    m_operationId = 0;
    m_loading = false;
    m_graph->cancelOperation({operationId});
    emit stateChanged();
}

void TaskHistoryController::onStorageFinished(quint64 operationId, const QVariantMap &values,
                                              domain::AppError error)
{
    if (operationId == 0 || operationId != m_operationId) return;
    const bool append = m_appendResult;
    m_operationId = 0;
    m_loading = false;
    if (error.isError()) {
        setError(error.userMessage());
        emit stateChanged();
        return;
    }
    if (error.isCancelled()) {
        emit stateChanged();
        return;
    }

    const QVariantList events = values.value(QStringLiteral("events")).toList();
    if (append) m_model.appendOlder(events);
    else m_model.replace(events);
    if (values.value(QStringLiteral("invalidEventsSkipped")).toInt() > 0) {
        m_warningMessage = QStringLiteral(
            "Some task history entries were skipped because their saved data could not be read.");
    }
    m_hasMore = values.value(QStringLiteral("hasMore")).toBool();
    m_cursor.occurredAtUtc = values.value(QStringLiteral("nextOccurredAtUtc")).toString();
    m_cursor.eventId = values.value(QStringLiteral("nextEventId")).toLongLong();
    setError({});
    emit stateChanged();
}

void TaskHistoryController::setError(QString message)
{
    if (m_errorMessage == message) return;
    m_errorMessage = std::move(message);
    emit stateChanged();
}

} // namespace something::controllers
