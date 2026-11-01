#include "automation_controller.h"

#include "../application/operation_coordinator.h"
#include "../domain/task_date_rules.h"
#include "../services/desktop_applications_service.h"
#include "../services/setups_service.h"
#include "../services/task_service.h"
#include "../storage/repositories.h"
#include "background_controller.h"

#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <algorithm>

namespace something::controllers {
namespace {
using namespace something;

QVariantMap appErrorValues(domain::AppError error, const QString &fallback)
{
    const QString message = error.isCancelled() ? QStringLiteral("Action canceled.")
        : error.isError() ? (error.userMessage().isEmpty() ? fallback : error.userMessage())
                          : QString();
    return {{QStringLiteral("errorMessage"), message},
            {QStringLiteral("status"), error.isCancelled() ? QStringLiteral("canceled")
                : error.isError() ? QStringLiteral("failed") : QStringLiteral("succeeded")}};
}

QString safeTriggerTitle(const QString &trigger)
{
    if (trigger.startsWith(QStringLiteral("task."))) return QStringLiteral("Task");
    if (trigger.startsWith(QStringLiteral("application."))) return QStringLiteral("Application");
    if (trigger.startsWith(QStringLiteral("steam."))) return QStringLiteral("Steam");
    return QStringLiteral("Schedule");
}

bool releaseDateChanged(const QVariantMap &before, const QVariantMap &after)
{
    static const QStringList fields{QStringLiteral("releaseDateText"),
        QStringLiteral("datePrecision"), QStringLiteral("exactDate"),
        QStringLiteral("windowStart"), QStringLiteral("windowEnd")};
    for (const QString &field : fields)
        if (before.value(field) != after.value(field)) return true;
    return false;
}

QString usageIdentityForPath(const QString &path)
{
    if (path.trimmed().isEmpty()) return {};
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
    return QStringLiteral("path:") + normalized;
}
}

AutomationController::AutomationController(
    application::ApplicationServiceGraph *graph,
    services::ApplicationMutationWorkflow *mutations,
    services::ApplicationUsageService *usage,
    platform::DesktopApplicationsPlatform *desktopPlatform,
    BackgroundController *background, QObject *parent,
    std::function<bool(const QUrl &)> openUrl)
    : QObject(parent), m_graph(graph), m_mutations(mutations), m_usage(usage),
      m_desktopPlatform(desktopPlatform), m_background(background),
      m_openUrl(openUrl ? std::move(openUrl) : [](const QUrl &url) {
          return QDesktopServices::openUrl(url);
      })
{
    Q_ASSERT(m_graph);
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &AutomationController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &AutomationController::onValueFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, [this](application::ApplicationLifecycle::State state) {
        if (state == application::ApplicationLifecycle::State::Running) reload();
        else if (state == application::ApplicationLifecycle::State::ShuttingDown) {
            m_stopping = true;
            m_tickTimer.stop();
        }
    });
    if (m_mutations) {
        connect(m_mutations, &services::ApplicationMutationWorkflow::taskCompletionCommitted,
                this, &AutomationController::onTaskCompletionCommitted);
    }
    if (m_usage) {
        connect(m_usage, &services::ApplicationUsageService::applicationSessionStarted,
                this, &AutomationController::onApplicationSessionStarted);
        connect(m_usage, &services::ApplicationUsageService::applicationStateChanged,
                this, &AutomationController::onApplicationStateChanged);
        connect(m_usage, &services::ApplicationUsageService::applicationSessionEnded,
                this, [this](const QString &, const QString &sessionId, const QString &, const QString &) {
            m_chainDepthBySession.remove(sessionId);
        });
    }
    m_tickTimer.setInterval(15000);
    connect(&m_tickTimer, &QTimer::timeout, this, [this] {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (auto it = m_automatedLaunchDepthByIdentity.begin();
             it != m_automatedLaunchDepthByIdentity.end();) {
            if (!it->second.isValid() || it->second <= now) it = m_automatedLaunchDepthByIdentity.erase(it);
            else ++it;
        }
        checkOverdueTasks();
        checkScheduledRules();
    });
    if (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Running)
        reload();
}

QVariantList AutomationController::rules() const
{
    QVector<domain::AutomationRule> sorted;
    sorted.reserve(m_rules.size());
    for (const auto &rule : m_rules) sorted.push_back(rule);
    std::sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) {
        return QString::compare(left.name, right.name, Qt::CaseInsensitive) < 0;
    });
    QVariantList result;
    result.reserve(sorted.size());
    for (const auto &rule : sorted) result.push_back(ruleToMap(rule));
    return result;
}

void AutomationController::reload()
{
    if (m_stopping) return;
    const bool recover = !m_recoveryStarted;
    m_recoveryStarted = true;
    submitStorage([recover](storage::StorageRepositories &repositories,
                     const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        if (recover) {
            const auto recovered = repositories.automations.cancelUnfinished(utcNow());
            if (!recovered.succeeded()) return storage::StorageReply{{}, {}, recovered.error};
        }
        const auto rows = repositories.automations.rules();
        if (!rows.succeeded()) return storage::StorageReply{{}, {}, rows.error};
        QVariantList ruleMaps;
        QVariantMap lastAttempted;
        for (const auto &rule : rows.value) {
            if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
            ruleMaps.push_back(AutomationController::ruleToMap(rule));
            lastAttempted.insert(rule.id, rule.lastExecutionRequestedAtUtc);
        }
        const auto paused = repositories.preferences.get(QStringLiteral("automation.paused"));
        if (!paused.succeeded()) return storage::StorageReply{{}, {}, paused.error};
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("rules"), ruleMaps);
        reply.values.insert(QStringLiteral("lastAttempted"), lastAttempted);
        reply.values.insert(QStringLiteral("paused"), paused.value == QStringLiteral("1"));
        return reply;
    }, [this, recover](const QVariantMap &values, domain::AppError error) {
        if (error.isError() || error.isCancelled()) {
            if (recover) m_recoveryStarted = false;
            m_errorMessage = QStringLiteral("Automation rules could not be loaded.");
            emit stateChanged();
            return;
        }
        m_rules.clear();
        for (const QVariant &value : values.value(QStringLiteral("rules")).toList()) {
            auto rule = ruleFromMap(value.toMap());
            m_rules.insert(rule.id, std::move(rule));
        }
        m_lastAttemptedByRule.clear();
        const QVariantMap attempted = values.value(QStringLiteral("lastAttempted")).toMap();
        for (auto it = attempted.cbegin(); it != attempted.cend(); ++it)
            m_lastAttemptedByRule.insert(it.key(), QDateTime::fromString(
                it.value().toString(), Qt::ISODateWithMs).toUTC());
        m_paused = values.value(QStringLiteral("paused")).toBool();
        m_ready = true;
        if (recover) {
            m_firstScheduleScan = true;
            m_lastScheduleMinute.clear();
        }
        m_errorMessage.clear();
        if (!m_tickTimer.isActive()) m_tickTimer.start();
        emit stateChanged();
    }, QStringLiteral("load automation rules"));
}

