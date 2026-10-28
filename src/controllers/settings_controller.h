#pragma once

#include "../application/application_service_graph.h"
#include "../domain/clock.h"
#include "../services/application_mutation_workflow.h"

#include <QHash>
#include <QObject>

namespace something::controllers {

class SettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY stateChanged)
    Q_PROPERTY(int weekStart READ weekStart NOTIFY stateChanged)
    Q_PROPERTY(QString localTimeZone READ localTimeZone CONSTANT)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)

public:
    SettingsController(application::ApplicationServiceGraph *graph,
                       const domain::Clock *clock,
                       services::ApplicationMutationWorkflow *mutations,
                       QObject *parent = nullptr);

    bool ready() const noexcept { return m_ready; }
    bool loaded() const noexcept { return m_loaded; }
    bool loading() const noexcept { return m_loading; }
    bool saving() const noexcept { return m_saving; }
    int weekStart() const noexcept { return m_weekStart; }
    QString localTimeZone() const { return m_localTimeZone; }
    QString errorMessage() const { return m_errorMessage; }

    Q_INVOKABLE void setWeekStart(int sundayZero);
    Q_INVOKABLE void reload();

signals:
    void stateChanged();
    void weekStartChanged(int sundayZero);

private:
    enum class Action { LoadWeekStart };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onSharedMutationFinished(const services::MutationResult &result);
    void onSharedWeekStartCommitted(const QString &requestId, int sundayZero);
    void cancelPending();
    bool submit(Action action, storage::StorageCommand command);

    application::ApplicationServiceGraph *m_graph{nullptr};
    const domain::Clock *m_clock{nullptr};
    services::ApplicationMutationWorkflow *m_mutations{nullptr};
    QHash<quint64, Action> m_pending;
    QHash<QString, bool> m_sharedPending;
    quint64 m_operationId{0};
    int m_weekStart{1};
    QString m_localTimeZone;
    QString m_errorMessage;
    bool m_ready{false};
    bool m_loaded{false};
    bool m_loading{false};
    bool m_saving{false};
};

} // namespace something::controllers
