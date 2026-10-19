#pragma once

#include "../domain/app_error.h"

#include <QObject>

namespace something::application {

class ApplicationLifecycle final : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)

public:
    enum class State {
        Starting,
        Running,
        Failed,
        ShuttingDown,
        Stopped,
    };
    Q_ENUM(State)

    explicit ApplicationLifecycle(QObject *parent = nullptr);

    State state() const noexcept { return m_state; }
    something::domain::AppError startupError() const noexcept { return m_startupError; }
    QString stateName() const;

    bool markRunning();
    bool failStartup(something::domain::AppError error);
    bool beginShutdown();
    bool markStopped();

signals:
    void stateChanged(something::application::ApplicationLifecycle::State state);
    void startupErrorChanged();

private:
    void setState(State state);

    State m_state{State::Starting};
    something::domain::AppError m_startupError;
};

} // namespace something::application
