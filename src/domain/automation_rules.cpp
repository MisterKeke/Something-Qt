#include "automation_rules.h"
#include "notification_placeholders.h"

#include <QRegularExpression>
#include <QSet>
#include <QTime>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace something::domain::AutomationRules {
namespace {
const QStringList kTriggers{
    QStringLiteral("task.completed"), QStringLiteral("task.overdue"),
    QStringLiteral("application.session_started"),
    QStringLiteral("application.foreground_entered"),
    QStringLiteral("steam.price_target_reached"),
    QStringLiteral("steam.release_date_changed"),
    QStringLiteral("steam.game_news_available"),
    QStringLiteral("schedule.time_reached")};
const QStringList kActions{
    QStringLiteral("launch_application"), QStringLiteral("launch_setup"),
    QStringLiteral("open_url"), QStringLiteral("show_notification")};

bool isInteger(const QString &value, qint64 *number = nullptr)
{
    bool ok = false;
    const qint64 parsed = value.toLongLong(&ok);
    if (number) *number = parsed;
    return ok;
}

bool validCondition(const AutomationCondition &condition, const QString &trigger)
{
    if (condition.type == QStringLiteral("task_id")) {
        qint64 value = 0;
        return (trigger == QStringLiteral("task.completed")
                || trigger == QStringLiteral("task.overdue"))
            && isInteger(condition.operand, &value) && value > 0;
    }
    if (condition.type == QStringLiteral("task_tag"))
        return (trigger == QStringLiteral("task.completed")
                || trigger == QStringLiteral("task.overdue"))
            && !condition.operand.trimmed().isEmpty() && condition.operand.size() <= 64;
    if (condition.type == QStringLiteral("task_priority"))
        return (trigger == QStringLiteral("task.completed")
                || trigger == QStringLiteral("task.overdue"))
            && (condition.operand == QStringLiteral("low")
                || condition.operand == QStringLiteral("medium")
                || condition.operand == QStringLiteral("high"));
    if (condition.type == QStringLiteral("application_id"))
        return (trigger == QStringLiteral("application.session_started")
                || trigger == QStringLiteral("application.foreground_entered"))
            && !condition.operand.trimmed().isEmpty() && condition.operand.size() <= 512;
    if (condition.type == QStringLiteral("steam_app_id")) {
        qint64 value = 0;
        return trigger.startsWith(QStringLiteral("steam."))
            && isInteger(condition.operand, &value) && value > 0 && value <= 0xffffffffLL;
    }
    if (condition.type == QStringLiteral("price_lte")) {
        qint64 value = 0;
        return trigger == QStringLiteral("steam.price_target_reached")
            && isInteger(condition.operand, &value) && value > 0;
    }
    if (condition.type == QStringLiteral("day_of_week")) {
        qint64 value = 0;
        return trigger == QStringLiteral("schedule.time_reached")
            && isInteger(condition.operand, &value) && value >= 1 && value <= 7;
    }
    if (condition.type == QStringLiteral("time_range")) {
        const QStringList bounds = condition.operand.split(QLatin1Char('/'));
        return trigger == QStringLiteral("schedule.time_reached") && bounds.size() == 2
            && QTime::fromString(bounds.at(0), QStringLiteral("HH:mm")).isValid()
            && QTime::fromString(bounds.at(1), QStringLiteral("HH:mm")).isValid();
    }
    return false;
}

QVariantMap eventFields(const AutomationEvent &event)
{
    QVariantMap fields = event.payload;
    fields.insert(QStringLiteral("event.id"), event.id);
    fields.insert(QStringLiteral("event.type"), event.type);
    fields.insert(QStringLiteral("event.time"), event.occurredAtUtc.toLocalTime());
    return fields;
}
}

QStringList triggerTypes() { return kTriggers; }
QStringList actionTypes() { return kActions; }

