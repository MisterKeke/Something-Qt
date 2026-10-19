#include "dashboard_service.h"

#include "favorites_service.h"

namespace something::services {

storage::StorageCommand DashboardService::recentFavoriteNews(int limit)
{
    return FavoritesService::recentNews(limit);
}

} // namespace something::services
