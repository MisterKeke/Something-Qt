#pragma once

#include "../application/application_service_graph.h"
#include "../platform/desktop_applications_platform.h"
#include "application_mutation_workflow.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

#include <functional>

namespace something::controllers {
class BackgroundController;
}

namespace something::services {

struct TaskAutomationAdapters final {
    using CancelScheduled = std::function<void()>;
    using Schedule = std::function<CancelScheduled(int, std::function<void()>)>;

    std::function<QString()> utcNow;
    Schedule schedule;
    std::function<bool(const QUrl &)> openUrl;
    std::function<bool(const QString &, const QString &)> showNotification;
};

// Executes the four explicitly supported task actions. Configuration and
// execution history stay in SQLite; timers and OS integrations stay on the
// application side of the storage boundary.
class TaskAutomationService final : public QObject {
    Q_OBJECT
public:
    TaskAutomationService(application::ApplicationServiceGraph *graph,
                          ApplicationMutationWorkflow *mutations,
                          platform::DesktopApplicationsPlatform *desktopPlatform,
                          controllers::BackgroundController *background,
                          QObject *parent = nullptr,
                          TaskAutomationAdapters adapters = {});
    ~TaskAutomationService() override;

    void prepareShutdown();
    void observeTaskMutation(qint64 taskId, const QVariantMap &task, bool deleted);

signals:
    void executionChanged(qint64 taskId, const QString &status,
                          const QString &errorMessage, const QString &executedAtUtc);

private:
    enum class Stage { CompletionLookup, DelayedValidation, SaveResult, StartupRecovery,
                       LaunchApplication, LaunchSetup };
    struct PendingOperation final {
        Stage stage{Stage::CompletionLookup};
        application::OperationContext operation;
        qint64 taskId{0};
        QString completionEventId;
        QString executionId;
        QString requestedAtUtc;
        QString resultStatus;
        QString resultError;
        QString resultExecutedAtUtc;
        QVariantMap taskSnapshot;
        QVariantMap automation;
    };
    struct ScheduledAction final {
        qint64 taskId{0};
        QString completionEventId;
        QString executionId;
        QString requestedAtUtc;
        QVariantMap taskSnapshot;
        QVariantMap automation;
        TaskAutomationAdapters::CancelScheduled cancelTimer;
    };

    void onCompletionCommitted(const QString &completionEventId,
                               const QVariantMap &task, bool previousCompleted,
                               bool currentCompleted);
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onValueFinished(quint64 operationId, const QVariantMap &values,
                         domain::AppError error);
    bool submitStorage(PendingOperation pending, storage::StorageCommand command);
    bool submitValue(PendingOperation pending, services::BoundedWorkerPool::ValueWork work);
    void schedule(ScheduledAction action, int delayMs);
    void execute(ScheduledAction action);
    void finishExecution(qint64 taskId, const QString &executionId,
                         QString status, QString errorMessage);
    void invalidateStaleForTask(qint64 taskId, const QVariantMap &task,
                                bool deleted);
    void beginStartupRecovery();
    QString nowUtc() const;
    static QString targetReference(const QVariantMap &automation);
    static QString humanError(domain::AppError error, const QString &actionType);
    static QString expandMessage(QString message, const QVariantMap &task);
    static storage::StorageCommand completionLookup(qint64 taskId,
        QString completionEventId, QString executionId, QString requestedAtUtc,
        QVariantMap taskSnapshot);
    static storage::StorageCommand delayedValidation(const ScheduledAction &action,
                                                       QString startedAtUtc);
    static storage::StorageCommand saveResult(QString executionId, QString status,
        QString errorMessage, QString executedAtUtc, qint64 taskId);

    application::ApplicationServiceGraph *m_graph{nullptr};
    ApplicationMutationWorkflow *m_mutations{nullptr};
    platform::DesktopApplicationsPlatform *m_desktopPlatform{nullptr};
    QPointer<controllers::BackgroundController> m_background;
    TaskAutomationAdapters m_adapters;
    QHash<quint64, PendingOperation> m_operations;
    QHash<QString, ScheduledAction> m_scheduled;
    QSet<QString> m_completionEventsInFlight;
    QSet<QString> m_invalidatedCompletionEvents;
    bool m_recoveryStarted{false};
    bool m_stopping{false};
};

} // namespace something::services
