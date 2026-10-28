#pragma once

#include "../application/application_service_graph.h"
#include "../storage/storage_worker.h"

#include <QObject>
#include <QMetaType>
#include <QHash>

namespace something::services {

struct MutationResult final {
    QString requestId;
    QString kind;
    QString state; // succeeded, failed, cancelled
    QString errorCode;
    QString message;
    QVariantMap data;
};

struct MutationAdmission final {
    bool accepted{false};
    QString errorCode;
    QString message;
};

// Shared application workflow for the limited task and retained-setting
// mutations exposed by both GUI controllers and the embedded console.
class ApplicationMutationWorkflow final : public QObject {
    Q_OBJECT
public:
    static constexpr int MaximumPendingRequests = 8;
    explicit ApplicationMutationWorkflow(application::ApplicationServiceGraph *graph,
                                          QObject *parent = nullptr);
    MutationAdmission addTask(QString requestId, storage::TaskInput input,
                              std::function<void()> admitted = {});
    MutationAdmission completeTask(QString requestId, qint64 id, qint64 revision, bool completed,
                                   std::function<void()> admitted = {});
    MutationAdmission setWeekStart(QString requestId, int sundayZero,
                                   std::function<void()> admitted = {});
    bool cancel(const QString &requestId);

signals:
    void requestFinished(const something::services::MutationResult &result);
    void taskCommitted(const QString &requestId, const QVariantMap &task, bool created);
    void weekStartCommitted(const QString &requestId, int sundayZero);

private:
    struct Pending final { QString kind; quint64 operationId{0}; bool final{false}; };
    MutationAdmission dispatch(QString requestId, QString kind,
                               storage::StorageCommand command,
                               std::function<void()> admitted);
    void finish(QString requestId, QString state, domain::AppError error,
                const QVariantMap &values = {});
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onOperationFinished(quint64 operationId,
                             application::OperationCoordinator::State state,
                             domain::AppError error);
    application::ApplicationServiceGraph *m_graph{nullptr};
    QHash<QString, Pending> m_pending;
    QHash<quint64, QString> m_byOperation;
};

} // namespace something::services

Q_DECLARE_METATYPE(something::services::MutationResult)
