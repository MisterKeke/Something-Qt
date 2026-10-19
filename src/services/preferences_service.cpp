#include "preferences_service.h"

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

} // namespace

storage::StorageCommand PreferencesService::loadWeekStart()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto result = repositories.preferences.weekStart();
        if (!result.succeeded()) return failed(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("weekStart"), result.value);
        return reply;
    };
}

storage::StorageCommand PreferencesService::setWeekStart(int sundayZero)
{
    return [sundayZero](storage::StorageRepositories &repositories,
                        const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto result = repositories.preferences.setWeekStart(sundayZero);
        if (!result.succeeded()) return failed(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("weekStart"), sundayZero);
        return reply;
    };
}

} // namespace something::services
