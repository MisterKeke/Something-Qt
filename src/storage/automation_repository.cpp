#include "repositories.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <utility>

namespace something::storage {
namespace {
QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

template<typename T>
RepositoryResult<T> failure(domain::ErrorCategory category)
{
    RepositoryResult<T> result;
    result.error = domain::AppError(category);
    return result;
}

template<typename T>
RepositoryResult<T> failure(const QSqlQuery &query)
{
    Q_UNUSED(query);
    return failure<T>(domain::ErrorCategory::Storage);
}

QVariantMap jsonMap(const QString &json)
{
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    return document.isObject() ? document.object().toVariantMap() : QVariantMap{};
}
}

AutomationRepository::AutomationRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<QVector<AutomationRule>> AutomationRepository::rules() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id FROM automation_rules ORDER BY updated_at_utc DESC, name COLLATE NOCASE")))
        return failure<QVector<AutomationRule>>(query);
    QVector<AutomationRule> values;
    while (query.next()) {
        const auto loaded = loadRule(query.value(0).toString());
        if (!loaded.succeeded()) return failure<QVector<AutomationRule>>(loaded.error.category());
        values.push_back(loaded.value);
    }
    RepositoryResult<QVector<AutomationRule>> result;
    result.value = std::move(values);
    return result;
}

RepositoryResult<AutomationRule> AutomationRepository::loadRule(const QString &ruleId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT r.id,COALESCE(r.source_task_id,0),r.name,r.description,r.trigger_type,r.trigger_config_json,r.enabled,"
        "r.cooldown_seconds,r.revision,r.created_at_utc,r.updated_at_utc,"
        "COALESCE((SELECT requested_at_utc FROM automation_executions e WHERE e.rule_id=r.id "
        "AND e.status<>'skipped' "
        "ORDER BY e.rowid DESC LIMIT 1),''),COALESCE((SELECT COALESCE(finished_at_utc,requested_at_utc) "
        "FROM automation_executions e WHERE e.rule_id=r.id "
        "ORDER BY e.rowid DESC LIMIT 1),''),COALESCE((SELECT status FROM automation_executions e "
        "WHERE e.rule_id=r.id ORDER BY e.rowid DESC LIMIT 1),''),COALESCE((SELECT error_message "
        "FROM automation_executions e WHERE e.rule_id=r.id ORDER BY e.rowid DESC LIMIT 1),'') "
        "FROM automation_rules r WHERE r.id=?"));
    query.addBindValue(ruleId);
    if (!query.exec()) return failure<AutomationRule>(query);
    if (!query.next()) return failure<AutomationRule>(domain::ErrorCategory::NotFound);
    AutomationRule rule;
    rule.id = query.value(0).toString();
    rule.sourceTaskId = query.value(1).toLongLong();
    rule.name = query.value(2).toString();
    rule.description = query.value(3).toString();
    rule.triggerType = query.value(4).toString();
    rule.triggerConfig = jsonMap(query.value(5).toString());
    rule.enabled = query.value(6).toBool();
    rule.cooldownSeconds = query.value(7).toInt();
    rule.revision = query.value(8).toLongLong();
    rule.createdAtUtc = query.value(9).toString();
    rule.updatedAtUtc = query.value(10).toString();
    rule.lastExecutionRequestedAtUtc = query.value(11).toString();
    rule.lastExecutionAtUtc = query.value(12).toString();
    rule.lastExecutionStatus = query.value(13).toString();
    rule.lastExecutionError = query.value(14).toString();

    QSqlQuery conditions(m_database);
    conditions.prepare(QStringLiteral(
        "SELECT condition_type,operand,operator FROM automation_conditions WHERE rule_id=? ORDER BY position"));
    conditions.addBindValue(ruleId);
    if (!conditions.exec()) return failure<AutomationRule>(conditions);
    while (conditions.next())
        rule.conditions.push_back({conditions.value(0).toString(), conditions.value(1).toString(),
                                   conditions.value(2).toString()});

    QSqlQuery actions(m_database);
    actions.prepare(QStringLiteral(
        "SELECT action_id,action_type,target_id,url,message,delay_ms,execution_order,enabled "
        "FROM automation_actions WHERE rule_id=? ORDER BY execution_order"));
    actions.addBindValue(ruleId);
    if (!actions.exec()) return failure<AutomationRule>(actions);
    while (actions.next()) {
        AutomationAction action;
        action.id = actions.value(0).toString();
        action.type = actions.value(1).toString();
        action.targetId = actions.value(2).toLongLong();
        action.url = actions.value(3).toString();
        action.message = actions.value(4).toString();
        action.delayMs = actions.value(5).toInt();
        action.order = actions.value(6).toInt();
        action.enabled = actions.value(7).toBool();
        rule.actions.push_back(std::move(action));
    }
    RepositoryResult<AutomationRule> result;
    result.value = std::move(rule);
    return result;
}

