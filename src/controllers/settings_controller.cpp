#include "settings_controller.h"

#include "../services/preferences_service.h"

#include <QThread>

#include <utility>

namespace something::controllers {

SettingsController::SettingsController(application::ApplicationServiceGraph *graph,
                                       const domain::Clock *clock,
                                       QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_clock(clock)
{
    Q_ASSERT(m_graph && m_clock);
    Q_ASSERT(QThread::currentThread() == thread());
    m_localTimeZone = QString::fromUtf8(m_clock->localTimeZone().id());
    if (m_localTimeZone.isEmpty())
        m_localTimeZone = QStringLiteral("System local time");

    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &SettingsController::onStorageFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &SettingsController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

void SettingsController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    const bool ready = state == application::ApplicationLifecycle::State::Running;
    const bool becameReady = !m_ready && ready;
    m_ready = ready;
    if (ready && (becameReady || !m_loaded)) {
        reload();
    } else {
        if (!ready) cancelPending();
        if (state == application::ApplicationLifecycle::State::Failed)
            m_errorMessage = m_graph->lifecycle().startupError().userMessage();
    }
    emit stateChanged();
}

void SettingsController::reload()
{
    if (!m_ready || m_saving) return;
    if (m_loading) {
        const quint64 previous = m_operationId;
        m_pending.remove(previous);
        m_operationId = 0;
        m_graph->cancelOperation({previous});
        m_loading = false;
    }
    m_errorMessage.clear();
    if (!submit(Action::LoadWeekStart, services::PreferencesService::loadWeekStart())) {
        emit stateChanged();
        return;
    }
    m_loading = true;
    emit stateChanged();
}

void SettingsController::setWeekStart(int sundayZero)
{
    if (!m_ready || !m_loaded || m_saving || sundayZero < 0 || sundayZero > 6) {
        if (sundayZero < 0 || sundayZero > 6) {
            m_errorMessage = QStringLiteral("Choose a valid first day of the week.");
            emit stateChanged();
        }
        return;
    }
    if (sundayZero == m_weekStart) return;
    m_errorMessage.clear();
    if (!submit(Action::SaveWeekStart,
                services::PreferencesService::setWeekStart(sundayZero))) {
        emit stateChanged();
        return;
    }
    m_saving = true;
    emit stateChanged();
}

bool SettingsController::submit(Action action, storage::StorageCommand command)
{
    const auto operation = m_graph->beginOperation(
        action == Action::LoadWeekStart
            ? QStringLiteral("load retained settings")
            : QStringLiteral("save task week-start preference"));
    if (!operation.isValid()) {
        m_errorMessage = QStringLiteral("Settings are not available right now.");
        return false;
    }
    m_operationId = operation.id.value;
    m_pending.insert(m_operationId, action);
    if (m_graph->submitStorage(operation, std::move(command))) return true;
    m_pending.remove(m_operationId);
    m_operationId = 0;
    m_graph->completeOperation(operation.id,
                               domain::AppError(domain::ErrorCategory::Busy));
    m_errorMessage = QStringLiteral("Settings could not be saved. Try again.");
    return false;
}

void SettingsController::onStorageFinished(quint64 operationId,
                                           const QVariantMap &values,
                                           domain::AppError error)
{
    const auto it = m_pending.find(operationId);
    if (it == m_pending.end()) return;
    const Action action = it.value();
    m_pending.erase(it);
    if (m_operationId == operationId) m_operationId = 0;
    if (action == Action::LoadWeekStart) m_loading = false;
    else m_saving = false;

    if (error.isError()) {
        m_errorMessage = error.userMessage();
        if (action == Action::LoadWeekStart) m_loaded = true;
        emit stateChanged();
        return;
    }
    if (error.isCancelled()) {
        emit stateChanged();
        return;
    }

    const int value = values.value(QStringLiteral("weekStart"), 1).toInt();
    if (value < 0 || value > 6) {
        m_errorMessage = QStringLiteral("Saved week-start setting is invalid.");
        if (action == Action::LoadWeekStart) m_loaded = true;
        emit stateChanged();
        return;
    }

    m_errorMessage.clear();
    m_loaded = true;
    if (m_weekStart != value) {
        m_weekStart = value;
        emit weekStartChanged(value);
    }
    emit stateChanged();
}

void SettingsController::cancelPending()
{
    const auto pending = m_pending.keys();
    m_pending.clear();
    m_operationId = 0;
    m_loading = false;
    m_saving = false;
    for (quint64 id : pending)
        m_graph->cancelOperation({id});
}

} // namespace something::controllers