void AutomationController::saveRule(QVariantMap map, qint64 expectedRevision)
{
    if (!m_ready || m_stopping) return;
    domain::AutomationRule rule = ruleFromMap(map);
    if (rule.sourceTaskId > 0) {
        m_errorMessage = QStringLiteral("Task-specific automations are managed from the Task Editor.");
        emit stateChanged();
        return;
    }
    submitStorage([rule = std::move(rule), expectedRevision](storage::StorageRepositories &repositories,
                    const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto saved = repositories.automations.saveRule(std::move(rule), expectedRevision);
        if (!saved.succeeded()) return storage::StorageReply{{}, {}, saved.error};
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("rule"), ruleToMap(saved.value));
        return reply;
    }, [this](const QVariantMap &values, domain::AppError error) {
        if (error.isError() || error.isCancelled()) {
            m_errorMessage = error.category() == domain::ErrorCategory::Conflict
                ? QStringLiteral("This rule changed elsewhere. Reload it before saving again.")
                : QStringLiteral("The automation rule could not be saved. Check its trigger and actions.");
        } else {
            auto rule = ruleFromMap(values.value(QStringLiteral("rule")).toMap());
            const QString id = rule.id;
            m_rules.insert(id, std::move(rule));
            m_errorMessage.clear();
            emit ruleSaved(id);
        }
        emit stateChanged();
    }, QStringLiteral("save automation rule"));
}

void AutomationController::deleteRule(const QString &ruleId)
{
    if (!m_ready || !m_rules.contains(ruleId) || m_rules.value(ruleId).sourceTaskId > 0) return;
    submitStorage([ruleId](storage::StorageRepositories &repositories,
                           const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto deleted = repositories.automations.removeRule(ruleId);
        if (!deleted.succeeded()) return storage::StorageReply{{}, {}, deleted.error};
        return storage::StorageReply{};
    }, [this, ruleId](const QVariantMap &, domain::AppError error) {
        if (error.isError()) m_errorMessage = QStringLiteral("The automation rule could not be deleted.");
        else {
            m_rules.remove(ruleId);
            for (auto it = m_running.begin(); it != m_running.end();) {
                if (it->rule.id == ruleId) {
                    const QString executionId = it.key();
                    ++it;
                    finishExecution(executionId, QStringLiteral("canceled"),
                                    QStringLiteral("The rule was deleted before completion."));
                } else ++it;
            }
            emit ruleDeleted(ruleId);
        }
        emit stateChanged();
    }, QStringLiteral("delete automation rule"));
}

void AutomationController::duplicateRule(const QString &ruleId)
{
    const auto it = m_rules.constFind(ruleId);
    if (it == m_rules.cend() || it->sourceTaskId > 0) return;
    auto copy = ruleToMap(it.value());
    copy.remove(QStringLiteral("id"));
    copy.insert(QStringLiteral("sourceTaskId"), 0);
    copy.insert(QStringLiteral("name"), it->name + QStringLiteral(" copy"));
    copy.insert(QStringLiteral("revision"), 0);
    QVariantList actionMaps;
    for (const QVariant &item : copy.value(QStringLiteral("actions")).toList()) {
        QVariantMap action = item.toMap();
        action.remove(QStringLiteral("id"));
        actionMaps.push_back(action);
    }
    copy.insert(QStringLiteral("actions"), actionMaps);
    saveRule(copy, 0);
}

void AutomationController::setRuleEnabled(const QString &ruleId, bool enabled)
{
    const auto it = m_rules.constFind(ruleId);
    if (it == m_rules.cend() || it->sourceTaskId > 0 || it->enabled == enabled) return;
    QVariantMap map = ruleToMap(it.value());
    map.insert(QStringLiteral("enabled"), enabled);
    saveRule(map, it->revision);
}

void AutomationController::setPaused(bool paused)
{
    if (m_paused == paused || m_stopping) return;
    submitStorage([paused](storage::StorageRepositories &repositories,
                           const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto saved = repositories.preferences.set(QStringLiteral("automation.paused"),
            paused ? QStringLiteral("1") : QStringLiteral("0"));
        if (!saved.succeeded()) return storage::StorageReply{{}, {}, saved.error};
        return storage::StorageReply{};
    }, [this, paused](const QVariantMap &, domain::AppError error) {
        if (error.isError()) m_errorMessage = QStringLiteral("The automation pause state could not be saved.");
        else {
            m_paused = paused;
            m_errorMessage.clear();
            if (paused) {
                const QStringList running = m_running.keys();
                for (const QString &executionId : running)
                    finishExecution(executionId, QStringLiteral("canceled"),
                                    QStringLiteral("Automations were paused."));
            }
        }
        emit stateChanged();
    }, QStringLiteral("save automation pause state"));
}

