#pragma once

#include "../domain/task_scheduling.h"
#include "../domain/scheduling_engine.h"
#include "../storage/storage_worker.h"

namespace something::services {

// Storage command factories for scheduling metadata and profile preferences.
// The returned maps contain detached values suitable for crossing the
// storage-thread boundary.
class SchedulingService final {
public:
    static storage::StorageCommand loadTaskSchedules();
    static storage::StorageCommand loadTaskSchedule(qint64 taskId);
    static storage::StorageCommand loadPreviewSnapshot();
    static storage::StorageCommand applyAcceptedSchedules(
        QVector<domain::TaskScheduleProposal> proposals,
        QVector<domain::TaskScheduleSnapshot> taskSnapshot,
        qint64 expectedPreferencesRevision);
    static storage::StorageCommand saveTaskSchedule(domain::TaskSchedule schedule,
                                                    qint64 expectedRevision);
    static storage::StorageCommand loadPreferences();
    static storage::StorageCommand savePreferences(domain::SchedulingPreferences preferences,
                                                   qint64 expectedRevision);

    static QVariantMap toVariantMap(const domain::TaskSchedule &schedule);
    static QVariantMap toVariantMap(const domain::SchedulingPreferences &preferences);
    static QVariantMap toVariantMap(const domain::SchedulingResult &result);
    static domain::AppError taskScheduleFromVariantMap(const QVariantMap &map,
                                                       domain::TaskSchedule *schedule);
    static domain::AppError preferencesFromVariantMap(
        const QVariantMap &map, domain::SchedulingPreferences *preferences);
};

} // namespace something::services