RepositoryResult<AutomationRule> AutomationRepository::saveRule(
    AutomationRule rule, qint64 expectedRevision)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (expectedRevision < 0 || rule.sourceTaskId != 0)
        return failure<AutomationRule>(domain::ErrorCategory::Validation);
    if (rule.id.isEmpty()) rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (domain::AutomationRules::normalizeRule(rule).isError())
        return failure<AutomationRule>(domain::ErrorCategory::Validation);
    const QString timestamp = nowUtc();
    if (!m_database.transaction()) return failure<AutomationRule>(domain::ErrorCategory::Storage);
    auto rollback = [this] { m_database.rollback(); };

    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral(
        "SELECT revision,created_at_utc,source_task_id FROM automation_rules WHERE id=?"));
    exists.addBindValue(rule.id);
    if (!exists.exec()) { rollback(); return failure<AutomationRule>(exists); }
    const bool updating = exists.next();
    if (updating && !exists.value(2).isNull()) {
        rollback();
        return failure<AutomationRule>(domain::ErrorCategory::Validation);
    }
    if ((updating && (expectedRevision <= 0 || exists.value(0).toLongLong() != expectedRevision))
        || (!updating && expectedRevision != 0)) {
        rollback();
        return failure<AutomationRule>(domain::ErrorCategory::Conflict);
    }
    rule.revision = updating ? expectedRevision + 1 : 1;
    rule.createdAtUtc = updating ? exists.value(1).toString() : timestamp;
    rule.updatedAtUtc = timestamp;
    const QString config = QString::fromUtf8(QJsonDocument(
        QJsonObject::fromVariantMap(rule.triggerConfig)).toJson(QJsonDocument::Compact));
    QSqlQuery save(m_database);
    if (updating) {
        save.prepare(QStringLiteral(
            "UPDATE automation_rules SET name=?,description=?,trigger_type=?,trigger_config_json=?,"
            "enabled=?,cooldown_seconds=?,revision=?,updated_at_utc=? WHERE id=? AND revision=?"));
        save.addBindValue(rule.name); save.addBindValue(rule.description);
        save.addBindValue(rule.triggerType); save.addBindValue(config); save.addBindValue(rule.enabled);
        save.addBindValue(rule.cooldownSeconds); save.addBindValue(rule.revision);
        save.addBindValue(timestamp); save.addBindValue(rule.id); save.addBindValue(expectedRevision);
    } else {
        save.prepare(QStringLiteral(
            "INSERT INTO automation_rules(id,name,description,trigger_type,trigger_config_json,"
            "enabled,cooldown_seconds,revision,created_at_utc,updated_at_utc) VALUES(?,?,?,?,?,?,?,?,?,?)"));
        save.addBindValue(rule.id); save.addBindValue(rule.name); save.addBindValue(rule.description);
        save.addBindValue(rule.triggerType); save.addBindValue(config); save.addBindValue(rule.enabled);
        save.addBindValue(rule.cooldownSeconds); save.addBindValue(rule.revision);
        save.addBindValue(timestamp); save.addBindValue(timestamp);
    }
    if (!save.exec()) { rollback(); return failure<AutomationRule>(save); }
    QSqlQuery removeChildren(m_database);
    removeChildren.prepare(QStringLiteral("DELETE FROM automation_conditions WHERE rule_id=?"));
    removeChildren.addBindValue(rule.id);
    if (!removeChildren.exec()) { rollback(); return failure<AutomationRule>(removeChildren); }
    QSqlQuery removeActions(m_database);
    removeActions.prepare(QStringLiteral("DELETE FROM automation_actions WHERE rule_id=?"));
    removeActions.addBindValue(rule.id);
    if (!removeActions.exec()) { rollback(); return failure<AutomationRule>(removeActions); }
    QSqlQuery addCondition(m_database);
    addCondition.prepare(QStringLiteral(
        "INSERT INTO automation_conditions(rule_id,position,condition_type,operand,operator) VALUES(?,?,?,?,?)"));
    for (int i = 0; i < rule.conditions.size(); ++i) {
        const auto &condition = rule.conditions.at(i);
        addCondition.bindValue(0, rule.id); addCondition.bindValue(1, i);
        addCondition.bindValue(2, condition.type); addCondition.bindValue(3, condition.operand);
        addCondition.bindValue(4, condition.op);
        if (!addCondition.exec()) { rollback(); return failure<AutomationRule>(addCondition); }
    }
    QSqlQuery addAction(m_database);
    addAction.prepare(QStringLiteral(
        "INSERT INTO automation_actions(action_id,rule_id,execution_order,action_type,target_id,"
        "url,message,delay_ms,enabled) VALUES(?,?,?,?,?,?,?,?,?)"));
    for (const auto &action : rule.actions) {
        addAction.bindValue(0, action.id); addAction.bindValue(1, rule.id);
        addAction.bindValue(2, action.order); addAction.bindValue(3, action.type);
        addAction.bindValue(4, action.targetId > 0 ? QVariant(action.targetId) : QVariant());
        addAction.bindValue(5, action.url.isEmpty() ? QVariant() : QVariant(action.url));
        addAction.bindValue(6, action.message); addAction.bindValue(7, action.delayMs);
        addAction.bindValue(8, action.enabled);
        if (!addAction.exec()) { rollback(); return failure<AutomationRule>(addAction); }
    }
    if (!m_database.commit()) { rollback(); return failure<AutomationRule>(domain::ErrorCategory::Storage); }
    return loadRule(rule.id);
}

