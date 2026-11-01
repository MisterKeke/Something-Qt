#include "task_automation_service.h"

#include "../controllers/background_controller.h"
#include "desktop_applications_service.h"
#include "setups_service.h"
#include "task_service.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <utility>

namespace something::services {
namespace {

storage::StorageReply failure(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

QString systemUtcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QVariantMap automationMap(const domain::TaskAutomation &automation)
{
    return {
        {QStringLiteral("id"), automation.id},
        {QStringLiteral("taskId"), automation.taskId},
        {QStringLiteral("configured"), true},
        {QStringLiteral("enabled"), automation.enabled},
        {QStringLiteral("triggerType"), automation.triggerType},
        {QStringLiteral("actionType"), automation.actionType},
        {QStringLiteral("targetId"), automation.targetId},
        {QStringLiteral("url"), automation.url},
        {QStringLiteral("notificationMessage"), automation.notificationMessage},
        {QStringLiteral("delayMs"), automation.delayMs},
        {QStringLiteral("revision"), automation.revision},
    };
}

QString automationError(domain::AppError error, const QString &actionType)
{
    if (error.isCancelled()) return QStringLiteral("Action canceled.");
    switch (error.category()) {
    case domain::ErrorCategory::NotFound:
        return actionType == QStringLiteral("launch_setup")
            ? QStringLiteral("Selected Setup is no longer available.")
            : QStringLiteral("Selected application is no longer available.");
    case domain::ErrorCategory::Validation:
        return actionType == QStringLiteral("launch_application")
            ? QStringLiteral("The saved application executable is missing or invalid. Select a replacement.")
            : actionType == QStringLiteral("launch_setup")
                ? QStringLiteral("The selected Setup has no launchable applications.")
                : QStringLiteral("The automation target is invalid.");
    case domain::ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("Application launching is unavailable on this device.");
    case domain::ErrorCategory::Platform:
        return QStringLiteral("The operating system did not accept the launch request.");
    default:
        return QStringLiteral("The automation action could not be completed.");
    }
}

} // namespace

TaskAutomationService::TaskAutomationService(
    application::ApplicationServiceGraph *graph,
    ApplicationMutationWorkflow *mutations,
    platform::DesktopApplicationsPlatform *desktopPlatform,
    controllers::BackgroundController *background,
    QObject *parent, TaskAutomationAdapters adapters, bool completionEventsEnabled)
    : QObject(parent)
    , m_graph(graph)
    , m_mutations(mutations)
    , m_desktopPlatform(desktopPlatform)
    , m_background(background)
    , m_adapters(std::move(adapters))
{
    Q_ASSERT(m_graph && m_mutations && m_desktopPlatform);
    if (!m_adapters.utcNow) m_adapters.utcNow = systemUtcNow;
    if (!m_adapters.schedule) {
        m_adapters.schedule = [this](int delayMs, std::function<void()> callback) {
            auto *timer = new QTimer(this);
            timer->setSingleShot(true);
            timer->setTimerType(Qt::PreciseTimer);
            QPointer<QTimer> timerGuard(timer);
            connect(timer, &QTimer::timeout, this,
                    [timerGuard, callback = std::move(callback)]() mutable {
                if (timerGuard) timerGuard->deleteLater();
                callback();
            });
            timer->start(qMax(0, delayMs));
            return [timerGuard] {
                if (!timerGuard) return;
                timerGuard->stop();
                timerGuard->deleteLater();
            };
        };
    }
    if (!m_adapters.openUrl)
        m_adapters.openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    if (!m_adapters.showNotification) {
        m_adapters.showNotification = [this](const QString &title, const QString &message) {
            return m_background && m_background->showAutomationNotification(title, message);
        };
    }
    if (completionEventsEnabled) {
        connect(m_mutations, &ApplicationMutationWorkflow::taskCompletionCommitted,
                this, &TaskAutomationService::onCompletionCommitted);
    }
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &TaskAutomationService::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &TaskAutomationService::onValueFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &TaskAutomationService::onLifecycleChanged);
}

TaskAutomationService::~TaskAutomationService()
{
    for (auto it = m_scheduled.begin(); it != m_scheduled.end(); ++it) {
        if (it->cancelTimer) it->cancelTimer();
    }
}

QString TaskAutomationService::nowUtc() const
{
    return m_adapters.utcNow();
}

void TaskAutomationService::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    if (state == application::ApplicationLifecycle::State::Running)
        beginStartupRecovery();
    else if (state == application::ApplicationLifecycle::State::ShuttingDown)
        prepareShutdown();
}

