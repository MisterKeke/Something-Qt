#pragma once

#include "network_worker.h"
#include "../domain/app_error.h"

#include <QtGlobal>

#include <optional>

namespace something::providers {

struct SteamNewsProviderRequest final {
    quint32 appId{0};
    std::optional<qint64> olderThanUnixSeconds;
    ProviderRequest providerRequest;
    something::domain::AppError error;

    bool succeeded() const noexcept
    {
        return appId != 0 && !error.isError() && providerRequest.url.isValid();
    }
};

// Builds a fixed GetNewsForApp request. A missing cursor checks the latest
// items; a valid cursor requests older items using Steam's enddate parameter.
// No caller-supplied URL or feed selector reaches the network boundary.
SteamNewsProviderRequest makeSteamNewsProviderRequest(
    quint32 appId, std::optional<qint64> olderThanUnixSeconds = std::nullopt);

} // namespace something::providers
