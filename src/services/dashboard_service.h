#pragma once

#include "../storage/storage_worker.h"

namespace something::services {

// Offline Dashboard read path for already-persisted favorite-update snapshots.
// Provider scans are attached by the Favorites/provider feature stage.
class DashboardService final {
public:
    static storage::StorageCommand recentFavoriteNews(int limit = 60);
};

} // namespace something::services
