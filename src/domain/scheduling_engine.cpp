#include "scheduling_engine.h"

#include "task_scheduling.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <utility>

namespace something::domain {
namespace {

struct Candidate final {
    Task task;
    TaskSchedule previous;
    std::optional<QDate> deadline;
    int durationMinutes{0};
    int score{0};
    int priorityRank{0};
    int difficultyRank{0};
    int resultIndex{-1};
    bool usedDefaultDuration{false};
    bool previouslyScheduled{false};
    bool overdue{false};
    QStringList warnings;
};

struct OccupiedBlock final {
    qint64 taskId{0};
    QDateTime startUtc;
    QDateTime endUtc;
};

struct DayState final {
    int plannedMinutes{0};
    bool hasUnspecifiedStartTime{false};
    QVector<OccupiedBlock> blocks;
    QVector<qint64> taskIds;
};

int priorityRank(const QString &priority)
{
    if (priority == QStringLiteral("high")) return 2;
    if (priority == QStringLiteral("medium")) return 1;
    return 0;
}

int difficultyRank(const QString &difficulty)
{
    if (difficulty == QStringLiteral("hard")) return 3;
    if (difficulty == QStringLiteral("medium")) return 2;
    if (difficulty == QStringLiteral("easy")) return 1;
    return 0;
}

int urgencyPoints(const std::optional<QDate> &deadline, QDate today, bool overdue)
{
    if (!deadline) return 0;
    if (overdue) return 250;
    const qint64 daysUntilDeadline = today.daysTo(*deadline);
    const qint64 boundedDays = std::min<qint64>(daysUntilDeadline, 20);
    return int(std::max<qint64>(0, 200 - boundedDays * 10));
}

bool candidateLess(const Candidate &left, const Candidate &right)
{
    if (left.score != right.score) return left.score > right.score;
    if (left.deadline.has_value() != right.deadline.has_value()) return left.deadline.has_value();
    if (left.deadline && right.deadline && *left.deadline != *right.deadline)
        return *left.deadline < *right.deadline;
    if (left.priorityRank != right.priorityRank) return left.priorityRank > right.priorityRank;
    if (left.difficultyRank != right.difficultyRank)
        return left.difficultyRank > right.difficultyRank;
    if (left.previouslyScheduled != right.previouslyScheduled) return left.previouslyScheduled;
    if (left.durationMinutes != right.durationMinutes)
        return left.durationMinutes < right.durationMinutes;
    return left.task.id < right.task.id;
}

bool isWorkingDay(const SchedulingPreferences &preferences, QDate date)
{
    return preferences.preferredWorkingDays.contains(date.dayOfWeek());
}

bool localDateTime(QDate date, QTime time, const QTimeZone &zone, QDateTime *result)
{
    const QDateTime value(date, time, zone);
    if (!value.isValid() || value.date() != date || value.time() != time) return false;
    *result = value;
    return true;
}

bool blockFitsWorkday(const QDateTime &start, int durationMinutes,
                      const SchedulingPreferences &preferences,
                      const QTimeZone &zone, QDateTime *endUtc)
{
    if (!start.isValid() || start.time() < preferences.workdayStart
        || start.time() >= preferences.workdayEnd || durationMinutes <= 0) {
        return false;
    }
    const QDateTime end = start.addSecs(qint64(durationMinutes) * 60);
    if (!end.isValid()) return false;
    const QDateTime endLocal = end.toTimeZone(zone);
    if (!endLocal.isValid() || endLocal.date() != start.date()
        || endLocal.time() > preferences.workdayEnd) {
        return false;
    }
    *endUtc = end.toUTC();
    return true;
}

bool blocksConflict(const QDateTime &startUtc, const QDateTime &endUtc,
                    const OccupiedBlock &existing, int breakMinutes)
{
    const qint64 requiredGap = qint64(breakMinutes) * 60 * 1000;
    if (endUtc <= existing.startUtc)
        return endUtc.msecsTo(existing.startUtc) < requiredGap;
    if (existing.endUtc <= startUtc)
        return existing.endUtc.msecsTo(startUtc) < requiredGap;
    return true;
}

QString taskLabel(const Candidate &candidate)
{
    return candidate.task.title.isEmpty()
        ? QStringLiteral("task %1").arg(candidate.task.id)
        : QStringLiteral("“%1”").arg(candidate.task.title);
}

void addWarning(ScheduleRecommendation &recommendation, const QString &warning)
{
    if (!recommendation.warnings.contains(warning)) recommendation.warnings.push_back(warning);
}

void setOverdueWarning(ScheduleRecommendation &recommendation)
{
    if (!recommendation.overdue) return;
    addWarning(recommendation,
               QStringLiteral("The deadline was already overdue on the current local date."));
}

void setDeadlineWarning(ScheduleRecommendation &recommendation,
                        const std::optional<QDate> &deadline)
{
    if (recommendation.plannedDate && deadline && *recommendation.plannedDate > *deadline) {
        addWarning(recommendation,
                   QStringLiteral("Cannot schedule before the deadline with the current capacity."));
    }
}

int firstMinuteAtOrAfterNow(QDate date, QDate today, const QDateTime &localNow)
{
    if (date != today) return 0;
    const QTime time = localNow.time();
    int minute = time.hour() * 60 + time.minute();
    if (time.second() > 0 || time.msec() > 0) ++minute;
    return minute;
}

bool tryPlace(Candidate &candidate, QDate date, QDate today,
              const QDateTime &localNow, const QTimeZone &zone,
              const SchedulingPreferences &preferences,
              QMap<QDate, DayState> &days, QTime *chosenStart)
{
    if (!isWorkingDay(preferences, date)) return false;
    DayState &day = days[date];
    if (day.hasUnspecifiedStartTime
        || day.plannedMinutes + candidate.durationMinutes
               > preferences.maximumPlannedWorkMinutesPerDay) {
        return false;
    }

    const int workdayStartMinute = preferences.workdayStart.hour() * 60
        + preferences.workdayStart.minute();
    const int workdayEndMinute = preferences.workdayEnd.hour() * 60
        + preferences.workdayEnd.minute();
    int cursor = std::max(workdayStartMinute,
                          firstMinuteAtOrAfterNow(date, today, localNow));
    const int breakMinutes = preferences.breakBetweenWorkBlocksMinutes.value_or(0);

    while (cursor < workdayEndMinute) {
        const QTime startTime(cursor / 60, cursor % 60);
        QDateTime start;
        if (!localDateTime(date, startTime, zone, &start)) {
            ++cursor; // Skip a nonexistent local time at a daylight-saving boundary.
            continue;
        }
        if (date == today && start < localNow) {
            ++cursor;
            continue;
        }

        QDateTime endUtc;
        if (!blockFitsWorkday(start, candidate.durationMinutes, preferences, zone, &endUtc))
            break;
        const QDateTime startUtc = start.toUTC();

        bool conflict = false;
        int nextCursor = cursor + 1;
        for (const OccupiedBlock &existing : std::as_const(day.blocks)) {
            if (!blocksConflict(startUtc, endUtc, existing, breakMinutes)) continue;
            conflict = true;
            const QDateTime afterBreak = existing.endUtc.addSecs(qint64(breakMinutes) * 60)
                                             .toTimeZone(zone);
            if (afterBreak.date() == date) {
                const int afterMinute = afterBreak.time().hour() * 60
                    + afterBreak.time().minute()
                    + ((afterBreak.time().second() > 0 || afterBreak.time().msec() > 0) ? 1 : 0);
                nextCursor = std::max(nextCursor, afterMinute);
            } else {
                nextCursor = workdayEndMinute;
            }
        }
        if (!conflict) {
            day.plannedMinutes += candidate.durationMinutes;
            day.blocks.push_back({candidate.task.id, startUtc, endUtc});
            day.taskIds.push_back(candidate.task.id);
            std::sort(day.blocks.begin(), day.blocks.end(), [](const auto &left, const auto &right) {
                if (left.startUtc != right.startUtc) return left.startUtc < right.startUtc;
                return left.taskId < right.taskId;
            });
            *chosenStart = startTime;
            return true;
        }
        cursor = nextCursor;
    }
    return false;
}

bool taskDataIsValid(const Task &task, std::optional<QDate> *deadline)
{
    if (task.id <= 0 || task.revision <= 0 || task.title.trimmed().isEmpty()
        || (task.priority != QStringLiteral("low")
            && task.priority != QStringLiteral("medium")
            && task.priority != QStringLiteral("high"))
        || (!task.difficulty.isEmpty() && task.difficulty != QStringLiteral("easy")
            && task.difficulty != QStringLiteral("medium")
            && task.difficulty != QStringLiteral("hard"))) {
        return false;
    }
    if (task.dueDate.isEmpty()) {
        deadline->reset();
        return true;
    }
    const QDate date = QDate::fromString(task.dueDate, Qt::ISODate);
    if (!date.isValid() || date.toString(Qt::ISODate) != task.dueDate) return false;
    *deadline = date;
    return true;
}

QString baseReason(const Candidate &candidate, const ScheduleRecommendation &recommendation)
{
    QString reason;
    if (candidate.previous.plannedDate && recommendation.plannedStartTime
               && recommendation.plannedDate == candidate.previous.plannedDate
               && !candidate.previous.plannedStartTime) {
        reason = QStringLiteral("Kept the previously planned date and assigned the earliest available start time.");
    } else if (recommendation.retainedExistingSchedule && recommendation.plannedStartTime) {
        reason = QStringLiteral("Kept the existing planned work block on %1 at %2.")
                     .arg(recommendation.plannedDate->toString(Qt::ISODate),
                          recommendation.plannedStartTime->toString(QStringLiteral("HH:mm")));
    } else if (recommendation.retainedExistingSchedule) {
        reason = QStringLiteral("Kept the existing planned date; its start time is unspecified.");
    } else if (recommendation.plannedDate && candidate.deadline
               && *recommendation.plannedDate <= *candidate.deadline) {
        reason = QStringLiteral("Recommended for %1 because it is the next available work block before the %2 deadline.")
                     .arg(recommendation.plannedDate->toString(Qt::ISODate),
                          candidate.deadline->toString(Qt::ISODate));
    } else if (recommendation.plannedDate && candidate.deadline) {
        reason = QStringLiteral("Recommended for %1 as the next available work block after earlier capacity filled.")
                     .arg(recommendation.plannedDate->toString(Qt::ISODate));
    } else if (recommendation.plannedDate) {
        reason = QStringLiteral("Recommended for %1 as the next available work block; no deadline is set.")
                     .arg(recommendation.plannedDate->toString(Qt::ISODate));
    }

    if (candidate.priorityRank == 2) reason += QStringLiteral(" High priority increased its score.");
    else if (candidate.priorityRank == 1) reason += QStringLiteral(" Medium priority increased its score.");
    if (candidate.difficultyRank == 3) reason += QStringLiteral(" Hard difficulty added a small tie-break preference.");
    else if (candidate.difficultyRank == 2) reason += QStringLiteral(" Medium difficulty added a small tie-break preference.");
    if (recommendation.usedDefaultDuration) {
        reason += QStringLiteral(" Used the default estimate of %1 minutes.")
                      .arg(recommendation.estimatedDurationMinutes);
    }
    return reason.trimmed();
}

} // namespace

AppError calculateScheduleRecommendations(const SchedulingInput &input,
                                          SchedulingResult *result)
{
    if (!result) return AppError(ErrorCategory::Validation);
    *result = SchedulingResult{};
    if (!input.nowUtc.isValid() || input.planningHorizonDays < 1
        || input.planningHorizonDays > 3650) {
        return AppError(ErrorCategory::Validation);
    }

    SchedulingPreferences preferences = input.preferences;
    if (normalizeSchedulingPreferences(preferences).isError())
        return AppError(ErrorCategory::Validation);
    const QTimeZone zone = input.localTimeZone.isValid()
        ? input.localTimeZone : QTimeZone::utc();
    const QDateTime localNow = input.nowUtc.toUTC().toTimeZone(zone);
    if (!localNow.isValid() || !localNow.date().isValid())
        return AppError(ErrorCategory::Validation);

    const QDate today = localNow.date();
    const QDate horizonEnd = today.addDays(input.planningHorizonDays);
    if (!horizonEnd.isValid()) return AppError(ErrorCategory::Validation);

    QHash<qint64, TaskSchedule> schedulesByTask;
    for (TaskSchedule schedule : input.existingSchedules) {
        if (normalizeTaskSchedule(schedule).isError()
            || schedulesByTask.contains(schedule.taskId)) {
            return AppError(ErrorCategory::Validation);
        }
        schedulesByTask.insert(schedule.taskId, std::move(schedule));
    }

    QVector<Candidate> candidates;
    candidates.reserve(input.incompleteTasks.size());
    QSet<qint64> seenTaskIds;
    for (const Task &task : input.incompleteTasks) {
        if (task.completed) continue;
        std::optional<QDate> deadline;
        if (!taskDataIsValid(task, &deadline) || seenTaskIds.contains(task.id))
            return AppError(ErrorCategory::Validation);
        seenTaskIds.insert(task.id);

        TaskSchedule previous;
        previous.taskId = task.id;
        if (schedulesByTask.contains(task.id)) previous = schedulesByTask.value(task.id);
        if (normalizeTaskSchedule(previous).isError()) return AppError(ErrorCategory::Validation);

        Candidate candidate;
        candidate.task = task;
        candidate.previous = previous;
        candidate.deadline = deadline;
        candidate.usedDefaultDuration = !previous.estimatedDurationMinutes.has_value();
        candidate.durationMinutes = previous.estimatedDurationMinutes.value_or(
            preferences.defaultEstimatedDurationMinutes);
        candidate.previouslyScheduled = previous.plannedDate.has_value();
        candidate.overdue = deadline && *deadline < today;
        candidate.priorityRank = priorityRank(task.priority);
        candidate.difficultyRank = difficultyRank(task.difficulty);
        const int priorityPoints = candidate.priorityRank * 100;
        const int difficultyPoints = candidate.difficultyRank == 3 ? 20
            : candidate.difficultyRank == 2 ? 10 : 0;
        const int previousPlanPoints = candidate.previouslyScheduled ? 10 : 0;
        candidate.score = priorityPoints + difficultyPoints + previousPlanPoints
            + urgencyPoints(deadline, today, candidate.overdue);
        if (candidate.overdue) {
            candidate.warnings.push_back(
                QStringLiteral("The deadline was already overdue on the current local date."));
        }
        if (previous.plannedDate && *previous.plannedDate < today) {
            candidate.warnings.push_back(
                QStringLiteral("The previous planned date was in the past and will be replaced."));
        }
        candidates.push_back(std::move(candidate));
    }

    std::stable_sort(candidates.begin(), candidates.end(), candidateLess);
    result->recommendations.clear();
    result->days.clear();
    result->warnings.clear();
    result->today = today;
    result->planningHorizonDays = input.planningHorizonDays;
    result->recommendations.reserve(candidates.size());

    QHash<qint64, int> candidateIndexById;
    for (int index = 0; index < candidates.size(); ++index) {
        Candidate &candidate = candidates[index];
        candidate.resultIndex = index;
        candidateIndexById.insert(candidate.task.id, index);
        ScheduleRecommendation recommendation;
        recommendation.task = candidate.task;
        recommendation.taskRevision = candidate.task.revision;
        recommendation.scheduleRevision = candidate.previous.revision;
        recommendation.estimatedDurationMinutes = candidate.durationMinutes;
        recommendation.score = candidate.score;
        recommendation.overdue = candidate.overdue;
        recommendation.usedDefaultDuration = candidate.usedDefaultDuration;
        recommendation.previouslyScheduled = candidate.previouslyScheduled;
        recommendation.warnings = candidate.warnings;
        result->recommendations.push_back(std::move(recommendation));
    }

    QMap<QDate, DayState> dayStates;
    QVector<int> fixedCandidates;
    QVector<int> dateOnlyCandidates;
    QVector<int> reallocationCandidates;
    for (int index = 0; index < candidates.size(); ++index) {
        Candidate &candidate = candidates[index];
        const TaskSchedule &previous = candidate.previous;
        if (!previous.plannedDate) {
            reallocationCandidates.push_back(index);
            continue;
        }
        if (*previous.plannedDate < today) {
            reallocationCandidates.push_back(index);
            continue;
        }
        if (!isWorkingDay(preferences, *previous.plannedDate)) {
            candidate.warnings.push_back(
                QStringLiteral("The previous planned date is not a configured working day."));
            reallocationCandidates.push_back(index);
            continue;
        }
        if (candidate.deadline && *candidate.deadline >= today
            && *previous.plannedDate > *candidate.deadline) {
            candidate.warnings.push_back(
                QStringLiteral("The previous plan is after the deadline; an earlier block will be tried."));
            reallocationCandidates.push_back(index);
            continue;
        }
        if (!previous.plannedStartTime) {
            if (candidate.durationMinutes > preferences.maximumPlannedWorkMinutesPerDay) {
                candidate.warnings.push_back(
                    QStringLiteral("The existing date-only plan exceeds the daily capacity."));
                reallocationCandidates.push_back(index);
                continue;
            }
            dateOnlyCandidates.push_back(index);
            continue;
        }

        QDateTime start;
        QDateTime endUtc;
        if (localDateTime(*previous.plannedDate, *previous.plannedStartTime, zone, &start)
            && !(start.date() == today && start < localNow)
            && blockFitsWorkday(start, candidate.durationMinutes, preferences, zone, &endUtc)) {
            fixedCandidates.push_back(index);
        } else {
            candidate.warnings.push_back(
                QStringLiteral("The previous planned time cannot be kept within the current planning window."));
            reallocationCandidates.push_back(index);
        }
    }

    for (const Candidate &candidate : std::as_const(candidates)) {
        ScheduleRecommendation &recommendation = result->recommendations[candidate.resultIndex];
        for (const QString &warning : candidate.warnings) addWarning(recommendation, warning);
    }

    std::sort(fixedCandidates.begin(), fixedCandidates.end(), [&](int left, int right) {
        const Candidate &a = candidates.at(left);
        const Candidate &b = candidates.at(right);
        if (a.previous.plannedDate != b.previous.plannedDate)
            return *a.previous.plannedDate < *b.previous.plannedDate;
        if (a.previous.plannedStartTime != b.previous.plannedStartTime)
            return *a.previous.plannedStartTime < *b.previous.plannedStartTime;
        return a.task.id < b.task.id;
    });

    const int breakMinutes = preferences.breakBetweenWorkBlocksMinutes.value_or(0);
    for (const int index : fixedCandidates) {
        Candidate &candidate = candidates[index];
        ScheduleRecommendation &recommendation = result->recommendations[candidate.resultIndex];
        const QDate date = *candidate.previous.plannedDate;
        QDateTime start;
        QDateTime endUtc;
        localDateTime(date, *candidate.previous.plannedStartTime, zone, &start);
        blockFitsWorkday(start, candidate.durationMinutes, preferences, zone, &endUtc);
        DayState &day = dayStates[date];
        const QDateTime startUtc = start.toUTC();
        for (const OccupiedBlock &existing : std::as_const(day.blocks)) {
            if (!blocksConflict(startUtc, endUtc, existing, breakMinutes)) continue;
            const auto other = candidateIndexById.constFind(existing.taskId);
            const QString otherName = other == candidateIndexById.cend()
                ? QStringLiteral("another planned task") : taskLabel(candidates.at(other.value()));
            recommendation.hasConflict = true;
            addWarning(recommendation,
                       QStringLiteral("The existing work block conflicts with %1.").arg(otherName));
            if (other != candidateIndexById.cend()) {
                ScheduleRecommendation &otherRecommendation =
                    result->recommendations[candidates.at(other.value()).resultIndex];
                otherRecommendation.hasConflict = true;
                addWarning(otherRecommendation,
                           QStringLiteral("The existing work block conflicts with %1.")
                               .arg(taskLabel(candidate)));
            }
        }
        day.blocks.push_back({candidate.task.id, startUtc, endUtc});
        day.plannedMinutes += candidate.durationMinutes;
        day.taskIds.push_back(candidate.task.id);
        recommendation.state = RecommendationState::Scheduled;
        recommendation.plannedDate = date;
        recommendation.plannedStartTime = candidate.previous.plannedStartTime;
        recommendation.retainedExistingSchedule = true;
        recommendation.reason = baseReason(candidate, recommendation);
        setOverdueWarning(recommendation);
        setDeadlineWarning(recommendation, candidate.deadline);
    }

    for (const int index : dateOnlyCandidates) {
        Candidate &candidate = candidates[index];
        ScheduleRecommendation &recommendation = result->recommendations[candidate.resultIndex];
        DayState &day = dayStates[*candidate.previous.plannedDate];
        day.hasUnspecifiedStartTime = true;
        day.plannedMinutes += candidate.durationMinutes;
        day.taskIds.push_back(candidate.task.id);
        recommendation.state = RecommendationState::Scheduled;
        recommendation.plannedDate = candidate.previous.plannedDate;
        recommendation.retainedExistingSchedule = true;
        recommendation.reason = baseReason(candidate, recommendation);
        addWarning(recommendation,
                   QStringLiteral("The existing plan has no start time; no other work was placed on this day."));
        setOverdueWarning(recommendation);
        setDeadlineWarning(recommendation, candidate.deadline);
    }

    for (auto dayIt = dayStates.begin(); dayIt != dayStates.end(); ++dayIt) {
        DayState &day = dayIt.value();
        if (day.plannedMinutes > preferences.maximumPlannedWorkMinutesPerDay) {
            for (const qint64 taskId : std::as_const(day.taskIds)) {
                const auto candidate = candidateIndexById.constFind(taskId);
                if (candidate == candidateIndexById.cend()) continue;
                ScheduleRecommendation &recommendation = result->recommendations[
                    candidates.at(candidate.value()).resultIndex];
                recommendation.overloaded = true;
                addWarning(recommendation,
                           QStringLiteral("Existing scheduled work exceeds the configured daily capacity on %1.")
                               .arg(dayIt.key().toString(Qt::ISODate)));
            }
        }
        if (day.hasUnspecifiedStartTime && day.taskIds.size() > 1) {
            for (const qint64 taskId : std::as_const(day.taskIds)) {
                const auto candidate = candidateIndexById.constFind(taskId);
                if (candidate == candidateIndexById.cend()) continue;
                ScheduleRecommendation &recommendation = result->recommendations[
                    candidates.at(candidate.value()).resultIndex];
                recommendation.hasConflict = true;
                addWarning(recommendation,
                           QStringLiteral("A date-only existing plan may conflict with other work on this day."));
            }
        }
    }

    for (const int index : reallocationCandidates) {
        Candidate &candidate = candidates[index];
        ScheduleRecommendation &recommendation = result->recommendations[candidate.resultIndex];
        if (candidate.durationMinutes > preferences.maximumPlannedWorkMinutesPerDay
            || candidate.durationMinutes > preferences.workdayStart.msecsTo(preferences.workdayEnd) / 60000) {
            recommendation.reason = QStringLiteral(
                "Cannot schedule this task as one work block: its %1-minute estimate exceeds the configured single-block capacity.")
                .arg(candidate.durationMinutes);
            addWarning(recommendation,
                       QStringLiteral("This scheduler does not split one task across multiple work blocks."));
            continue;
        }
        if (preferences.preferredWorkingDays.isEmpty()) {
            recommendation.reason = QStringLiteral("Cannot schedule: no working days are enabled.");
            continue;
        }

        std::optional<QDate> preferredDate;
        if (candidate.previous.plannedDate && *candidate.previous.plannedDate >= today
            && *candidate.previous.plannedDate <= horizonEnd
            && isWorkingDay(preferences, *candidate.previous.plannedDate)
            && (!candidate.deadline || *candidate.deadline < today
                || *candidate.previous.plannedDate <= *candidate.deadline)) {
            preferredDate = candidate.previous.plannedDate;
        }

        bool placed = false;
        bool earlierCapacityWasInsufficient = false;
        QDate selectedDate;
        QTime selectedStart;
        auto attempt = [&](QDate date) {
            if (!isWorkingDay(preferences, date)) return false;
            if (tryPlace(candidate, date, today, localNow, zone, preferences,
                         dayStates, &selectedStart)) {
                selectedDate = date;
                return true;
            }
            earlierCapacityWasInsufficient = true;
            return false;
        };

        if (preferredDate) placed = attempt(*preferredDate);
        for (int offset = 0; !placed && offset <= input.planningHorizonDays; ++offset) {
            const QDate date = today.addDays(offset);
            if (preferredDate && date == *preferredDate) continue;
            if (attempt(date)) placed = true;
        }

        if (!placed) {
            recommendation.reason = QStringLiteral(
                "No available work block was found within the %1-day planning horizon.")
                .arg(input.planningHorizonDays);
            if (candidate.deadline && *candidate.deadline >= today) {
                addWarning(recommendation,
                           QStringLiteral("Current capacity cannot fit this task before its deadline."));
            }
            continue;
        }

        recommendation.state = RecommendationState::Scheduled;
        recommendation.plannedDate = selectedDate;
        recommendation.plannedStartTime = selectedStart;
        recommendation.retainedExistingSchedule = false;
        if (candidate.previous.plannedDate && *candidate.previous.plannedDate > selectedDate) {
            addWarning(recommendation,
                       QStringLiteral("Moved earlier than the previous plan to meet the deadline sooner."));
        } else if (candidate.previous.plannedDate && *candidate.previous.plannedDate != selectedDate) {
            addWarning(recommendation,
                       QStringLiteral("The previous planned date had insufficient capacity or no longer fit the work settings."));
        } else if (earlierCapacityWasInsufficient && selectedDate > today) {
            addWarning(recommendation,
                       QStringLiteral("Moved later because earlier working days had insufficient available capacity."));
        }
        recommendation.reason = baseReason(candidate, recommendation);
        setOverdueWarning(recommendation);
        setDeadlineWarning(recommendation, candidate.deadline);
    }

    if (preferences.preferredWorkingDays.isEmpty() && !candidates.isEmpty()) {
        result->warnings.push_back(QStringLiteral("No working days are enabled; unscheduled tasks cannot be placed."));
    }

    QSet<QDate> summaryDates;
    for (int offset = 0; offset <= input.planningHorizonDays; ++offset) {
        const QDate date = today.addDays(offset);
        if (isWorkingDay(preferences, date)) summaryDates.insert(date);
    }
    for (auto it = dayStates.cbegin(); it != dayStates.cend(); ++it) summaryDates.insert(it.key());
    QList<QDate> orderedDates = summaryDates.values();
    std::sort(orderedDates.begin(), orderedDates.end());
    for (const QDate date : std::as_const(orderedDates)) {
        const DayState day = dayStates.value(date);
        const bool overloaded = day.plannedMinutes > preferences.maximumPlannedWorkMinutesPerDay;
        result->days.push_back({date,
                                day.plannedMinutes,
                                preferences.maximumPlannedWorkMinutesPerDay,
                                std::max(0, preferences.maximumPlannedWorkMinutesPerDay
                                                - day.plannedMinutes),
                                overloaded,
                                day.hasUnspecifiedStartTime});
        if (overloaded) {
            result->warnings.push_back(
                QStringLiteral("Existing work overloads %1.").arg(date.toString(Qt::ISODate)));
        }
    }
    return AppError::none();
}

} // namespace something::domain