AppError validateEvent(const AutomationEvent &event)
{
    if (event.id.isEmpty() || event.id.size() > 256 || !kTriggers.contains(event.type)
        || event.sourceModule.isEmpty() || event.sourceModule.size() > 64
        || event.sourceEntityId.size() > 256 || !event.occurredAtUtc.isValid()
        || event.occurredAtUtc.timeSpec() != Qt::UTC || event.chainDepth < 0 || event.chainDepth > 8)
        return AppError(ErrorCategory::Validation);
    const auto payloadString = [&event](const QString &key) {
        return event.payload.value(key).toString();
    };
    if (event.type.startsWith(QStringLiteral("task."))) {
        qint64 taskId = 0;
        if (!isInteger(payloadString(QStringLiteral("task.id")), &taskId) || taskId <= 0
            || payloadString(QStringLiteral("task.title")).size() > 512)
            return AppError(ErrorCategory::Validation);
    } else if (event.type.startsWith(QStringLiteral("application."))) {
        if (payloadString(QStringLiteral("application.id")).isEmpty()
            || payloadString(QStringLiteral("application.id")).size() > 512)
            return AppError(ErrorCategory::Validation);
    } else if (event.type.startsWith(QStringLiteral("steam."))) {
        qint64 appId = 0;
        if (!isInteger(payloadString(QStringLiteral("steam.appId")), &appId)
            || appId <= 0 || appId > 0xffffffffLL)
            return AppError(ErrorCategory::Validation);
    }
    return {};
}

AppError normalizeRule(AutomationRule &rule)
{
    rule.name = rule.name.trimmed();
    rule.description = rule.description.trimmed();
    if (rule.name.isEmpty() || rule.name.size() > 100 || rule.description.size() > 500
        || !kTriggers.contains(rule.triggerType) || rule.cooldownSeconds < 0
        || rule.cooldownSeconds > 86400 || rule.conditions.size() > 12
        || rule.actions.isEmpty() || rule.actions.size() > 12)
        return AppError(ErrorCategory::Validation);

    if (rule.triggerType == QStringLiteral("task.completed")
        || rule.triggerType == QStringLiteral("task.overdue")) {
        const qint64 taskId = rule.triggerConfig.value(QStringLiteral("taskId")).toLongLong();
        if (taskId < 0 || rule.triggerConfig.value(QStringLiteral("tag")).toString().size() > 64)
            return AppError(ErrorCategory::Validation);
        const QString priority = rule.triggerConfig.value(QStringLiteral("priority")).toString();
        if (!priority.isEmpty() && priority != QStringLiteral("low")
            && priority != QStringLiteral("medium") && priority != QStringLiteral("high"))
            return AppError(ErrorCategory::Validation);
    } else if (rule.triggerType.startsWith(QStringLiteral("application."))) {
        const QString identity = rule.triggerConfig.value(QStringLiteral("applicationId")).toString();
        if (identity.trimmed().isEmpty() || identity.size() > 512)
            return AppError(ErrorCategory::Validation);
    } else if (rule.triggerType.startsWith(QStringLiteral("steam."))) {
        const qint64 appId = rule.triggerConfig.value(QStringLiteral("appId")).toLongLong();
        if (appId <= 0 || appId > 0xffffffffLL) return AppError(ErrorCategory::Validation);
        if (rule.triggerType == QStringLiteral("steam.price_target_reached")) {
            const qint64 target = rule.triggerConfig.value(QStringLiteral("targetMinor")).toLongLong();
            const QString currency = rule.triggerConfig.value(QStringLiteral("currency")).toString();
            const QString country = rule.triggerConfig.value(QStringLiteral("country")).toString();
            static const QRegularExpression code(QStringLiteral("^[A-Z]{3}$"));
            static const QRegularExpression countryCode(QStringLiteral("^[A-Z]{2}$"));
            if (target <= 0 || !code.match(currency).hasMatch()
                || !countryCode.match(country).hasMatch())
                return AppError(ErrorCategory::Validation);
        }
    } else if (rule.triggerType == QStringLiteral("schedule.time_reached")) {
        const QString time = rule.triggerConfig.value(QStringLiteral("time")).toString();
        const QString date = rule.triggerConfig.value(QStringLiteral("date")).toString();
        const QVariantList days = rule.triggerConfig.value(QStringLiteral("weekdays")).toList();
        if (!QTime::fromString(time, QStringLiteral("HH:mm")).isValid()
            || days.size() > 7 || (date.isEmpty() && days.isEmpty())
            || (!date.isEmpty() && !QDate::fromString(date, Qt::ISODate).isValid()))
            return AppError(ErrorCategory::Validation);
        for (const QVariant &day : days)
            if (day.toInt() < 1 || day.toInt() > 7) return AppError(ErrorCategory::Validation);
    }

    for (AutomationCondition &condition : rule.conditions) {
        if (!validCondition(condition, rule.triggerType)) return AppError(ErrorCategory::Validation);
        const QString expectedOperator = condition.type == QStringLiteral("price_lte")
            ? QStringLiteral("less_equal") : condition.type == QStringLiteral("time_range")
                ? QStringLiteral("between") : condition.type == QStringLiteral("task_tag")
                    ? QStringLiteral("contains") : QStringLiteral("equals");
        if (condition.op.isEmpty()) condition.op = expectedOperator;
        if (condition.op != expectedOperator) return AppError(ErrorCategory::Validation);
    }
    QSet<QString> ids;
    for (int index = 0; index < rule.actions.size(); ++index) {
        AutomationAction &action = rule.actions[index];
        if (action.id.isEmpty()) action.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (ids.contains(action.id) || !kActions.contains(action.type)
            || action.delayMs < 0 || action.delayMs > 3600000 || action.order < 0)
            return AppError(ErrorCategory::Validation);
        ids.insert(action.id);
        if (action.type == QStringLiteral("launch_application")
            || action.type == QStringLiteral("launch_setup")) {
            if (action.targetId <= 0 || !action.url.isEmpty()) return AppError(ErrorCategory::Validation);
        } else if (action.type == QStringLiteral("open_url")) {
            const QUrl url(action.url.trimmed());
            if ((url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))
                || url.host().isEmpty() || !url.userName().isEmpty() || !url.password().isEmpty())
                return AppError(ErrorCategory::Validation);
        } else if (action.type == QStringLiteral("show_notification")) {
            if (action.message.isEmpty() || action.message.size() > 500
                || !NotificationPlaceholders::templateSupported(action.message, rule.triggerType))
                return AppError(ErrorCategory::Validation);
        }
        action.order = index;
    }
    std::stable_sort(rule.actions.begin(), rule.actions.end(), [](const auto &left, const auto &right) {
        return left.order < right.order;
    });
    return {};
}

