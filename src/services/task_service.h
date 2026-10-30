#pragma once

#include "../storage/storage_worker.h"

#include <QDate>
#include <QVariantMap>

namespace something::services {

// Owns task use-case dispatch and the detached value boundary between the
// storage worker and GUI-thread models/controllers.
class TaskService final {
public:
    static storage::StorageCommand list(storage::TaskFilter filter, int pageSize);
    static storage::StorageCommand calendarDate(QDate date);
    static storage::StorageCommand calendarDateWithSchedules(QDate date);
    static storage::StorageCommand calendarYear(int year);
    static storage::StorageCommand calendarDatePage(QDate date, int offset, int pageSize = 50);
    static storage::StorageCommand dashboardToday(QDate today,
                                                  bool includeOverdue = true,
                                                  bool includeUndated = false);
    static storage::StorageCommand dashboardWeek(QDate today,
                                                 bool includeOverdue = false,
                                                 bool includeUndated = false);
    static storage::StorageCommand get(qint64 id);
    static storage::StorageCommand create(storage::TaskInput input);
    static storage::StorageCommand update(qint64 id, qint64 expectedRevision,
                                          storage::TaskInput input);
    static storage::StorageCommand setCompleted(qint64 id, qint64 expectedRevision,
                                                bool completed);
    static storage::StorageCommand setSubtaskCompleted(qint64 id, qint64 subtaskId,
                                                       qint64 expectedRevision,
                                                       bool completed);
    static storage::StorageCommand remove(qint64 id, qint64 expectedRevision);

    static QVariantMap toVariantMap(const storage::Task &task);
    static storage::Task fromVariantMap(const QVariantMap &map);
};

} // namespace something::services
