#pragma once

#include "../storage/storage_worker.h"
#include "../storage/repository_types.h"

#include <QVariantList>
#include <QVariantMap>

#include <optional>

namespace something::services {

class TaskHistoryService final {
public:
    static storage::StorageCommand forTask(
        qint64 taskId, std::optional<domain::TaskHistoryEventType> eventType,
        int pageSize, storage::TaskHistoryCursor before = {});
    static storage::StorageCommand recent(
        QString taskTitleQuery, std::optional<domain::TaskHistoryEventType> eventType,
        int pageSize, storage::TaskHistoryCursor before = {});

    static QVariantMap toVariantMap(const domain::TaskHistoryEvent &event);
};

} // namespace something::services
