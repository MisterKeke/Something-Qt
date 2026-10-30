#pragma once

#include "app_error.h"

#include <QDate>
#include <QString>
#include <QTime>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace something::domain {

// A task's due date remains a deadline. These values describe work planned
// separately from that deadline. Times are local wall-clock times in the
// profile's current time zone; they are not UTC instants.
struct TaskSchedule final {
    qint64 taskId{0};
    std::optional<QDate> plannedDate;
    std::optional<QTime> plannedStartTime;
    std::optional<int> estimatedDurationMinutes;
    // Zero represents a task with no schedule metadata row yet.
    qint64 revision{0};
    QString updatedAt;
};

struct SchedulingPreferences final {
    // ISO weekday numbers: Monday=1 through Sunday=7. An empty list means
    // the profile has explicitly disabled all working days.
    QVector<int> preferredWorkingDays{1, 2, 3, 4, 5};
    QTime workdayStart{9, 0};
    QTime workdayEnd{17, 0};
    int maximumPlannedWorkMinutesPerDay{360};
    int defaultEstimatedDurationMinutes{60};
    std::optional<int> breakBetweenWorkBlocksMinutes{15};
    qint64 revision{1};
    QString updatedAt;
};

inline constexpr int MaximumTaskEstimateMinutes = 525600; // one year
inline constexpr int MaximumDailyWorkMinutes = 24 * 60;
inline constexpr int MaximumBreakMinutes = 240;

// Canonicalizes the working-day order and validates all scheduling values.
AppError normalizeTaskSchedule(TaskSchedule &schedule);
AppError normalizeSchedulingPreferences(SchedulingPreferences &preferences);

} // namespace something::domain