RepositoryResult<void> AutomationRepository::removeRule(const QString &ruleId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "DELETE FROM automation_rules WHERE id=? AND source_task_id IS NULL"));
    query.addBindValue(ruleId);
    if (!query.exec()) return failure<void>(query);
    if (query.numRowsAffected() == 0) return failure<void>(domain::ErrorCategory::NotFound);
    return {};
}

RepositoryResult<bool> AutomationRepository::insertExecution(
    const QString &executionId, const QString &ruleId, const QString &ruleName,
    const AutomationEvent &event, const QString &requestedAtUtc)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const QDateTime requested = QDateTime::fromString(requestedAtUtc, Qt::ISODateWithMs);
    if (executionId.isEmpty() || ruleId.isEmpty() || event.id.isEmpty()
        || !requested.isValid() || requested.timeSpec() != Qt::UTC
        || domain::AutomationRules::validateEvent(event).isError())
        return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO automation_executions(execution_id,rule_id,rule_name_snapshot,event_id,"
        "trigger_type,source_module,requested_at_utc,event_occurred_at_utc,status) "
        "VALUES(?,?,?,?,?,?,?,?,'pending') "
        "ON CONFLICT(rule_id,event_id) DO NOTHING"));
    query.addBindValue(executionId); query.addBindValue(ruleId); query.addBindValue(ruleName);
    query.addBindValue(event.id); query.addBindValue(event.type); query.addBindValue(event.sourceModule);
    query.addBindValue(requestedAtUtc);
    query.addBindValue(event.occurredAtUtc.toString(Qt::ISODateWithMs));
    if (!query.exec()) return failure<bool>(query);
    RepositoryResult<bool> result;
    result.value = query.numRowsAffected() == 1;
    return result;
}

RepositoryResult<void> AutomationRepository::updateExecution(
    const QString &executionId, QString status, QString startedAtUtc,
    QString finishedAtUtc, QString errorMessage)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    static const QSet<QString> allowed{QStringLiteral("pending"), QStringLiteral("running"),
        QStringLiteral("succeeded"), QStringLiteral("failed"), QStringLiteral("canceled"),
        QStringLiteral("skipped")};
    if (!allowed.contains(status)) return failure<void>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE automation_executions SET status=?,started_at_utc=COALESCE(NULLIF(?,''),started_at_utc),"
        "finished_at_utc=COALESCE(NULLIF(?,''),finished_at_utc),error_message=? WHERE execution_id=?"));
    query.addBindValue(status); query.addBindValue(startedAtUtc); query.addBindValue(finishedAtUtc);
    query.addBindValue(errorMessage.left(1000)); query.addBindValue(executionId);
    if (!query.exec()) return failure<void>(query);
    return {};
}

