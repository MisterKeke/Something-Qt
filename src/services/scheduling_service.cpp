#include "scheduling_service.h"

#include "task_service.h"

#include <QDate>
#include <QTime>
#include <QVariantList>
#include <QVariant>

#include <utility>

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

storage::StorageReply savedScheduleReply(storage::RepositoryResult<domain::TaskSchedule> result)
{
    if (!result.succeeded()) return failed(result.error);
    storage::StorageReply reply;
    reply.values.insert(QStringLiteral("schedule"),
                        SchedulingService::toVariantMap(result.value));
    return reply;
}

storage::StorageReply savedPreferencesReply(
    storage::RepositoryResult<domain::SchedulingPreferences> result)
{
    if (!result.succeeded()) return failed(result.error);
    storage::StorageReply reply;
    reply.values.insert(QStringLiteral("preferences"),
                        SchedulingService::toVariantMap(result.value));
    return reply;
}

} // namespace

QVariantMap SchedulingService::toVariantMap(const domain::TaskSchedule &schedule)
{
    return {
        {QStringLiteral("taskId"), schedule.taskId},
        {QStringLiteral("plannedDate"), schedule.plannedDate
                 ? schedule.plannedDate->toString(Qt::ISODate) : QString{}},
        {QStringLiteral("plannedStartTime"), schedule.plannedStartTime
                 ? schedule.plannedStartTime->toString(QStringLiteral("HH:mm")) : QString{}},
        {QStringLiteral("estimatedDurationMinutes"), schedule.estimatedDurationMinutes
                 ? QVariant(*schedule.estimatedDurationMinutes) : QVariant{}},
        {QStringLiteral("revision"), schedule.revision},
        {QStringLiteral("updatedAt"), schedule.updatedAt},
    };
}

QVariantMap SchedulingService::toVariantMap(const domain::SchedulingPreferences &preferences)
{
    QVariantList workingDays;
    workingDays.reserve(preferences.preferredWorkingDays.size());
    for (const int day : preferences.preferredWorkingDays) workingDays.push_back(day);
    return {
        {QStringLiteral("preferredWorkingDays"), workingDays},
        {QStringLiteral("workdayStart"), preferences.workdayStart.toString(QStringLiteral("HH:mm"))},
        {QStringLiteral("workdayEnd"), preferences.workdayEnd.toString(QStringLiteral("HH:mm"))},
        {QStringLiteral("maximumPlannedWorkMinutesPerDay"),
         preferences.maximumPlannedWorkMinutesPerDay},
        {QStringLiteral("defaultEstimatedDurationMinutes"),
         preferences.defaultEstimatedDurationMinutes},
        {QStringLiteral("breakBetweenWorkBlocksMinutes"),
         preferences.breakBetweenWorkBlocksMinutes
             ? QVariant(*preferences.breakBetweenWorkBlocksMinutes) : QVariant{}},
        {QStringLiteral("revision"), preferences.revision},
        {QStringLiteral("updatedAt"), preferences.updatedAt},
    };
}

