#include "task_scheduling.h"

#include <QSet>

#include <algorithm>

namespace something::domain {
namespace {

bool hasMinutePrecision(const QTime &time)
{
    return time.isValid() && time.second() == 0 && time.msec() == 0;
}

} // namespace

AppError normalizeTaskSchedule(TaskSchedule &schedule)
{
    if (schedule.taskId <= 0 || schedule.revision < 0
        || (schedule.plannedDate && !schedule.plannedDate->isValid())
        || (schedule.plannedStartTime
            && (!hasMinutePrecision(*schedule.plannedStartTime) || !schedule.plannedDate))
        || (schedule.estimatedDurationMinutes
            && (*schedule.estimatedDurationMinutes < 1
                || *schedule.estimatedDurationMinutes > MaximumTaskEstimateMinutes))) {
        return AppError(ErrorCategory::Validation);
    }
    return AppError::none();
}

AppError normalizeSchedulingPreferences(SchedulingPreferences &preferences)
{
    if (preferences.revision <= 0 || !hasMinutePrecision(preferences.workdayStart)
        || !hasMinutePrecision(preferences.workdayEnd)
        || preferences.workdayStart >= preferences.workdayEnd
        || preferences.maximumPlannedWorkMinutesPerDay < 1
        || preferences.maximumPlannedWorkMinutesPerDay > MaximumDailyWorkMinutes
        || preferences.defaultEstimatedDurationMinutes < 1
        || preferences.defaultEstimatedDurationMinutes > MaximumTaskEstimateMinutes
        || (preferences.breakBetweenWorkBlocksMinutes
            && (*preferences.breakBetweenWorkBlocksMinutes < 1
                || *preferences.breakBetweenWorkBlocksMinutes > MaximumBreakMinutes))) {
        return AppError(ErrorCategory::Validation);
    }

    const int availableWindowMinutes =
        (preferences.workdayEnd.hour() * 60 + preferences.workdayEnd.minute())
        - (preferences.workdayStart.hour() * 60 + preferences.workdayStart.minute());
    if (preferences.maximumPlannedWorkMinutesPerDay > availableWindowMinutes) {
        return AppError(ErrorCategory::Validation);
    }

    QSet<int> seen;
    for (const int day : preferences.preferredWorkingDays) {
        if (day < 1 || day > 7 || seen.contains(day)) {
            return AppError(ErrorCategory::Validation);
        }
        seen.insert(day);
    }
    std::sort(preferences.preferredWorkingDays.begin(), preferences.preferredWorkingDays.end());
    return AppError::none();
}

} // namespace something::domain
