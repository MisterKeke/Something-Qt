#pragma once

#include "../storage/storage_worker.h"

#include <QDate>

namespace something::services {

class CalendarService final {
public:
    static storage::StorageCommand dateWithSchedules(QDate date,
                                                     bool includeGameReleases);
    static storage::StorageCommand year(int year, bool includeGameReleases);
};

} // namespace something::services