QVariantMap SchedulingService::toVariantMap(const domain::SchedulingResult &result)
{
    QVariantList recommendations;
    recommendations.reserve(result.recommendations.size());
    for (const domain::ScheduleRecommendation &recommendation : result.recommendations) {
        QVariantList warnings;
        warnings.reserve(recommendation.warnings.size());
        for (const QString &warning : recommendation.warnings) warnings.push_back(warning);
        const bool canAccept = recommendation.state == domain::RecommendationState::Scheduled
            && !recommendation.hasConflict && !recommendation.overloaded;
        recommendations.push_back(QVariantMap{
            {QStringLiteral("taskId"), recommendation.task.id},
            {QStringLiteral("title"), recommendation.task.title},
            {QStringLiteral("dueDate"), recommendation.task.dueDate},
            {QStringLiteral("priority"), recommendation.task.priority},
            {QStringLiteral("difficulty"), recommendation.task.difficulty},
            {QStringLiteral("estimatedDurationMinutes"), recommendation.estimatedDurationMinutes},
            {QStringLiteral("plannedDate"), recommendation.plannedDate
                 ? recommendation.plannedDate->toString(Qt::ISODate) : QString{}},
            {QStringLiteral("plannedStartTime"), recommendation.plannedStartTime
                 ? recommendation.plannedStartTime->toString(QStringLiteral("HH:mm")) : QString{}},
            {QStringLiteral("score"), recommendation.score},
            {QStringLiteral("state"), recommendation.state == domain::RecommendationState::Scheduled
                 ? QStringLiteral("scheduled") : QStringLiteral("unscheduled")},
            {QStringLiteral("overdue"), recommendation.overdue},
            {QStringLiteral("usedDefaultDuration"), recommendation.usedDefaultDuration},
            {QStringLiteral("previouslyScheduled"), recommendation.previouslyScheduled},
            {QStringLiteral("retainedExistingSchedule"), recommendation.retainedExistingSchedule},
            {QStringLiteral("hasConflict"), recommendation.hasConflict},
            {QStringLiteral("overloaded"), recommendation.overloaded},
            {QStringLiteral("reason"), recommendation.reason},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("taskRevision"), recommendation.taskRevision},
            {QStringLiteral("scheduleRevision"), recommendation.scheduleRevision},
            {QStringLiteral("canAccept"), canAccept},
            {QStringLiteral("accepted"), false},
        });
    }

    QVariantList days;
    days.reserve(result.days.size());
    for (const domain::SchedulingDaySummary &day : result.days) {
        days.push_back(QVariantMap{
            {QStringLiteral("date"), day.date.toString(Qt::ISODate)},
            {QStringLiteral("plannedMinutes"), day.plannedMinutes},
            {QStringLiteral("capacityMinutes"), day.capacityMinutes},
            {QStringLiteral("remainingMinutes"), day.remainingMinutes},
            {QStringLiteral("overloaded"), day.overloaded},
            {QStringLiteral("hasUnspecifiedStartTime"), day.hasUnspecifiedStartTime},
        });
    }
    return {
        {QStringLiteral("recommendations"), recommendations},
        {QStringLiteral("days"), days},
        {QStringLiteral("warnings"), result.warnings},
        {QStringLiteral("today"), result.today.toString(Qt::ISODate)},
        {QStringLiteral("planningHorizonDays"), result.planningHorizonDays},
    };
}

domain::AppError SchedulingService::taskScheduleFromVariantMap(
    const QVariantMap &map, domain::TaskSchedule *schedule)
{
    if (!schedule) return domain::AppError(domain::ErrorCategory::Validation);
    domain::TaskSchedule value;
    value.taskId = map.value(QStringLiteral("taskId")).toLongLong();
    const QString dateText = map.value(QStringLiteral("plannedDate")).toString();
    if (!dateText.isEmpty()) {
        const QDate date = QDate::fromString(dateText, Qt::ISODate);
        if (!date.isValid() || date.toString(Qt::ISODate) != dateText)
            return domain::AppError(domain::ErrorCategory::Validation);
        value.plannedDate = date;
    }
    const QString timeText = map.value(QStringLiteral("plannedStartTime")).toString();
    if (!timeText.isEmpty()) {
        const QTime time = QTime::fromString(timeText, QStringLiteral("HH:mm"));
        if (!time.isValid() || time.toString(QStringLiteral("HH:mm")) != timeText)
            return domain::AppError(domain::ErrorCategory::Validation);
        value.plannedStartTime = time;
    }
    const QVariant estimate = map.value(QStringLiteral("estimatedDurationMinutes"));
    if (estimate.isValid() && !estimate.isNull()) value.estimatedDurationMinutes = estimate.toInt();
    value.revision = map.value(QStringLiteral("revision")).toLongLong();
    value.updatedAt = map.value(QStringLiteral("updatedAt")).toString();
    const domain::AppError error = domain::normalizeTaskSchedule(value);
    if (error.isError()) return error;
    *schedule = std::move(value);
    return {};
}

