#pragma once

#include "../application/application_service_graph.h"
#include "../domain/automation_rules.h"
#include "../platform/desktop_applications_platform.h"
#include "../services/application_mutation_workflow.h"
#include "../services/application_usage_service.h"
#include "../services/bounded_worker_pool.h"
#include "../services/setups_service.h"

#include <QObject>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QPointer>
#include <QUrl>

#include <functional>

namespace something::controllers {
class BackgroundController;
}

namespace something::controllers {

class AutomationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)
    Q_PROPERTY(QVariantList rules READ rules NOTIFY stateChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY stateChanged)
    Q_PROPERTY(bool historyHasMore READ historyHasMore NOTIFY stateChanged)

public:
    AutomationController(application::ApplicationServiceGraph *graph,
        services::ApplicationMutationWorkflow *mutations,
        services::ApplicationUsageService *usage,
        platform::DesktopApplicationsPlatform *desktopPlatform,
        BackgroundController *background,
        QObject *parent = nullptr,
        std::function<bool(const QUrl &)> openUrl = {});

    bool ready() const noexcept { return m_ready; }
    bool paused() const noexcept { return m_paused; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }
    QVariantList rules() const;
    QVariantList history() const { return m_history; }
    bool historyHasMore() const noexcept { return m_historyHasMore; }

    Q_INVOKABLE void reload();
    Q_INVOKABLE void saveRule(QVariantMap rule, qint64 expectedRevision = 0);
    Q_INVOKABLE void deleteRule(const QString &ruleId);
    Q_INVOKABLE void duplicateRule(const QString &ruleId);
    Q_INVOKABLE void setRuleEnabled(const QString &ruleId, bool enabled);
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void loadHistory(const QString &ruleId = {}, const QString &status = {},
                                 const QString &triggerType = {}, int offset = 0,
                                 const QString &startDate = {}, const QString &endDate = {});
    Q_INVOKABLE void clearMessages();

    void onTaskCompletionCommitted(const QString &eventId, const QVariantMap &task,
                                   bool previousCompleted, bool currentCompleted);
    void onApplicationSessionStarted(const QString &applicationId, const QString &sessionId,
                                     const QString &timestampUtc);
    void onApplicationStateChanged(const QString &applicationId, const QString &sessionId,
                                   const QString &state, const QString &timestampUtc);
    void onSteamPriceRefreshCommitted(const QVariantMap &previous, const QVariantMap &current);
    void onSteamReleaseRefreshCommitted(const QVariantMap &previous, const QVariantMap &current);
    void onSteamNewsRefreshCommitted(quint32 appId, const QVariantList &newArticles,
                                     const QString &timestampUtc);

signals:
    void stateChanged();
    void ruleSaved(const QString &ruleId);
    void ruleDeleted(const QString &ruleId);
    void executionChanged(qint64 taskId, const QString &status,
                          const QString &errorMessage, const QString &timestampUtc);

private:
    struct PendingOperation final {
        application::OperationContext operation;
        std::function<void(const QVariantMap &, domain::AppError)> completed;
    };
    struct RunningExecution final {
        domain::AutomationRule rule;
        domain::AutomationEvent event;
        QString executionId;
        int nextAction{0};
        QString actionStartedAtUtc;
        bool stopping{false};
    };

    bool submitStorage(storage::StorageCommand command,
                       std::function<void(const QVariantMap &, domain::AppError)> completed,
                       QString purpose);
    bool submitValue(services::BoundedWorkerPool::ValueWork work,
                     std::function<void(const QVariantMap &, domain::AppError)> completed,
                     QString purpose);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onValueFinished(quint64 operationId, const QVariantMap &values,
                         domain::AppError error);
    void receiveEvent(domain::AutomationEvent event);
    void claimExecution(const domain::AutomationRule &rule,
                        const domain::AutomationEvent &event);
    void runNextAction(const QString &executionId);
    void executeCurrentAction(const QString &executionId);
    void recordAction(const QString &executionId, const domain::AutomationAction &action,
                     const QString &startedAt, QString status, QString errorMessage,
                     bool continueSequence);
    void finishExecution(const QString &executionId, QString status, QString errorMessage = {});
    void markBlocked(const domain::AutomationRule &rule, const domain::AutomationEvent &event,
                     QString reason);
    void scheduleTick();
    void checkOverdueTasks();
    void checkScheduledRules();
    static domain::AutomationRule ruleFromMap(const QVariantMap &map);
    static QVariantMap ruleToMap(const domain::AutomationRule &rule);
    static domain::AutomationEvent taskEvent(const QString &eventId,
        const QVariantMap &task, const QString &type, const QString &timestampUtc);
    static QString utcNow();

    application::ApplicationServiceGraph *m_graph{nullptr};
    QPointer<services::ApplicationMutationWorkflow> m_mutations;
    QPointer<services::ApplicationUsageService> m_usage;
    platform::DesktopApplicationsPlatform *m_desktopPlatform{nullptr};
    QPointer<BackgroundController> m_background;
    std::function<bool(const QUrl &)> m_openUrl;
    QHash<quint64, PendingOperation> m_operations;
    QHash<quint64, std::function<void(const QVariantMap &, domain::AppError)>> m_valueOperations;
    QHash<QString, domain::AutomationRule> m_rules;
    QHash<QString, RunningExecution> m_running;
    QHash<QString, QDateTime> m_lastAttemptedByRule;
    QHash<QString, QPair<int, QDateTime>> m_automatedLaunchDepthByIdentity;
    QHash<QString, int> m_chainDepthBySession;
    QQueue<QDateTime> m_executionRateWindow;
    QTimer m_tickTimer;
    QVariantList m_history;
    bool m_historyHasMore{false};
    QString m_errorMessage;
    QString m_notification;
    QString m_lastScheduleMinute;
    bool m_ready{false};
    bool m_paused{false};
    bool m_stopping{false};
    bool m_firstScheduleScan{true};
    bool m_overdueQueryPending{false};
    bool m_recoveryStarted{false};
    int m_overdueOffset{0};
    int m_claimsPending{0};
    quint64 m_historyRequestGeneration{0};
};

} // namespace something::controllers