void TaskAutomationService::beginStartupRecovery()
{
    if (m_recoveryStarted || m_stopping) return;
    m_recoveryStarted = true;
    const QString timestamp = nowUtc();
    PendingOperation pending;
    pending.stage = Stage::StartupRecovery;
    submitStorage(std::move(pending), [timestamp](storage::StorageRepositories &repositories,
        const std::function<bool()> &cancelled) {
        if (cancelled()) return failure(domain::AppError::cancelled());
        const auto recovered = repositories.taskAutomations.cancelUnfinished(timestamp);
        if (!recovered.succeeded()) return failure(recovered.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("recoveredCount"), recovered.value);
        return reply;
    });
}

void TaskAutomationService::onCompletionCommitted(
    const QString &completionEventId, const QVariantMap &task,
    bool previousCompleted, bool currentCompleted)
{
    if (m_stopping || completionEventId.isEmpty() || previousCompleted
        || !currentCompleted || !task.value(QStringLiteral("completed")).toBool()) return;
    const QVariantMap configuredAutomation = task.value(QStringLiteral("automation")).toMap();
    if (!configuredAutomation.value(QStringLiteral("configured")).toBool()
        || !configuredAutomation.value(QStringLiteral("enabled")).toBool()) return;
    if (m_completionEventsInFlight.contains(completionEventId)) return;
    const qint64 taskId = task.value(QStringLiteral("id")).toLongLong();
    if (taskId <= 0) return;
    m_completionEventsInFlight.insert(completionEventId);

    QVariantMap snapshot = task;
    if (snapshot.value(QStringLiteral("completedAtUtc")).toString().isEmpty())
        snapshot.insert(QStringLiteral("completedAtUtc"), nowUtc());
    const QString executionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString requestedAtUtc = snapshot.value(QStringLiteral("completedAtUtc")).toString();
    PendingOperation pending;
    pending.stage = Stage::CompletionLookup;
    pending.taskId = taskId;
    pending.completionEventId = completionEventId;
    pending.executionId = executionId;
    pending.requestedAtUtc = requestedAtUtc;
    pending.taskSnapshot = snapshot;
    submitStorage(std::move(pending), completionLookup(taskId, completionEventId,
        executionId, requestedAtUtc, std::move(snapshot)));
}

void TaskAutomationService::observeTaskMutation(qint64 taskId, const QVariantMap &task,
                                                bool deleted)
{
    invalidateStaleForTask(taskId, task, deleted);
}

void TaskAutomationService::invalidateStaleForTask(qint64 taskId,
    const QVariantMap &task, bool deleted)
{
    const QVariantMap currentAutomation = task.value(QStringLiteral("automation")).toMap();
    for (auto operation = m_operations.cbegin(); operation != m_operations.cend(); ++operation) {
        if (operation->stage != Stage::CompletionLookup || operation->taskId != taskId)
            continue;
        const QVariantMap expectedAutomation =
            operation->taskSnapshot.value(QStringLiteral("automation")).toMap();
        const bool stale = deleted || !task.value(QStringLiteral("completed")).toBool()
            || !currentAutomation.value(QStringLiteral("configured")).toBool()
            || !currentAutomation.value(QStringLiteral("enabled")).toBool()
            || currentAutomation.value(QStringLiteral("revision")).toLongLong()
                != expectedAutomation.value(QStringLiteral("revision")).toLongLong();
        if (stale) m_invalidatedCompletionEvents.insert(operation->completionEventId);
    }

    for (auto it = m_scheduled.begin(); it != m_scheduled.end();) {
        if (it->taskId != taskId) {
            ++it;
            continue;
        }
        const QVariantMap pendingAutomation = it->automation;
        const bool stale = deleted || !task.value(QStringLiteral("completed")).toBool()
            || !currentAutomation.value(QStringLiteral("configured")).toBool()
            || !currentAutomation.value(QStringLiteral("enabled")).toBool()
            || currentAutomation.value(QStringLiteral("revision")).toLongLong()
                != pendingAutomation.value(QStringLiteral("revision")).toLongLong();
        if (!stale) {
            ++it;
            continue;
        }
        const QString executionId = it.key();
        if (it->cancelTimer) it->cancelTimer();
        it = m_scheduled.erase(it);
        if (!deleted) {
            finishExecution(taskId, executionId, QStringLiteral("canceled"),
                QStringLiteral("Task completion state or automation configuration changed before the delayed action ran."));
        }
    }
}

