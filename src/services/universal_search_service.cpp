#include "universal_search_service.h"

#include <utility>

namespace something::services {

storage::StorageCommand UniversalSearchService::search(QString query)
{
    return [query = std::move(query)](storage::StorageRepositories &repositories,
                                      const std::function<bool()> &cancelled) {
        storage::StorageReply reply;
        const auto result = repositories.universalSearch.search(query, cancelled);
        reply.error = result.error;
        if (result.succeeded())
            reply.values.insert(QStringLiteral("results"), result.value);
        return reply;
    };
}

} // namespace something::services
