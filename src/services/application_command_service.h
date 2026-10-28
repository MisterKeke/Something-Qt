#pragma once

#include "../application/application_service_graph.h"
#include "../application/command_registry.h"
#include "../application/command_parser.h"

#include <QObject>
#include <QHash>

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
    static constexpr int MaximumPendingRequests = 8;

    explicit ApplicationCommandService(application::ApplicationServiceGraph *graph,
                                        QObject *parent = nullptr);
    const application::CommandRegistry &registry() const noexcept { return m_registry; }
    QString submit(const QString &input);
    bool cancel(const QString &requestId);

signals:
    void requestAccepted(const something::services::ConsoleRequestResult &result);
    void requestRejected(const something::services::ConsoleRequestResult &result);
    void requestPending(const something::services::ConsoleRequestResult &result);
    void requestFinished(const something::services::ConsoleRequestResult &result);
    void requestCancelled(const something::services::ConsoleRequestResult &result);

private:
    struct PendingRequest final {
        ConsoleRequestResult result;
        quint64 operationId{0};
        bool finalEmitted{false};
    };

    void registerBuiltins();
    ConsoleRequestResult makeResult(QString id, QString state, QString command = {}) const;
    void finish(const QString &id, QString state, QString message = {}, QVariantMap data = {});
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onOperationFinished(quint64 operationId,
                             application::OperationCoordinator::State state,
                             domain::AppError error);
    QString availabilityError() const;

    application::ApplicationServiceGraph *m_graph{nullptr};
    application::CommandRegistry m_registry;
    QHash<QString, PendingRequest> m_pending;
    QHash<quint64, QString> m_requestByOperation;
};

} // namespace something::services

Q_DECLARE_METATYPE(something::services::ConsoleRequestResult)