void TaskAutomationService::prepareShutdown()
{
    if (m_stopping) return;
    m_stopping = true;
    const QStringList ids = m_scheduled.keys();
    for (const QString &id : ids) {
        auto it = m_scheduled.find(id);
        if (it == m_scheduled.end()) continue;
        if (it->cancelTimer) it->cancelTimer();
        m_scheduled.erase(it);
    }
    // The graph rejects new operations immediately after this hook. Persisted
    // pending/running rows are marked canceled by startup recovery; delayed
    // actions are never resumed across an application restart.
}

bool TaskAutomationService::submitStorage(PendingOperation pending,
                                           storage::StorageCommand command)
{
    const QString purpose = pending.stage == Stage::CompletionLookup
        ? QStringLiteral("load task automation")
        : pending.stage == Stage::DelayedValidation
            ? QStringLiteral("validate delayed task automation")
            : pending.stage == Stage::SaveResult
                ? QStringLiteral("save task automation result")
                : QStringLiteral("recover task automation history");
    const auto operation = m_graph->beginOperation(purpose);
    if (!operation.isValid()) {
        if (pending.stage == Stage::CompletionLookup)
            m_completionEventsInFlight.remove(pending.completionEventId);
        return false;
    }
    pending.operation = operation;
    const quint64 id = operation.id.value;
    m_operations.insert(id, std::move(pending));
    if (m_graph->submitStorage(operation, std::move(command))) return true;
    if (m_operations.contains(id)) {
        const PendingOperation failedOperation = m_operations.take(id);
        if (failedOperation.stage == Stage::CompletionLookup)
            m_completionEventsInFlight.remove(failedOperation.completionEventId);
        if (failedOperation.stage == Stage::SaveResult)
            emit executionChanged(failedOperation.taskId, failedOperation.resultStatus,
                QStringLiteral("The action result could not be saved."),
                failedOperation.resultExecutedAtUtc);
    }
    return false;
}

bool TaskAutomationService::submitValue(PendingOperation pending,
                                         services::BoundedWorkerPool::ValueWork work)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("execute task automation action"));
    if (!operation.isValid()) {
        finishExecution(pending.taskId, pending.executionId, QStringLiteral("failed"),
                        QStringLiteral("The automation executor is unavailable."));
        return false;
    }
    pending.operation = operation;
    const quint64 id = operation.id.value;
    m_operations.insert(id, std::move(pending));
    if (m_graph->submitExpensiveValue(operation, std::move(work))) return true;
    if (m_operations.contains(id)) {
        const PendingOperation failedOperation = m_operations.take(id);
        finishExecution(failedOperation.taskId, failedOperation.executionId,
            QStringLiteral("failed"), QStringLiteral("The automation action could not be started."));
    }
    return false;
}

void TaskAutomationService::schedule(ScheduledAction action, int delayMs)
{
    if (m_stopping) {
        finishExecution(action.taskId, action.executionId, QStringLiteral("canceled"),
            QStringLiteral("Canceled at application shutdown; delayed actions are not resumed after restart."));
        return;
    }
    const QString id = action.executionId;
    m_scheduled.insert(id, std::move(action));
    const auto cancelTimer = m_adapters.schedule(qMax(0, delayMs), [this, id] {
        auto it = m_scheduled.find(id);
        if (it == m_scheduled.end()) return;
        ScheduledAction action = it.value();
        m_scheduled.erase(it);
        action.cancelTimer = {};
        execute(std::move(action));
    });
    const auto scheduled = m_scheduled.find(id);
    if (scheduled != m_scheduled.end()) scheduled->cancelTimer = cancelTimer;
    else if (cancelTimer) cancelTimer();
}

