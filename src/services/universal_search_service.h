#pragma once

#include "../storage/storage_worker.h"

namespace something::services {

class UniversalSearchService final {
public:
    static storage::StorageCommand search(QString query);
};

} // namespace something::services
