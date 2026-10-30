#pragma once

#include "task.h"
#include "task_scheduling.h"

#include <QDateTime>
#include <QStringList>
#include <QTimeZone>
#include <QVector>

#include <optional>

namespace something::domain {

struct SchedulingInput final {
    QVector<Task> incompleteTasks;
    QVector<TaskSchedule> existingSchedules;
    SchedulingPreferences preferences;
    QDateTime nowUtc;
    QTimeZone localTimeZone;
    int planningHorizonDays{365};
};

enum class RecommendationState {
    Scheduled,
    Unscheduled,
};

struct ScheduleRecommendation final {
    Task task;
    qint64 taskRevision{0};
    qint64 scheduleRevision{0};
    std::optional<QDate> plannedDate;
    std::optional<QTime> plannedStartTime;
    int estimatedDurationMinutes{0};
    int score{0};
    RecommendationState state{RecommendationState::Unscheduled};
    bool overdue{false};
    bool usedDefaultDuration{false};
    bool previouslyScheduled{false};
    bool retainedExistingSchedule{false};
    bool hasConflict{false};
    bool overloaded{false};
    QString reason;
    QStringList warnings;
};

struct SchedulingDaySummary final {
    QDate date;
    int plannedMinutes{0};
    int capacityMinutes{0};
    int remainingMinutes{0};
    bool overloaded{false};
    bool hasUnspecifiedStartTime{false};
};

struct SchedulingResult final {
    QVector<ScheduleRecommendation> recommendations;
    QVector<SchedulingDaySummary> days;
    QStringList warnings;
    QDate today;
    int planningHorizonDays{0};
};

// Produces a deterministic, explainable one-block-per-task proposal. The
// function is value-only and has no database, QObject, or system-clock access.
AppError calculateScheduleRecommendations(const SchedulingInput &input,
                                          SchedulingResult *result);

} // namespace something::domain