void AutomationController::loadHistory(const QString &ruleId, const QString &status,
    const QString &triggerType, int offset, const QString &startDate, const QString &endDate)
{
    const quint64 generation = ++m_historyRequestGeneration;
    const QDate start = startDate.isEmpty() ? QDate{} : QDate::fromString(startDate, Qt::ISODate);
    const QDate end = endDate.isEmpty() ? QDate{} : QDate::fromString(endDate, Qt::ISODate);
    if ((!startDate.isEmpty() && !start.isValid()) || (!endDate.isEmpty() && !end.isValid())
        || (start.isValid() && end.isValid() && start > end)) {
        m_history.clear();
        m_historyHasMore = false;
        m_errorMessage = QStringLiteral("Enter a valid local date range in YYYY-MM-DD format.");
        emit stateChanged();
        return;
    }
    const QString startUtc = start.isValid()
        ? QDateTime(start, QTime(0, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs) : QString();
    const QString endExclusiveUtc = end.isValid()
        ? QDateTime(end.addDays(1), QTime(0, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs) : QString();
    submitStorage([ruleId, status, triggerType, offset, startUtc, endExclusiveUtc](storage::StorageRepositories &repositories,
                    const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto page = repositories.automations.history(ruleId, status, triggerType,
            startUtc, endExclusiveUtc, 50, offset);
        if (!page.succeeded()) return storage::StorageReply{{}, {}, page.error};
        storage::StorageReply reply; reply.values.insert(QStringLiteral("history"), page.value); return reply;
    }, [this, offset, generation](const QVariantMap &values, domain::AppError error) {
        if (generation != m_historyRequestGeneration) return;
        if (error.isError()) m_errorMessage = QStringLiteral("Automation execution history could not be loaded.");
        else {
            const QVariantList page = values.value(QStringLiteral("history")).toList();
            if (offset == 0) m_history = page;
            else m_history.append(page);
            while (m_history.size() > 500) m_history.removeFirst();
            m_historyHasMore = page.size() == 50;
            m_errorMessage.clear();
        }
        emit stateChanged();
    }, QStringLiteral("load automation execution history"));
}

void AutomationController::clearMessages()
{
    m_errorMessage.clear(); m_notification.clear(); emit stateChanged();
}

void AutomationController::onTaskCompletionCommitted(const QString &eventId,
    const QVariantMap &task, bool previousCompleted, bool currentCompleted)
{
    if (previousCompleted || !currentCompleted || !task.value(QStringLiteral("completed")).toBool()) return;
    receiveEvent(taskEvent(QStringLiteral("task:completed:") + eventId, task,
                           QStringLiteral("task.completed"),
                           task.value(QStringLiteral("completedAtUtc")).toString()));
}

void AutomationController::onApplicationSessionStarted(const QString &applicationId,
    const QString &sessionId, const QString &timestampUtc)
{
    domain::AutomationEvent event;
    event.id = QStringLiteral("usage:session:") + sessionId;
    event.type = QStringLiteral("application.session_started");
    event.sourceModule = QStringLiteral("application-usage");
    event.sourceEntityId = applicationId;
    event.occurredAtUtc = QDateTime::fromString(timestampUtc, Qt::ISODateWithMs).toUTC();
    const auto launchDepth = m_automatedLaunchDepthByIdentity.find(applicationId);
    if (launchDepth != m_automatedLaunchDepthByIdentity.end()) {
        if (launchDepth->second.isValid()
            && launchDepth->second > QDateTime::currentDateTimeUtc())
            event.chainDepth = launchDepth->first;
        m_automatedLaunchDepthByIdentity.erase(launchDepth);
    }
    m_chainDepthBySession.insert(sessionId, event.chainDepth);
    QString applicationName;
    if (m_usage) {
        for (const QVariant &value : m_usage->liveApplications()) {
            const QVariantMap row = value.toMap();
            if (row.value(QStringLiteral("applicationId")).toString() == applicationId) {
                applicationName = row.value(QStringLiteral("name")).toString();
                break;
            }
        }
    }
    event.payload = {{QStringLiteral("application.id"), applicationId},
        {QStringLiteral("application.sessionId"), sessionId},
        {QStringLiteral("application.name"), applicationName}};
    receiveEvent(std::move(event));
}

void AutomationController::onApplicationStateChanged(const QString &applicationId,
    const QString &sessionId, const QString &state, const QString &timestampUtc)
{
    if (!state.startsWith(QStringLiteral("Foreground"), Qt::CaseInsensitive)) return;
    domain::AutomationEvent event;
    event.id = QStringLiteral("usage:foreground:") + sessionId + QLatin1Char(':') + timestampUtc;
    event.type = QStringLiteral("application.foreground_entered");
    event.sourceModule = QStringLiteral("application-usage");
    event.sourceEntityId = applicationId;
    event.occurredAtUtc = QDateTime::fromString(timestampUtc, Qt::ISODateWithMs).toUTC();
    event.chainDepth = m_chainDepthBySession.value(sessionId, 0);
    event.payload = {{QStringLiteral("application.id"), applicationId},
        {QStringLiteral("application.sessionId"), sessionId}};
    if (m_usage) {
        for (const QVariant &value : m_usage->liveApplications()) {
            const QVariantMap row = value.toMap();
            if (row.value(QStringLiteral("applicationId")).toString() == applicationId) {
                event.payload.insert(QStringLiteral("application.name"),
                    row.value(QStringLiteral("name")));
                break;
            }
        }
    }
    receiveEvent(std::move(event));
}

void AutomationController::onSteamPriceRefreshCommitted(
    const QVariantMap &previous, const QVariantMap &current)
{
    if (previous.value(QStringLiteral("priceCountryCode")) != current.value(QStringLiteral("priceCountryCode"))
        || previous.value(QStringLiteral("currency")) != current.value(QStringLiteral("currency"))
        || previous.value(QStringLiteral("priceStatus")).toString() != QStringLiteral("priced")
        || current.value(QStringLiteral("priceStatus")).toString() != QStringLiteral("priced")) return;
    const qint64 oldPrice = previous.value(QStringLiteral("currentPriceMinor")).toLongLong();
    const qint64 price = current.value(QStringLiteral("currentPriceMinor")).toLongLong();
    if (oldPrice <= 0 || price <= 0) return;
    const QString country = current.value(QStringLiteral("priceCountryCode")).toString();
    const QString currency = current.value(QStringLiteral("currency")).toString();
    const QString timestamp = current.value(QStringLiteral("lastCheckedAt")).toString();
    const qint64 appId = current.value(QStringLiteral("steamAppId")).toLongLong();
    domain::AutomationEvent event;
    event.id = QStringLiteral("steam:price:%1:%2:%3").arg(appId).arg(country, timestamp);
    event.type = QStringLiteral("steam.price_target_reached");
    event.sourceModule = QStringLiteral("steam-price-tracking");
    event.sourceEntityId = QString::number(appId);
    event.occurredAtUtc = QDateTime::fromString(timestamp, Qt::ISODateWithMs).toUTC();
    event.payload = {{QStringLiteral("steam.appId"), appId},
        {QStringLiteral("steam.gameName"), current.value(QStringLiteral("name"))},
        {QStringLiteral("steam.priceMinor"), price},
        {QStringLiteral("steam.previousPriceMinor"), oldPrice},
        {QStringLiteral("steam.currency"), currency},
        {QStringLiteral("steam.country"), country},
        {QStringLiteral("steam.priceMinor"), price}};
    receiveEvent(std::move(event));
}

void AutomationController::onSteamReleaseRefreshCommitted(
    const QVariantMap &previous, const QVariantMap &current)
{
    if (previous.isEmpty() || current.isEmpty() || !releaseDateChanged(previous, current)) return;
    const qint64 appId = current.value(QStringLiteral("appId")).toLongLong();
    const QString timestamp = utcNow();
    domain::AutomationEvent event;
    event.id = QStringLiteral("steam:release:%1:%2").arg(appId).arg(timestamp);
    event.type = QStringLiteral("steam.release_date_changed");
    event.sourceModule = QStringLiteral("steam-release-radar");
    event.sourceEntityId = QString::number(appId);
    event.occurredAtUtc = QDateTime::fromString(timestamp, Qt::ISODateWithMs).toUTC();
    event.payload = {{QStringLiteral("steam.appId"), appId},
        {QStringLiteral("steam.gameName"), current.value(QStringLiteral("title"))},
        {QStringLiteral("steam.releaseDate"), current.value(QStringLiteral("releaseDateText"))},
        {QStringLiteral("steam.datePrecision"), current.value(QStringLiteral("datePrecision"))}};
    receiveEvent(std::move(event));
}

void AutomationController::onSteamNewsRefreshCommitted(
    quint32 appId, const QVariantList &newArticles, const QString &timestampUtc)
{
    for (const QVariant &value : newArticles) {
        const QVariantMap article = value.toMap();
        const QString newsId = article.value(QStringLiteral("newsId")).toString();
        if (newsId.isEmpty()) continue;
        domain::AutomationEvent event;
        event.id = QStringLiteral("steam:news:%1:%2").arg(appId).arg(newsId);
        event.type = QStringLiteral("steam.game_news_available");
        event.sourceModule = QStringLiteral("steam-game-updates");
        event.sourceEntityId = QString::number(appId);
        event.occurredAtUtc = QDateTime::fromString(timestampUtc, Qt::ISODateWithMs).toUTC();
        event.payload = {{QStringLiteral("steam.appId"), appId},
            {QStringLiteral("steam.gameName"), article.value(QStringLiteral("gameName"))},
            {QStringLiteral("steam.newsId"), newsId},
            {QStringLiteral("steam.newsTitle"), article.value(QStringLiteral("title"))}};
        receiveEvent(std::move(event));
    }
}

bool AutomationController::submitStorage(storage::StorageCommand command,
    std::function<void(const QVariantMap &, domain::AppError)> completed, QString purpose)
{
    if (m_stopping) return false;
    const auto operation = m_graph->beginOperation(std::move(purpose));
    if (!operation.isValid()) {
        if (completed) completed({}, domain::AppError(domain::ErrorCategory::Busy));
        return false;
    }
    const quint64 id = operation.id.value;
    m_operations.insert(id, {operation, std::move(completed)});
    if (m_graph->submitStorage(operation, std::move(command))) return true;
    auto pending = m_operations.take(id);
    if (pending.completed) pending.completed({}, domain::AppError(domain::ErrorCategory::Busy));
    return false;
}

bool AutomationController::submitValue(services::BoundedWorkerPool::ValueWork work,
    std::function<void(const QVariantMap &, domain::AppError)> completed, QString purpose)
{
    if (m_stopping) return false;
    const auto operation = m_graph->beginOperation(std::move(purpose));
    if (!operation.isValid()) {
        if (completed) completed({}, domain::AppError(domain::ErrorCategory::Busy));
        return false;
    }
    const quint64 id = operation.id.value;
    m_valueOperations.insert(id, std::move(completed));
    if (m_graph->submitExpensiveValue(operation, std::move(work))) return true;
    auto callback = m_valueOperations.take(id);
    if (callback) callback({}, domain::AppError(domain::ErrorCategory::Busy));
    return false;
}

void AutomationController::onStorageFinished(quint64 operationId,
    const QVariantMap &values, domain::AppError error)
{
    auto it = m_operations.find(operationId);
    if (it == m_operations.end()) return;
    auto callback = std::move(it->completed);
    m_operations.erase(it);
    if (callback) callback(values, error);
}

void AutomationController::onValueFinished(quint64 operationId,
    const QVariantMap &values, domain::AppError error)
{
    auto it = m_valueOperations.find(operationId);
    if (it == m_valueOperations.end()) return;
    auto callback = std::move(it.value());
    m_valueOperations.erase(it);
    if (callback) callback(values, error);
}

void AutomationController::receiveEvent(domain::AutomationEvent event)
{
    if (!m_ready || m_stopping
        || domain::AutomationRules::validateEvent(event).isError()) return;
    for (const auto &rule : m_rules) {
        if (!domain::AutomationRules::triggerMatches(rule, event)
            || !domain::AutomationRules::conditionsMatch(rule, event)) continue;
        int cooldown = rule.cooldownSeconds;
        if (rule.triggerType == QStringLiteral("application.foreground_entered"))
            cooldown = qMax(cooldown, 5);
        const QDateTime last = m_lastAttemptedByRule.value(rule.id);
        if (cooldown > 0 && last.isValid()
            && last.secsTo(QDateTime::currentDateTimeUtc()) < cooldown) {
            markBlocked(rule, event, QStringLiteral("Rule cooldown is active."));
            continue;
        }
        if (event.chainDepth >= 5) {
            markBlocked(rule, event, QStringLiteral("Automation chain depth limit reached."));
            continue;
        }
        if (m_running.size() + m_claimsPending >= 100) {
            markBlocked(rule, event, QStringLiteral("Maximum pending automation executions reached."));
            continue;
        }
        if (m_paused) continue;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        while (!m_executionRateWindow.isEmpty()
               && m_executionRateWindow.head().secsTo(now) >= 60)
            m_executionRateWindow.dequeue();
        if (m_executionRateWindow.size() >= 20) {
            markBlocked(rule, event, QStringLiteral("Automation rate limit reached."));
            continue;
        }
        m_executionRateWindow.enqueue(now);
        m_lastAttemptedByRule.insert(rule.id, now);
        auto current = m_rules.find(rule.id);
        if (current != m_rules.end())
            current->lastExecutionRequestedAtUtc = now.toString(Qt::ISODateWithMs);
        claimExecution(rule, event);
    }
}

void AutomationController::claimExecution(const domain::AutomationRule &rule,
    const domain::AutomationEvent &event)
{
    ++m_claimsPending;
    const QString executionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString requestedAt = utcNow();
    submitStorage([executionId, rule, event, requestedAt](storage::StorageRepositories &repositories,
                    const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto inserted = repositories.automations.insertExecution(
            executionId, rule.id, rule.name, event, requestedAt);
        if (!inserted.succeeded()) return storage::StorageReply{{}, {}, inserted.error};
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("inserted"), inserted.value);
        return reply;
    }, [this, executionId, rule, event](const QVariantMap &values, domain::AppError error) {
        m_claimsPending = qMax(0, m_claimsPending - 1);
        if (error.isError() || error.isCancelled() || !values.value(QStringLiteral("inserted")).toBool())
            return;
        RunningExecution running;
        running.rule = rule;
        running.event = event;
        running.executionId = executionId;
        m_running.insert(executionId, std::move(running));
        const auto current = m_rules.constFind(rule.id);
        if (current == m_rules.cend() || !current->enabled || current->revision != rule.revision) {
            finishExecution(executionId, QStringLiteral("canceled"),
                            QStringLiteral("Rule changed or was disabled before the action started."));
            return;
        }
        const QString startedAt = utcNow();
        submitStorage([executionId, startedAt](storage::StorageRepositories &repositories,
                        const std::function<bool()> &cancelled) {
            if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
            const auto result = repositories.automations.updateExecution(
                executionId, QStringLiteral("running"), startedAt, {}, {});
            if (!result.succeeded()) return storage::StorageReply{{}, {}, result.error};
            return storage::StorageReply{};
        }, [this, executionId, startedAt](const QVariantMap &, domain::AppError updateError) {
            if (updateError.isError()) finishExecution(executionId, QStringLiteral("failed"),
                QStringLiteral("Execution could not be started in history."));
            else {
                const auto it = m_running.find(executionId);
                if (it == m_running.end()) return;
                auto rule = m_rules.find(it->rule.id);
                if (rule != m_rules.end()) {
                    rule->lastExecutionAtUtc = startedAt;
                    rule->lastExecutionStatus = QStringLiteral("running");
                    rule->lastExecutionError.clear();
                }
                emit executionChanged(it->rule.sourceTaskId, QStringLiteral("running"), {}, startedAt);
                emit stateChanged();
                runNextAction(executionId);
            }
        }, QStringLiteral("start automation execution"));
    }, QStringLiteral("claim automation execution"));
}

void AutomationController::runNextAction(const QString &executionId)
{
    auto it = m_running.find(executionId);
    if (it == m_running.end()) return;
    const auto currentRule = m_rules.constFind(it->rule.id);
    if (m_stopping || m_paused || currentRule == m_rules.cend() || !currentRule->enabled
        || currentRule->revision != it->rule.revision) {
        finishExecution(executionId, QStringLiteral("canceled"),
            QStringLiteral("Rule was disabled, changed, or paused before the action ran."));
        return;
    }
    while (it->nextAction < it->rule.actions.size()
           && !it->rule.actions.at(it->nextAction).enabled) {
        const auto action = it->rule.actions.at(it->nextAction++);
        const QString timestamp = utcNow();
        recordAction(executionId, action, timestamp, QStringLiteral("skipped"),
                     QStringLiteral("Action is disabled."), true);
        return;
    }
    if (it->nextAction >= it->rule.actions.size()) {
        finishExecution(executionId, QStringLiteral("succeeded"));
        return;
    }
    const int delay = it->rule.actions.at(it->nextAction).delayMs;
    if (delay > 0) {
        QTimer::singleShot(delay, this, [this, executionId] { executeCurrentAction(executionId); });
    } else {
        executeCurrentAction(executionId);
    }
}

void AutomationController::executeCurrentAction(const QString &executionId)
{
    auto it = m_running.find(executionId);
    if (it == m_running.end()) return;
    const auto currentRule = m_rules.constFind(it->rule.id);
    if (m_stopping || m_paused || currentRule == m_rules.cend() || !currentRule->enabled
        || currentRule->revision != it->rule.revision) {
        finishExecution(executionId, QStringLiteral("canceled"),
            QStringLiteral("Execution was canceled before the delayed action ran."));
        return;
    }
    const domain::AutomationAction action = it->rule.actions.at(it->nextAction++);
    const QString actionStartedAt = utcNow();
    it->actionStartedAtUtc = actionStartedAt;
    auto complete = [this, executionId, action, actionStartedAt]
        (QString status, QString errorMessage) {
        recordAction(executionId, action, actionStartedAt, std::move(status),
                     std::move(errorMessage), true);
    };

    if (action.type == QStringLiteral("show_notification")) {
        bool valid = false;
        const QString message = domain::AutomationRules::expandNotification(
            action.message, it->event, &valid);
        if (!valid) { complete(QStringLiteral("failed"), QStringLiteral("Notification contains a placeholder unavailable for this event.")); return; }
        const QString title = safeTriggerTitle(it->event.type);
        const bool shown = m_background && m_background->showAutomationNotification(title, message);
        complete(shown ? QStringLiteral("succeeded") : QStringLiteral("failed"),
                 shown ? QString() : QStringLiteral("Notification could not be displayed."));
        return;
    }
    if (action.type == QStringLiteral("open_url")) {
        const QUrl url(action.url, QUrl::StrictMode);
        const bool opened = (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"))
            && !url.host().isEmpty() && url.userName().isEmpty() && url.password().isEmpty()
            && m_openUrl && m_openUrl(url);
        complete(opened ? QStringLiteral("succeeded") : QStringLiteral("failed"),
                 opened ? QString() : QStringLiteral("URL is invalid or could not be opened."));
        return;
    }

    const bool setup = action.type == QStringLiteral("launch_setup");
    if (action.type != QStringLiteral("launch_application") && !setup) {
        complete(QStringLiteral("failed"), QStringLiteral("Unsupported action type."));
        return;
    }
    const QString purpose = setup ? QStringLiteral("load automation setup target")
                                  : QStringLiteral("load automation application target");
    const storage::StorageCommand lookup = setup
        ? services::SetupsService::launchData(action.targetId)
        : services::DesktopApplicationsService::get(action.targetId);
    submitStorage(lookup, [this, executionId, setup, action, actionStartedAt]
        (const QVariantMap &values, domain::AppError error) {
        if (error.isError() || error.isCancelled()) {
            const QString message = error.category() == domain::ErrorCategory::NotFound
                ? (setup ? QStringLiteral("Selected Setup no longer exists.")
                         : QStringLiteral("Target application not found."))
                : (error.isCancelled() ? QStringLiteral("Action canceled.")
                                       : QStringLiteral("Action target could not be loaded."));
            recordAction(executionId, action, actionStartedAt,
                         error.isCancelled() ? QStringLiteral("canceled") : QStringLiteral("failed"),
                         message, true);
            return;
        }
        const auto native = m_desktopPlatform;
        const QVariantMap applicationRecord = values.value(QStringLiteral("application")).toMap();
        const QVariantList setupApplications = values.value(QStringLiteral("applications")).toList();
        auto execution = m_running.constFind(executionId);
        if (execution == m_running.cend()) return;
        const auto currentRule = m_rules.constFind(execution->rule.id);
        if (m_stopping || m_paused || currentRule == m_rules.cend()
            || !currentRule->enabled || currentRule->revision != execution->rule.revision) {
            finishExecution(executionId, QStringLiteral("canceled"),
                QStringLiteral("Rule was disabled, changed, or paused before the launch began."));
            return;
        }
        const int nextDepth = execution->event.chainDepth + 1;
        const QDateTime expiry = QDateTime::currentDateTimeUtc().addSecs(120);
        if (setup) {
            for (const QVariant &value : setupApplications) {
                const QString identity = usageIdentityForPath(value.toMap()
                    .value(QStringLiteral("executablePath")).toString());
                if (identity.isEmpty()) continue;
                const auto previousDepth = m_automatedLaunchDepthByIdentity.value(identity);
                m_automatedLaunchDepthByIdentity.insert(identity,
                    {qMax(nextDepth, previousDepth.first), expiry});
            }
        } else {
            const QString identity = usageIdentityForPath(applicationRecord
                .value(QStringLiteral("executablePath")).toString());
            if (!identity.isEmpty()) {
                const auto previousDepth = m_automatedLaunchDepthByIdentity.value(identity);
                m_automatedLaunchDepthByIdentity.insert(identity,
                    {qMax(nextDepth, previousDepth.first), expiry});
            }
        }
        submitValue([setup, native, applicationRecord, setupApplications]
                    (const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            if (!setup) {
                result.error = services::DesktopApplicationsService::launchSavedRecord(
                    applicationRecord, native, token);
                result.values = appErrorValues(result.error, QStringLiteral("Target application could not be launched."));
                return result;
            }
            application::CancellationSource userCancellation;
            const auto summary = services::SetupsService::launchMembers(setupApplications,
                token, userCancellation, [native](const QVariantMap &record,
                    const application::CancellationToken &appToken,
                    const application::CancellationToken &userToken) {
                    return services::DesktopApplicationsService::launchSavedRecord(
                        record, native, appToken, &userToken);
                });
            const bool succeeded = !summary.cancelled && summary.attempted > 0
                && summary.launched == summary.attempted;
            result.values.insert(QStringLiteral("status"), summary.cancelled
                ? QStringLiteral("canceled") : succeeded ? QStringLiteral("succeeded")
                    : QStringLiteral("failed"));
            if (summary.cancelled) result.error = domain::AppError::cancelled();
            else if (!succeeded) result.error = domain::AppError(domain::ErrorCategory::Provider);
            result.values.insert(QStringLiteral("errorMessage"), summary.cancelled
                ? QStringLiteral("Setup launch canceled.") : succeeded ? QString()
                    : QStringLiteral("Setup could not launch all saved applications."));
            return result;
        }, [this, executionId, action, actionStartedAt]
            (const QVariantMap &result, domain::AppError launchError) {
            QString status = result.value(QStringLiteral("status")).toString();
            if (status.isEmpty()) status = launchError.isCancelled()
                ? QStringLiteral("canceled") : QStringLiteral("failed");
            recordAction(executionId, action, actionStartedAt, status,
                result.value(QStringLiteral("errorMessage")).toString(), true);
        }, QStringLiteral("execute automation launch"));
    }, purpose);
}

void AutomationController::recordAction(const QString &executionId,
    const domain::AutomationAction &action, const QString &startedAt,
    QString status, QString errorMessage, bool continueSequence)
{
    const QString finishedAt = utcNow();
    submitStorage([executionId, action, startedAt, finishedAt, status, errorMessage]
        (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto saved = repositories.automations.recordActionResult(
            executionId, action, startedAt, finishedAt, status, errorMessage);
        if (!saved.succeeded()) return storage::StorageReply{{}, {}, saved.error};
        return storage::StorageReply{};
    }, [this, executionId, status, errorMessage, finishedAt, continueSequence]
       (const QVariantMap &, domain::AppError error) {
        auto it = m_running.find(executionId);
        if (it == m_running.end()) return;
        if (error.isError()) {
            finishExecution(executionId, QStringLiteral("failed"),
                            QStringLiteral("Action result could not be saved."));
            return;
        }
        if (status != QStringLiteral("succeeded") && status != QStringLiteral("skipped")) {
            finishExecution(executionId, status, errorMessage);
            return;
        }
        if (continueSequence) runNextAction(executionId);
    }, QStringLiteral("save automation action result"));
}

void AutomationController::finishExecution(const QString &executionId,
    QString status, QString errorMessage)
{
    auto it = m_running.find(executionId);
    if (it == m_running.end()) return;
    const QString ruleId = it->rule.id;
    const qint64 taskId = it->rule.sourceTaskId;
    const QString finishedAt = utcNow();
    const QString eventId = it->event.id;
    m_running.erase(it);
    auto rule = m_rules.find(ruleId);
    if (rule != m_rules.end()) {
        rule->lastExecutionAtUtc = finishedAt;
        rule->lastExecutionStatus = status;
        rule->lastExecutionError = errorMessage;
    }
    submitStorage([executionId, status, errorMessage, finishedAt]
        (storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto saved = repositories.automations.updateExecution(
            executionId, status, {}, finishedAt, errorMessage);
        if (!saved.succeeded()) return storage::StorageReply{{}, {}, saved.error};
        return storage::StorageReply{};
    }, [this, ruleId, taskId, eventId, status, errorMessage, finishedAt]
       (const QVariantMap &, domain::AppError error) {
        const QString visibleError = error.isError()
            ? QStringLiteral("Execution finished, but its result could not be saved.") : errorMessage;
        const QString visibleStatus = error.isError() ? QStringLiteral("failed") : status;
        if (error.isError()) {
            auto current = m_rules.find(ruleId);
            if (current != m_rules.end()) {
                current->lastExecutionStatus = visibleStatus;
                current->lastExecutionError = visibleError;
            }
        }
        emit executionChanged(taskId, visibleStatus, visibleError, finishedAt);
        if (!visibleError.isEmpty()) {
            m_errorMessage = visibleError;
            m_notification = QStringLiteral("Automation execution failed.");
        }
        emit stateChanged();
        Q_UNUSED(eventId);
    }, QStringLiteral("finish automation execution"));
}

void AutomationController::markBlocked(const domain::AutomationRule &rule,
    const domain::AutomationEvent &event, QString reason)
{
    const QString executionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString requestedAt = utcNow();
    submitStorage([executionId, rule, event, reason, requestedAt](storage::StorageRepositories &repositories,
                    const std::function<bool()> &cancelled) {
        if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
        const auto inserted = repositories.automations.insertExecution(executionId, rule.id,
            rule.name, event, requestedAt);
        if (!inserted.succeeded()) return storage::StorageReply{{}, {}, inserted.error};
        if (!inserted.value) {
            storage::StorageReply duplicate;
            duplicate.values.insert(QStringLiteral("inserted"), false);
            return duplicate;
        }
        const QString finished = utcNow();
        const auto updated = repositories.automations.updateExecution(executionId,
            QStringLiteral("skipped"), {}, finished, reason);
        if (!updated.succeeded()) return storage::StorageReply{{}, {}, updated.error};
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("inserted"), true);
        return reply;
    }, [this, rule, reason, requestedAt](const QVariantMap &values, domain::AppError error) {
        if (error.isError()) {
            m_errorMessage = QStringLiteral("A blocked automation could not be recorded.");
            m_notification = QStringLiteral("Automation history could not be saved.");
            emit stateChanged();
            return;
        }
        if (!values.value(QStringLiteral("inserted")).toBool()) return;
        auto current = m_rules.find(rule.id);
        const QString timestamp = requestedAt;
        if (current != m_rules.end()) {
            current->lastExecutionRequestedAtUtc = timestamp;
            current->lastExecutionAtUtc = timestamp;
            current->lastExecutionStatus = QStringLiteral("skipped");
            current->lastExecutionError = reason;
        }
        emit executionChanged(rule.sourceTaskId, QStringLiteral("skipped"), reason, timestamp);
        emit stateChanged();
    }, QStringLiteral("record blocked automation"));
}

void AutomationController::scheduleTick()
{
    if (!m_stopping && !m_tickTimer.isActive()) m_tickTimer.start();
}

void AutomationController::checkOverdueTasks()
{
    if (!m_ready || m_stopping || m_paused || m_overdueQueryPending) return;
    bool hasRules = false;
    for (const auto &rule : m_rules)
        if (rule.enabled && rule.triggerType == QStringLiteral("task.overdue")) { hasRules = true; break; }
    if (!hasRules) return;
    m_overdueQueryPending = true;
    const int startOffset = m_overdueOffset;
    submitStorage([startOffset](storage::StorageRepositories &repositories,
                     const std::function<bool()> &cancelled) {
        storage::TaskFilter filter;
        filter.overdue = true;
        filter.sort = QStringLiteral("due_date");
        filter.direction = QStringLiteral("asc");
        filter.limit = 200;
        int offset = startOffset;
        QVariantList rows;
        for (int page = 0; page < 5; ++page) {
            if (cancelled()) return storage::StorageReply{{}, {}, domain::AppError::cancelled()};
            filter.offset = offset;
            const auto tasks = repositories.tasks.list(filter);
            if (!tasks.succeeded()) return storage::StorageReply{{}, {}, tasks.error};
            for (const auto &task : tasks.value) rows.push_back(services::TaskService::toVariantMap(task));
            offset += tasks.value.size();
            if (tasks.value.size() < filter.limit) { offset = 0; break; }
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("tasks"), rows);
        reply.values.insert(QStringLiteral("nextOffset"), offset);
        return reply;
    }, [this](const QVariantMap &values, domain::AppError error) {
        m_overdueQueryPending = false;
        if (error.isError() || error.isCancelled()) return;
        m_overdueOffset = values.value(QStringLiteral("nextOffset")).toInt();
        for (const QVariant &item : values.value(QStringLiteral("tasks")).toList()) {
            const QVariantMap task = item.toMap();
            const QString dueDate = task.value(QStringLiteral("dueDate")).toString();
            const qint64 id = task.value(QStringLiteral("id")).toLongLong();
            if (id <= 0 || dueDate.isEmpty()) continue;
            receiveEvent(taskEvent(QStringLiteral("task:overdue:%1:%2").arg(id).arg(dueDate),
                task, QStringLiteral("task.overdue"), utcNow()));
        }
    }, QStringLiteral("check overdue automation tasks"));
}

void AutomationController::checkScheduledRules()
{
    if (!m_ready || m_stopping || m_paused) return;
    const QDateTime localNow = QDateTime::currentDateTime();
    const QString minute = localNow.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    if (m_firstScheduleScan) {
        m_firstScheduleScan = false;
        m_lastScheduleMinute = minute;
        return; // missed and startup-time slots are skipped, never replayed
    }
    if (minute == m_lastScheduleMinute) return;
    m_lastScheduleMinute = minute;
    for (const auto &rule : m_rules) {
        if (!rule.enabled || rule.triggerType != QStringLiteral("schedule.time_reached")) continue;
        const QTime configured = QTime::fromString(
            rule.triggerConfig.value(QStringLiteral("time")).toString(), QStringLiteral("HH:mm"));
        if (!configured.isValid() || configured != localNow.time().addSecs(-localNow.time().second())) continue;
        const QString date = rule.triggerConfig.value(QStringLiteral("date")).toString();
        if (!date.isEmpty() && date != localNow.date().toString(Qt::ISODate)) continue;
        const QVariantList weekdays = rule.triggerConfig.value(QStringLiteral("weekdays")).toList();
        if (date.isEmpty() && !weekdays.isEmpty()) {
            bool selected = false;
            for (const auto &day : weekdays) if (day.toInt() == localNow.date().dayOfWeek()) selected = true;
            if (!selected) continue;
        }
        domain::AutomationEvent event;
        event.id = QStringLiteral("schedule:%1:%2").arg(rule.id, minute);
        event.type = QStringLiteral("schedule.time_reached");
        event.sourceModule = QStringLiteral("automation-scheduler");
        event.sourceEntityId = rule.id;
        event.occurredAtUtc = localNow.toUTC();
        event.payload = {{QStringLiteral("schedule.id"), rule.id},
            {QStringLiteral("schedule.localTime"), minute}};
        receiveEvent(std::move(event));
    }
}

domain::AutomationRule AutomationController::ruleFromMap(const QVariantMap &map)
{
    domain::AutomationRule rule;
    rule.id = map.value(QStringLiteral("id")).toString();
    rule.sourceTaskId = map.value(QStringLiteral("sourceTaskId")).toLongLong();
    rule.name = map.value(QStringLiteral("name")).toString();
    rule.description = map.value(QStringLiteral("description")).toString();
    rule.triggerType = map.value(QStringLiteral("triggerType")).toString();
    rule.triggerConfig = map.value(QStringLiteral("triggerConfig")).toMap();
    rule.enabled = map.value(QStringLiteral("enabled"), true).toBool();
    rule.cooldownSeconds = map.value(QStringLiteral("cooldownSeconds")).toInt();
    rule.revision = map.value(QStringLiteral("revision"), 1).toLongLong();
    rule.createdAtUtc = map.value(QStringLiteral("createdAtUtc")).toString();
    rule.updatedAtUtc = map.value(QStringLiteral("updatedAtUtc")).toString();
    rule.lastExecutionRequestedAtUtc = map.value(QStringLiteral("lastExecutionRequestedAtUtc")).toString();
    rule.lastExecutionAtUtc = map.value(QStringLiteral("lastExecutionAtUtc")).toString();
    rule.lastExecutionStatus = map.value(QStringLiteral("lastExecutionStatus")).toString();
    rule.lastExecutionError = map.value(QStringLiteral("lastExecutionError")).toString();
    for (const QVariant &item : map.value(QStringLiteral("conditions")).toList()) {
        const QVariantMap condition = item.toMap();
        rule.conditions.push_back({condition.value(QStringLiteral("type")).toString(),
                                   condition.value(QStringLiteral("operand")).toString(),
                                   condition.value(QStringLiteral("operator")).toString()});
    }
    for (const QVariant &item : map.value(QStringLiteral("actions")).toList()) {
        const QVariantMap value = item.toMap();
        domain::AutomationAction action;
        action.id = value.value(QStringLiteral("id")).toString();
        action.type = value.value(QStringLiteral("type")).toString();
        action.targetId = value.value(QStringLiteral("targetId")).toLongLong();
        action.url = value.value(QStringLiteral("url")).toString();
        action.message = value.value(QStringLiteral("message")).toString();
        action.delayMs = value.value(QStringLiteral("delayMs")).toInt();
        action.order = value.value(QStringLiteral("order")).toInt();
        action.enabled = value.value(QStringLiteral("enabled"), true).toBool();
        rule.actions.push_back(std::move(action));
    }
    return rule;
}

QVariantMap AutomationController::ruleToMap(const domain::AutomationRule &rule)
{
    QVariantList conditions;
    for (const auto &condition : rule.conditions)
        conditions.push_back(QVariantMap{{QStringLiteral("type"), condition.type},
            {QStringLiteral("operand"), condition.operand},
            {QStringLiteral("operator"), condition.op}});
    QVariantList actions;
    for (const auto &action : rule.actions)
        actions.push_back(QVariantMap{{QStringLiteral("id"), action.id},
            {QStringLiteral("type"), action.type}, {QStringLiteral("targetId"), action.targetId},
            {QStringLiteral("url"), action.url}, {QStringLiteral("message"), action.message},
            {QStringLiteral("delayMs"), action.delayMs}, {QStringLiteral("order"), action.order},
            {QStringLiteral("enabled"), action.enabled}});
    return {{QStringLiteral("id"), rule.id}, {QStringLiteral("sourceTaskId"), rule.sourceTaskId},
        {QStringLiteral("name"), rule.name},
        {QStringLiteral("description"), rule.description},
        {QStringLiteral("triggerType"), rule.triggerType},
        {QStringLiteral("triggerConfig"), rule.triggerConfig},
        {QStringLiteral("conditions"), conditions}, {QStringLiteral("actions"), actions},
        {QStringLiteral("enabled"), rule.enabled},
        {QStringLiteral("cooldownSeconds"), rule.cooldownSeconds},
        {QStringLiteral("revision"), rule.revision},
        {QStringLiteral("createdAtUtc"), rule.createdAtUtc},
        {QStringLiteral("updatedAtUtc"), rule.updatedAtUtc},
        {QStringLiteral("lastExecutionRequestedAtUtc"), rule.lastExecutionRequestedAtUtc},
        {QStringLiteral("lastExecutionAtUtc"), rule.lastExecutionAtUtc},
        {QStringLiteral("lastExecutionStatus"), rule.lastExecutionStatus},
        {QStringLiteral("lastExecutionError"), rule.lastExecutionError},
        {QStringLiteral("conditionSummary"), domain::AutomationRules::conditionSummary(rule)},
        {QStringLiteral("actionSummary"), domain::AutomationRules::actionSummary(rule)}};
}

domain::AutomationEvent AutomationController::taskEvent(const QString &eventId,
    const QVariantMap &task, const QString &type, const QString &timestampUtc)
{
    domain::AutomationEvent event;
    event.id = eventId;
    event.type = type;
    event.sourceModule = QStringLiteral("tasks");
    event.sourceEntityId = QString::number(task.value(QStringLiteral("id")).toLongLong());
    event.occurredAtUtc = QDateTime::fromString(timestampUtc, Qt::ISODateWithMs).toUTC();
    if (!event.occurredAtUtc.isValid()) event.occurredAtUtc = QDateTime::currentDateTimeUtc();
    event.payload = {{QStringLiteral("task.id"), task.value(QStringLiteral("id"))},
        {QStringLiteral("task.title"), task.value(QStringLiteral("title"))},
        {QStringLiteral("task.priority"), task.value(QStringLiteral("priority"))},
        {QStringLiteral("task.tags"), task.value(QStringLiteral("tags"))},
        {QStringLiteral("task.dueDate"), task.value(QStringLiteral("dueDate"))}};
    if (task.contains(QStringLiteral("completedAtUtc")))
        event.payload.insert(QStringLiteral("task.completedAt"),
            task.value(QStringLiteral("completedAtUtc")));
    return event;
}

QString AutomationController::utcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

} // namespace something::controllers
