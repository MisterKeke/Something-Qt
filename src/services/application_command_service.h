#pragma once

#include "../application/application_service_graph.h"
#include "../application/command_registry.h"
#include "../application/command_parser.h"
#include "application_mutation_workflow.h"
#include "../domain/clock.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <functional>

namespace something::services {

struct ConsoleRequestResult final {
    QString requestId;
    bool accepted{false};
    QString state; // rejected, pending, succeeded, failed, cancelled
    QString command;
    QString message;
    QVariantMap data;
};

// Application-specific command dispatch. It never invokes arbitrary QObject
// methods and does not expose a process, shell, script, or executable API.
class ApplicationCommandService final : public QObject {
    Q_OBJECT
public:
    static constexpr int MaximumPendingRequests = ApplicationMutationWorkflow::MaximumPendingRequests;

    explicit ApplicationCommandService(application::ApplicationServiceGraph *graph,
                                        QObject *parent = nullptr);
    void setClock(const domain::Clock *clock) noexcept { m_clock = clock; }
    void setSnapshotProvider(QString name, std::function<QVariantMap()> provider);
    void setMutationWorkflow(ApplicationMutationWorkflow *workflow);
    const application::CommandRegistry &registry() const noexcept { return m_registry; }
    QString submit(const QString &input);
    bool cancel(const QString &requestId);

signals:
    void requestAccepted(const something::services::ConsoleRequestResult &result);
    void requestRejected(const something::services::ConsoleRequestResult &result);
    void requestPending(const something::services::ConsoleRequestResult &result);
    void requestFinished(const something::services::ConsoleRequestResult &result);
    void requestCancelled(const something::services::ConsoleRequestResult &result);
    void routeRequested(const QString &routeId);

private:
    struct PendingRequest final {
        ConsoleRequestResult result;
        quint64 operationId{0};
        bool finalEmitted{false};
    };

    void registerBuiltins();
    void registerPhaseThreeCommands();
    void onMutationFinished(const something::services::MutationResult &result);
    ConsoleRequestResult makeResult(QString id, QString state, QString command = {}) const;
    void finish(const QString &id, QString state, QString message = {}, QVariantMap data = {});
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onOperationFinished(quint64 operationId,
                             application::OperationCoordinator::State state,
                             domain::AppError error);
    QString availabilityError() const;
    QString queryAvailabilityError() const;

    application::ApplicationServiceGraph *m_graph{nullptr};
    application::CommandRegistry m_registry;
    const domain::Clock *m_clock{nullptr};
    QHash<QString, std::function<QVariantMap()>> m_snapshotProviders;
    QHash<QString, PendingRequest> m_pending;
    QHash<quint64, QString> m_requestByOperation;
    ApplicationMutationWorkflow *m_mutations{nullptr};
    QSet<QString> m_mutationRequests;
    QHash<QString, QString> m_mutationCommands;
};

} // namespace something::services

Q_DECLARE_METATYPE(something::services::ConsoleRequestResult)