bool triggerMatches(const AutomationRule &rule, const AutomationEvent &event)
{
    if (!rule.enabled || rule.triggerType != event.type) return false;
    const QVariantMap &config = rule.triggerConfig;
    if (event.type.startsWith(QStringLiteral("task."))) {
        const qint64 configuredId = config.value(QStringLiteral("taskId")).toLongLong();
        return (configuredId == 0 || configuredId == event.payload.value(QStringLiteral("task.id")).toLongLong())
            && (config.value(QStringLiteral("tag")).toString().isEmpty()
                || event.payload.value(QStringLiteral("task.tags")).toStringList()
                    .contains(config.value(QStringLiteral("tag")).toString(), Qt::CaseInsensitive))
            && (config.value(QStringLiteral("priority")).toString().isEmpty()
                || config.value(QStringLiteral("priority")).toString()
                    == event.payload.value(QStringLiteral("task.priority")).toString());
    }
    if (event.type.startsWith(QStringLiteral("application.")))
        return config.value(QStringLiteral("applicationId")).toString().isEmpty()
            || config.value(QStringLiteral("applicationId")).toString()
                == event.payload.value(QStringLiteral("application.id")).toString();
    if (event.type.startsWith(QStringLiteral("steam."))) {
        if (config.value(QStringLiteral("appId")).toLongLong()
            != event.payload.value(QStringLiteral("steam.appId")).toLongLong()) return false;
        if (event.type == QStringLiteral("steam.price_target_reached"))
            return config.value(QStringLiteral("country")).toString()
                    == event.payload.value(QStringLiteral("steam.country")).toString()
                && config.value(QStringLiteral("currency")).toString()
                    == event.payload.value(QStringLiteral("steam.currency")).toString()
                && event.payload.value(QStringLiteral("steam.previousPriceMinor")).toLongLong()
                    > config.value(QStringLiteral("targetMinor")).toLongLong()
                && event.payload.value(QStringLiteral("steam.priceMinor")).toLongLong()
                    <= config.value(QStringLiteral("targetMinor")).toLongLong();
        return true;
    }
    if (event.type == QStringLiteral("schedule.time_reached"))
        return (config.value(QStringLiteral("scheduleId")).toString().isEmpty()
                    ? rule.id : config.value(QStringLiteral("scheduleId")).toString())
            == event.payload.value(QStringLiteral("schedule.id")).toString();
    return false;
}