RepositoryResult<void> AutomationRepository::recordActionResult(
    const QString &executionId, const AutomationAction &action, QString startedAtUtc,
    QString finishedAtUtc, QString status, QString errorMessage)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    static const QSet<QString> allowed{QStringLiteral("succeeded"), QStringLiteral("failed"),
        QStringLiteral("canceled"), QStringLiteral("skipped")};
    if (executionId.isEmpty() || action.id.isEmpty() || !allowed.contains(status))
        return failure<void>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO automation_action_results(execution_id,action_id,action_type,execution_order,"
        "started_at_utc,finished_at_utc,status,error_message) VALUES(?,?,?,?,?,?,?,?)"));
    query.addBindValue(executionId); query.addBindValue(action.id); query.addBindValue(action.type);
    query.addBindValue(action.order); query.addBindValue(startedAtUtc); query.addBindValue(finishedAtUtc);
    query.addBindValue(status); query.addBindValue(errorMessage.left(1000));
    if (!query.exec()) return failure<void>(query);
    return {};
}

RepositoryResult<QVariantList> AutomationRepository::history(
    const QString &ruleId, const QString &status, const QString &triggerType,
    const QString &startUtc, const QString &endExclusiveUtc, int limit, int offset) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (limit < 1 || limit > 200 || offset < 0 || offset > 1000000)
        return failure<QVariantList>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT execution_id,COALESCE(rule_id,''),rule_name_snapshot,event_id,trigger_type,source_module,"
        "requested_at_utc,event_occurred_at_utc,started_at_utc,finished_at_utc,status,error_message,"
        "(SELECT COUNT(*) FROM automation_action_results a WHERE a.execution_id=e.execution_id AND a.status='succeeded'),"
        "(SELECT COUNT(*) FROM automation_action_results a WHERE a.execution_id=e.execution_id) "
        "FROM automation_executions e WHERE (?='' OR rule_id=?) AND (?='' OR status=?) "
        "AND (?='' OR trigger_type=?) AND (?='' OR requested_at_utc>=?) "
        "AND (?='' OR requested_at_utc<?) "
        "ORDER BY requested_at_utc DESC,execution_id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(ruleId); query.addBindValue(ruleId);
    query.addBindValue(status); query.addBindValue(status);
    query.addBindValue(triggerType); query.addBindValue(triggerType);
    query.addBindValue(startUtc); query.addBindValue(startUtc);
    query.addBindValue(endExclusiveUtc); query.addBindValue(endExclusiveUtc);
    query.addBindValue(limit); query.addBindValue(offset);
    if (!query.exec()) return failure<QVariantList>(query);
    QVariantList rows;
    while (query.next()) {
        rows.push_back(QVariantMap{{QStringLiteral("executionId"), query.value(0)},
            {QStringLiteral("ruleId"), query.value(1)}, {QStringLiteral("ruleName"), query.value(2)},
            {QStringLiteral("eventId"), query.value(3)}, {QStringLiteral("triggerType"), query.value(4)},
            {QStringLiteral("sourceModule"), query.value(5)}, {QStringLiteral("requestedAtUtc"), query.value(6)},
            {QStringLiteral("eventOccurredAtUtc"), query.value(7)},
            {QStringLiteral("startedAtUtc"), query.value(8)}, {QStringLiteral("finishedAtUtc"), query.value(9)},
            {QStringLiteral("status"), query.value(10)}, {QStringLiteral("errorMessage"), query.value(11)},
            {QStringLiteral("actionsCompleted"), query.value(12)}, {QStringLiteral("actionsAttempted"), query.value(13)}});
    }
    RepositoryResult<QVariantList> result; result.value = std::move(rows); return result;
}

RepositoryResult<int> AutomationRepository::cancelUnfinished(QString finishedAtUtc)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE automation_executions SET status='canceled',finished_at_utc=?,"
        "error_message='Interrupted by application exit; side effects are not replayed.' "
        "WHERE status IN ('pending','running')"));
    query.addBindValue(finishedAtUtc);
    if (!query.exec()) return failure<int>(query);
    RepositoryResult<int> result; result.value = query.numRowsAffected(); return result;
}

} // namespace something::storage
