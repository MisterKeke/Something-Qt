#include "application_lifecycle.h"

namespace something::application {

ApplicationLifecycle::ApplicationLifecycle(QObject *parent)
    : QObject(parent)
{
}

QString ApplicationLifecycle::stateName() const
{
    switch (m_state) {
    case State::Starting:
        return QStringLiteral("starting");
    case State::Running:
        return QStringLiteral("running");
    case State::Failed:
        return QStringLiteral("failed");
    case State::ShuttingDown:
        return QStringLiteral("shutting-down");
    case State::Stopped:
        return QStringLiteral("stopped");
    }
    return QStringLiteral("failed");
}

bool ApplicationLifecycle::markRunning()
{
    if (m_state != State::Starting) {
        return false;
    }
    setState(State::Running);
    return true;
}

bool ApplicationLifecycle::failStartup(something::domain::AppError error)
{
    if (m_state != State::Starting || !error.isError()) {
        return false;
    }
    m_startupError = error;
    emit startupErrorChanged();
    setState(State::Failed);
    return true;
}

bool ApplicationLifecycle::beginShutdown()
{
    if (m_state == State::ShuttingDown || m_state == State::Stopped) {
        return false;
    }
    setState(State::ShuttingDown);
    return true;
}

bool ApplicationLifecycle::markStopped()
{
    if (m_state != State::ShuttingDown) {
        return false;
    }
    setState(State::Stopped);
    return true;
}

void ApplicationLifecycle::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(m_state);
}

} // namespace something::application