bool conditionsMatch(const AutomationRule &rule, const AutomationEvent &event)
{
    const QVariantMap payload = eventFields(event);
    for (const AutomationCondition &condition : rule.conditions) {
        if (condition.type == QStringLiteral("task_id")
            && payload.value(QStringLiteral("task.id")).toString() != condition.operand) return false;
        if (condition.type == QStringLiteral("task_tag")
            && !payload.value(QStringLiteral("task.tags")).toStringList()
                .contains(condition.operand, Qt::CaseInsensitive)) return false;
        if (condition.type == QStringLiteral("task_priority")
            && payload.value(QStringLiteral("task.priority")).toString() != condition.operand) return false;
        if (condition.type == QStringLiteral("application_id")
            && payload.value(QStringLiteral("application.id")).toString() != condition.operand) return false;
        if (condition.type == QStringLiteral("steam_app_id")
            && payload.value(QStringLiteral("steam.appId")).toString() != condition.operand) return false;
        if (condition.type == QStringLiteral("price_lte")
            && payload.value(QStringLiteral("steam.priceMinor")).toLongLong()
                > condition.operand.toLongLong()) return false;
        if (condition.type == QStringLiteral("day_of_week")
            && payload.value(QStringLiteral("event.time")).toDateTime().date().dayOfWeek()
                != condition.operand.toInt()) return false;
        if (condition.type == QStringLiteral("time_range")) {
            const QStringList bounds = condition.operand.split(QLatin1Char('/'));
            const QTime current = payload.value(QStringLiteral("event.time")).toDateTime().time();
            const QTime from = QTime::fromString(bounds.at(0), QStringLiteral("HH:mm"));
            const QTime to = QTime::fromString(bounds.at(1), QStringLiteral("HH:mm"));
            const int currentMinute = current.hour() * 60 + current.minute();
            const int fromMinute = from.hour() * 60 + from.minute();
            const int toMinute = to.hour() * 60 + to.minute();
            const bool matches = fromMinute <= toMinute
                ? (currentMinute >= fromMinute && currentMinute <= toMinute)
                : (currentMinute >= fromMinute || currentMinute <= toMinute);
            if (!matches) return false;
        }
    }
    return true;
}

bool scheduledTimeMatchesMinute(const QString &configuredTime, const QDateTime &localNow)
{
    const QTime configured = QTime::fromString(configuredTime, QStringLiteral("HH:mm"));
    if (!configured.isValid() || !localNow.isValid()) return false;
    const QTime observedMinute(localNow.time().hour(), localNow.time().minute());
    return configured == observedMinute;
}

bool isForegroundEntry(bool wasForeground, const QString &state)
{
    if (state.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0) return false;
    return !wasForeground
        && state.startsWith(QStringLiteral("Foreground"), Qt::CaseInsensitive);
}

QString conditionSummary(const AutomationRule &rule)
{
    if (rule.conditions.isEmpty()) return QStringLiteral("No extra conditions");
    QStringList values;
    for (const auto &condition : rule.conditions)
        values.push_back(condition.type + QStringLiteral(": ") + condition.operand);
    return values.join(QStringLiteral(" · "));
}

QString actionSummary(const AutomationRule &rule)
{
    QStringList values;
    for (const auto &action : rule.actions) {
        if (!action.enabled) continue;
        values.push_back(QString(action.type).replace(QLatin1Char('_'), QLatin1Char(' ')));
    }
    return values.join(QStringLiteral(" → "));
}

QString expandNotification(QString pattern, const AutomationEvent &event, bool *valid)
{
    static const QRegularExpression token(QStringLiteral("\\{([a-zA-Z.]+)\\}"));
    const QVariantMap fields = eventFields(event);
    QString result;
    qsizetype cursor = 0;
    auto matches = token.globalMatch(pattern);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += pattern.mid(cursor, match.capturedStart() - cursor);
        const QString key = match.captured(1);
        if (!fields.contains(key)) {
            if (valid) *valid = false;
            return {};
        }
        result += fields.value(key).toString();
        cursor = match.capturedEnd();
    }
    result += pattern.mid(cursor);
    if (valid) *valid = true;
    return result;
}
} // namespace something::domain::AutomationRules