domain::AppError SchedulingService::preferencesFromVariantMap(
    const QVariantMap &map, domain::SchedulingPreferences *preferences)
{
    if (!preferences) return domain::AppError(domain::ErrorCategory::Validation);
    domain::SchedulingPreferences value;
    value.preferredWorkingDays.clear();
    const QVariantList days = map.value(QStringLiteral("preferredWorkingDays")).toList();
    value.preferredWorkingDays.reserve(days.size());
    for (const QVariant &day : days) value.preferredWorkingDays.push_back(day.toInt());

    const QString startText = map.value(QStringLiteral("workdayStart")).toString();
    const QString endText = map.value(QStringLiteral("workdayEnd")).toString();
    value.workdayStart = QTime::fromString(startText, QStringLiteral("HH:mm"));
    value.workdayEnd = QTime::fromString(endText, QStringLiteral("HH:mm"));
    if (!value.workdayStart.isValid() || !value.workdayEnd.isValid()
        || value.workdayStart.toString(QStringLiteral("HH:mm")) != startText
        || value.workdayEnd.toString(QStringLiteral("HH:mm")) != endText) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    value.maximumPlannedWorkMinutesPerDay =
        map.value(QStringLiteral("maximumPlannedWorkMinutesPerDay")).toInt();
    value.defaultEstimatedDurationMinutes =
        map.value(QStringLiteral("defaultEstimatedDurationMinutes")).toInt();
    const QVariant breakValue = map.value(QStringLiteral("breakBetweenWorkBlocksMinutes"));
    if (breakValue.isValid() && !breakValue.isNull()) value.breakBetweenWorkBlocksMinutes = breakValue.toInt();
    value.revision = map.value(QStringLiteral("revision")).toLongLong();
    value.updatedAt = map.value(QStringLiteral("updatedAt")).toString();
    const domain::AppError error = domain::normalizeSchedulingPreferences(value);
    if (error.isError()) return error;
    *preferences = std::move(value);
    return {};
}

storage::StorageCommand SchedulingService::loadPreviewSnapshot()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        QVariantList tasks;
        storage::TaskFilter filter;
        filter.completion = QStringLiteral("incomplete");
        filter.limit = 200;
        for (int offset = 0; ; offset += filter.limit) {
            if (cancelled()) return failed(domain::AppError::cancelled());
            filter.offset = offset;
            const auto result = repositories.tasks.list(filter);
            if (!result.succeeded()) return failed(result.error);
            for (const storage::Task &task : result.value)
                tasks.push_back(TaskService::toVariantMap(task));
            if (result.value.size() < filter.limit) break;
            if (offset > 50000000 - filter.limit)
                return failed(domain::AppError(domain::ErrorCategory::Validation));
        }

        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto scheduleResult = repositories.scheduling.taskSchedules();
        if (!scheduleResult.succeeded()) return failed(scheduleResult.error);
        QVariantList schedules;
        schedules.reserve(scheduleResult.value.size());
        for (const domain::TaskSchedule &schedule : scheduleResult.value)
            schedules.push_back(SchedulingService::toVariantMap(schedule));

        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto preferenceResult = repositories.scheduling.schedulingPreferences();
        if (!preferenceResult.succeeded()) return failed(preferenceResult.error);

        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("tasks"), tasks);
        reply.values.insert(QStringLiteral("schedules"), schedules);
        reply.values.insert(QStringLiteral("preferences"),
                            SchedulingService::toVariantMap(preferenceResult.value));
        return reply;
    };
}

storage::StorageCommand SchedulingService::loadTaskSchedules()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto result = repositories.scheduling.taskSchedules();
        if (!result.succeeded()) return failed(result.error);
        QVariantList rows;
        rows.reserve(result.value.size());
        for (const auto &schedule : result.value)
            rows.push_back(SchedulingService::toVariantMap(schedule));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("schedules"), rows);
        return reply;
    };
}

storage::StorageCommand SchedulingService::loadTaskSchedule(qint64 taskId)
{
    return [taskId](storage::StorageRepositories &repositories,
                    const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        return savedScheduleReply(repositories.scheduling.taskSchedule(taskId));
    };
}

storage::StorageCommand SchedulingService::saveTaskSchedule(domain::TaskSchedule schedule,
                                                            qint64 expectedRevision)
{
    if (schedule.revision != expectedRevision || expectedRevision < 0
        || domain::normalizeTaskSchedule(schedule).isError()) {
        return [](storage::StorageRepositories &, const std::function<bool()> &) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        };
    }
    return [schedule = std::move(schedule), expectedRevision](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        return savedScheduleReply(
            repositories.scheduling.setTaskSchedule(schedule, expectedRevision));
    };
}

storage::StorageCommand SchedulingService::loadPreferences()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        return savedPreferencesReply(repositories.scheduling.schedulingPreferences());
    };
}

storage::StorageCommand SchedulingService::savePreferences(
    domain::SchedulingPreferences preferences, qint64 expectedRevision)
{
    if (preferences.revision != expectedRevision || expectedRevision <= 0
        || domain::normalizeSchedulingPreferences(preferences).isError()) {
        return [](storage::StorageRepositories &, const std::function<bool()> &) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        };
    }
    return [preferences = std::move(preferences), expectedRevision](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        return savedPreferencesReply(repositories.scheduling.setSchedulingPreferences(
            preferences, expectedRevision));
    };
}

} // namespace something::services
