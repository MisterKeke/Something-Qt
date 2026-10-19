#pragma once

#include "../storage/storage_worker.h"

namespace something::services {

class PreferencesService final {
public:
    static storage::StorageCommand loadWeekStart();
    static storage::StorageCommand setWeekStart(int sundayZero);
};

} // namespace something::services
