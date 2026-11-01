#pragma once

#include "app_error.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace something::domain {

// Event and rule values are detached from QObject and SQLite lifetimes so
// sources, matching, persistence, and presentation can be tested independently.
struct AutomationEvent final {
    QString id;
    QString type;
    QString sourceModule;
    QString sourceEntityId;
    QDateTime occurredAtUtc;
    QVariantMap payload;
    int chainDepth{0};
};

struct AutomationCondition final {
    QString type;
    QString operand;
    QString op;
};

struct AutomationAction final {
    QString id;
    QString type;
    qint64 targetId{0};
    QString url;
    QString message;
    int delayMs{0};
    int order{0};
    bool enabled{true};
};

struct AutomationRule final {
    QString id;
    qint64 sourceTaskId{0}; // nonzero rules are owned by the Task Editor
    QString name;
    QString description;
    QString triggerType;
    QVariantMap triggerConfig;
    QVector<AutomationCondition> conditions;
    QVector<AutomationAction> actions;
    bool enabled{true};
    int cooldownSeconds{0};
    qint64 revision{1};
    QString createdAtUtc;
    QString updatedAtUtc;
    QString lastExecutionRequestedAtUtc;
    QString lastExecutionAtUtc;
    QString lastExecutionStatus;
    QString lastExecutionError;
};

namespace AutomationRules {

QStringList triggerTypes();
QStringList actionTypes();
AppError validateEvent(const AutomationEvent &event);
AppError normalizeRule(AutomationRule &rule);
bool triggerMatches(const AutomationRule &rule, const AutomationEvent &event);
bool conditionsMatch(const AutomationRule &rule, const AutomationEvent &event);
QString conditionSummary(const AutomationRule &rule);
QString actionSummary(const AutomationRule &rule);
QString expandNotification(QString pattern, const AutomationEvent &event,
                           bool *valid = nullptr);

} // namespace AutomationRules
} // namespace something::domain
