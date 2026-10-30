#include "scheduling_service.h"

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