void TaskAutomationService::execute(ScheduledAction action)
{
    if (m_stopping) {
        finishExecution(action.taskId, action.executionId, QStringLiteral("canceled"),
            QStringLiteral("Canceled at application shutdown; delayed actions are not resumed after restart."));
        return;
    }
    PendingOperation pending;
    pending.stage = Stage::DelayedValidation;
    pending.taskId = action.taskId;
    pending.completionEventId = action.completionEventId;
    pending.executionId = action.executionId;
    pending.requestedAtUtc = action.requestedAtUtc;
    pending.taskSnapshot = action.taskSnapshot;
    pending.automation = action.automation;
    submitStorage(std::move(pending), delayedValidation(action, nowUtc()));
}

void TaskAutomationService::onStorageFinished(quint64 operationId,
    const QVariantMap &values, domain::AppError error)
{
    auto it = m_operations.find(operationId);
    if (it == m_operations.end()) return;
    PendingOperation pending = it.value();
    m_operations.erase(it);

    if (pending.stage == Stage::CompletionLookup) {
        m_completionEventsInFlight.remove(pending.completionEventId);
        const bool invalidated = m_invalidatedCompletionEvents.remove(
            pending.completionEventId) > 0;
        if (error.isError() || error.isCancelled()
            || !values.value(QStringLiteral("eligible")).toBool()) return;
        if (invalidated) {
            finishExecution(pending.taskId, pending.executionId,
                QStringLiteral("canceled"),
                QStringLiteral("Task completion state or automation configuration changed before the delayed action ran."));
            return;
        }
        ScheduledAction action;
        action.taskId = pending.taskId;
        action.completionEventId = pending.completionEventId;
        action.executionId = pending.executionId;
        action.requestedAtUtc = pending.requestedAtUtc;
        action.taskSnapshot = values.value(QStringLiteral("task")).toMap();
        action.automation = values.value(QStringLiteral("automation")).toMap();
        schedule(std::move(action), values.value(QStringLiteral("delayMs")).toInt());
        emit executionChanged(pending.taskId, QStringLiteral("pending"), {},
                              pending.requestedAtUtc);
        return;
    }

    if (pending.stage == Stage::StartupRecovery) {
        if (values.value(QStringLiteral("recoveredCount")).toInt() > 0) {
            emit executionChanged(0, QStringLiteral("canceled"),
                QStringLiteral("An action was interrupted by application exit and was not resumed."),
                nowUtc());
        }
        return;
    }
    if (pending.stage == Stage::SaveResult) {
        const QString finalError = error.isError()
            ? QStringLiteral("The action result could not be saved.") : pending.resultError;
        emit executionChanged(pending.taskId, pending.resultStatus, finalError,
                              pending.resultExecutedAtUtc);
        return;
    }

    if (error.isError() || error.isCancelled()) {
        finishExecution(pending.taskId, pending.executionId,
            error.isCancelled() ? QStringLiteral("canceled") : QStringLiteral("failed"),
            error.isCancelled() ? QStringLiteral("Action canceled.")
                                : humanError(error, pending.automation.value(QStringLiteral("actionType")).toString()));
        return;
    }
    if (!values.value(QStringLiteral("ready")).toBool()) {
        finishExecution(pending.taskId, pending.executionId,
            values.value(QStringLiteral("status"), QStringLiteral("canceled")).toString(),
            values.value(QStringLiteral("errorMessage")).toString());
        return;
    }

    emit executionChanged(pending.taskId, QStringLiteral("running"), {},
                          values.value(QStringLiteral("startedAtUtc")).toString());

    const QString actionType = pending.automation.value(QStringLiteral("actionType")).toString();
    if (actionType == QStringLiteral("launch_application")) {
        const QVariantMap record = values.value(QStringLiteral("application")).toMap();
        auto *native = m_desktopPlatform;
        PendingOperation launch = pending;
        launch.stage = Stage::LaunchApplication;
        submitValue(std::move(launch), [record, native](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            const domain::AppError launchError = DesktopApplicationsService::launchSavedRecord(
                record, native, token);
            result.values.insert(QStringLiteral("status"), launchError.isCancelled()
                ? QStringLiteral("canceled")
                : launchError.isError() ? QStringLiteral("failed") : QStringLiteral("succeeded"));
            result.values.insert(QStringLiteral("errorMessage"), launchError.isError() || launchError.isCancelled()
                ? humanError(launchError, QStringLiteral("launch_application")) : QString());
            result.error = launchError;
            return result;
        });
        return;
    }
    if (actionType == QStringLiteral("launch_setup")) {
        const QVariantList applications = values.value(QStringLiteral("applications")).toList();
        auto *native = m_desktopPlatform;
        PendingOperation launch = pending;
        launch.stage = Stage::LaunchSetup;
        submitValue(std::move(launch), [applications, native](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            application::CancellationSource userCancellation;
            const auto summary = SetupsService::launchMembers(applications, token,
                userCancellation, [native](const QVariantMap &record,
                    const application::CancellationToken &appToken,
                    const application::CancellationToken &userToken) {
                    return DesktopApplicationsService::launchSavedRecord(
                        record, native, appToken, &userToken);
                });
            const bool succeeded = !summary.cancelled
                && summary.attempted > 0 && summary.launched == summary.attempted;
            QString detail;
            if (summary.cancelled) detail = QStringLiteral("Setup launch canceled.");
            else if (!succeeded) {
                detail = QStringLiteral("Setup launched %1 of %2 applications.")
                    .arg(summary.launched).arg(summary.attempted);
                if (!summary.failures.isEmpty())
                    detail += QStringLiteral(" ") + summary.failures.front().message;
            }
            result.values.insert(QStringLiteral("status"), summary.cancelled
                ? QStringLiteral("canceled") : succeeded ? QStringLiteral("succeeded")
                                                           : QStringLiteral("failed"));
            result.values.insert(QStringLiteral("errorMessage"), detail);
            result.values.insert(QStringLiteral("summary"), summary.toVariantMap());
            if (summary.cancelled) result.error = domain::AppError::cancelled();
            else if (!succeeded) result.error = domain::AppError(domain::ErrorCategory::Platform);
            return result;
        });
        return;
    }
    if (actionType == QStringLiteral("open_url")) {
        const QUrl url(pending.automation.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
        const bool accepted = url.isValid() && m_adapters.openUrl(url);
        finishExecution(pending.taskId, pending.executionId,
            accepted ? QStringLiteral("succeeded") : QStringLiteral("failed"),
            accepted ? QString() : QStringLiteral("The operating system could not open this URL."));
        return;
    }
    if (actionType == QStringLiteral("show_notification")) {
        const QString message = expandMessage(
            pending.automation.value(QStringLiteral("notificationMessage")).toString(),
            pending.taskSnapshot);
        const bool shown = m_adapters.showNotification(QStringLiteral("Task completed"), message);
        finishExecution(pending.taskId, pending.executionId,
            shown ? QStringLiteral("succeeded") : QStringLiteral("failed"),
            shown ? QString() : QStringLiteral("The notification could not be displayed."));
        return;
    }
    finishExecution(pending.taskId, pending.executionId, QStringLiteral("failed"),
                    QStringLiteral("Unsupported automation action."));
}

void TaskAutomationService::onValueFinished(quint64 operationId,
    const QVariantMap &values, domain::AppError error)
{
    auto it = m_operations.find(operationId);
    if (it == m_operations.end()) return;
    const PendingOperation pending = it.value();
    m_operations.erase(it);
    QString status = values.value(QStringLiteral("status")).toString();
    QString errorMessage = values.value(QStringLiteral("errorMessage")).toString();
    if (error.isCancelled()) {
        status = QStringLiteral("canceled");
        if (errorMessage.isEmpty()) errorMessage = QStringLiteral("Action canceled.");
    } else if (error.isError() && status.isEmpty()) {
        status = QStringLiteral("failed");
        errorMessage = humanError(error,
            pending.automation.value(QStringLiteral("actionType")).toString());
    }
    if (status.isEmpty()) status = QStringLiteral("failed");
    finishExecution(pending.taskId, pending.executionId, std::move(status),
                    std::move(errorMessage));
}

void TaskAutomationService::finishExecution(qint64 taskId, const QString &executionId,
    QString status, QString errorMessage)
{
    const QString executedAtUtc = nowUtc();
    PendingOperation pending;
    pending.stage = Stage::SaveResult;
    pending.taskId = taskId;
    pending.executionId = executionId;
    pending.resultStatus = status;
    pending.resultError = errorMessage;
    pending.resultExecutedAtUtc = executedAtUtc;
    submitStorage(std::move(pending), saveResult(executionId, std::move(status),
        std::move(errorMessage), executedAtUtc, taskId));
}

QString TaskAutomationService::targetReference(const QVariantMap &automation)
{
    const QString type = automation.value(QStringLiteral("actionType")).toString();
    if (type == QStringLiteral("launch_application"))
        return QStringLiteral("application:%1").arg(automation.value(QStringLiteral("targetId")).toLongLong());
    if (type == QStringLiteral("launch_setup"))
        return QStringLiteral("setup:%1").arg(automation.value(QStringLiteral("targetId")).toLongLong());
    if (type == QStringLiteral("open_url")) return automation.value(QStringLiteral("url")).toString();
    return QStringLiteral("notification");
}

QString TaskAutomationService::humanError(domain::AppError error, const QString &actionType)
{
    return automationError(error, actionType);
}

QString TaskAutomationService::expandMessage(QString message, const QVariantMap &task)
{
    message.replace(QStringLiteral("{task.title}"), task.value(QStringLiteral("title")).toString());
    message.replace(QStringLiteral("{task.id}"), task.value(QStringLiteral("id")).toString());
    message.replace(QStringLiteral("{task.completedAt}"),
        task.value(QStringLiteral("completedAtUtc")).toString());
    return message;
}

storage::StorageCommand TaskAutomationService::completionLookup(
    qint64 taskId, QString completionEventId, QString executionId,
    QString requestedAtUtc, QVariantMap taskSnapshot)
{
    return [taskId, completionEventId = std::move(completionEventId),
            executionId = std::move(executionId), requestedAtUtc = std::move(requestedAtUtc),
            taskSnapshot = std::move(taskSnapshot)](
        storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failure(domain::AppError::cancelled());
        const auto task = repositories.tasks.get(taskId);
        if (!task.succeeded()) return failure(task.error);
        const QVariantMap expectedAutomation = taskSnapshot.value(QStringLiteral("automation")).toMap();
        if (!task.value.completed || !task.value.automation || !task.value.automation->enabled
            || !expectedAutomation.value(QStringLiteral("configured")).toBool()
            || !expectedAutomation.value(QStringLiteral("enabled")).toBool()
            || task.value.automation->id != expectedAutomation.value(QStringLiteral("id")).toLongLong()
            || task.value.automation->revision
                != expectedAutomation.value(QStringLiteral("revision")).toLongLong()) {
            storage::StorageReply reply;
            reply.values.insert(QStringLiteral("eligible"), false);
            return reply;
        }
        const domain::TaskAutomation &automation = *task.value.automation;
        storage::TaskAutomationExecution execution;
        execution.executionId = executionId;
        execution.automationId = automation.id;
        execution.taskId = taskId;
        execution.completionEventId = completionEventId;
        execution.actionType = automation.actionType;
        execution.targetReference = targetReference(automationMap(automation));
        execution.requestedAtUtc = requestedAtUtc;
        execution.status = QStringLiteral("pending");
        execution.configurationRevision = automation.revision;
        const auto inserted = repositories.taskAutomations.insertExecution(execution);
        if (!inserted.succeeded()) return failure(inserted.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("eligible"), inserted.value);
        if (!inserted.value) return reply;
        taskSnapshot.insert(QStringLiteral("automation"), automationMap(automation));
        reply.values.insert(QStringLiteral("task"), taskSnapshot);
        reply.values.insert(QStringLiteral("automation"), automationMap(automation));
        reply.values.insert(QStringLiteral("delayMs"), automation.delayMs);
        return reply;
    };
}

storage::StorageCommand TaskAutomationService::delayedValidation(
    const ScheduledAction &action, QString startedAtUtc)
{
    const qint64 taskId = action.taskId;
    const QString executionId = action.executionId;
    const QVariantMap expectedAutomation = action.automation;
    return [taskId, executionId, expectedAutomation,
            startedAtUtc = std::move(startedAtUtc)](
        storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return failure(domain::AppError::cancelled());
        const auto task = repositories.tasks.get(taskId);
        if (!task.succeeded()) {
            storage::StorageReply reply;
            reply.values.insert(QStringLiteral("ready"), false);
            reply.values.insert(QStringLiteral("status"), QStringLiteral("canceled"));
            reply.values.insert(QStringLiteral("errorMessage"),
                QStringLiteral("Task was deleted before the delayed action ran."));
            return reply;
        }
        if (!task.value.completed || !task.value.automation
            || !task.value.automation->enabled
            || task.value.automation->id != expectedAutomation.value(QStringLiteral("id")).toLongLong()
            || task.value.automation->revision
                != expectedAutomation.value(QStringLiteral("revision")).toLongLong()) {
            const auto canceled = repositories.taskAutomations.updateExecution(executionId,
                QStringLiteral("canceled"), startedAtUtc,
                QStringLiteral("Task completion state or automation configuration changed before the delayed action ran."));
            if (!canceled.succeeded()) return failure(canceled.error);
            storage::StorageReply reply;
            reply.values.insert(QStringLiteral("ready"), false);
            reply.values.insert(QStringLiteral("status"), QStringLiteral("canceled"));
            reply.values.insert(QStringLiteral("errorMessage"),
                QStringLiteral("Task completion state or automation configuration changed before the delayed action ran."));
            return reply;
        }

        const QString actionType = task.value.automation->actionType;
        const auto running = repositories.taskAutomations.updateExecution(executionId,
            QStringLiteral("running"), startedAtUtc, {});
        if (!running.succeeded()) return failure(running.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("ready"), true);
        reply.values.insert(QStringLiteral("automation"), automationMap(*task.value.automation));
        reply.values.insert(QStringLiteral("startedAtUtc"), startedAtUtc);
        if (actionType == QStringLiteral("launch_application")) {
            const auto application = repositories.applications.get(task.value.automation->targetId);
            if (!application.succeeded()) {
                const QString message = application.error.category() == domain::ErrorCategory::NotFound
                    ? QStringLiteral("Selected application is no longer available.")
                    : QStringLiteral("The saved application could not be loaded.");
                const auto updated = repositories.taskAutomations.updateExecution(executionId,
                    QStringLiteral("failed"), startedAtUtc, message);
                if (!updated.succeeded()) return failure(updated.error);
                reply.values.insert(QStringLiteral("ready"), false);
                reply.values.insert(QStringLiteral("status"), QStringLiteral("failed"));
                reply.values.insert(QStringLiteral("errorMessage"), message);
                return reply;
            }
            reply.values.insert(QStringLiteral("application"),
                DesktopApplicationsService::toVariantMap(application.value));
        } else if (actionType == QStringLiteral("launch_setup")) {
            auto launchData = SetupsService::launchData(task.value.automation->targetId);
            const auto data = launchData(repositories, cancelled);
            if (data.error.isError() || data.error.isCancelled()) {
                const QString message = data.error.category() == domain::ErrorCategory::NotFound
                    ? QStringLiteral("Selected Setup is no longer available.")
                    : data.error.isCancelled() ? QStringLiteral("Action canceled.")
                        : QStringLiteral("The selected Setup has no launchable applications.");
                const QString status = data.error.isCancelled()
                    ? QStringLiteral("canceled") : QStringLiteral("failed");
                const auto updated = repositories.taskAutomations.updateExecution(executionId,
                    status, startedAtUtc, message);
                if (!updated.succeeded()) return failure(updated.error);
                reply.values.insert(QStringLiteral("ready"), false);
                reply.values.insert(QStringLiteral("status"), status);
                reply.values.insert(QStringLiteral("errorMessage"), message);
                return reply;
            }
            reply.values.insert(QStringLiteral("applications"), data.values.value(QStringLiteral("applications")));
        }
        return reply;
    };
}

storage::StorageCommand TaskAutomationService::saveResult(QString executionId,
    QString status, QString errorMessage, QString executedAtUtc, qint64 taskId)
{
    return [executionId = std::move(executionId), status = std::move(status),
            errorMessage = std::move(errorMessage), executedAtUtc = std::move(executedAtUtc),
            taskId](storage::StorageRepositories &repositories,
                    const std::function<bool()> &cancelled) {
        if (cancelled()) return failure(domain::AppError::cancelled());
        const auto updated = repositories.taskAutomations.updateExecution(
            executionId, status, executedAtUtc, errorMessage);
        if (!updated.succeeded()) return failure(updated.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("taskId"), taskId);
        return reply;
    };
}

} // namespace something::services
