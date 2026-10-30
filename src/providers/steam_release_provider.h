#pragma once

#include "network_worker.h"
#include "../domain/app_error.h"

#include <QString>
#include <QtGlobal>

namespace something::providers {

struct SteamReleaseProviderRequest final {
    quint32 appId{0};
    QString canonicalStoreUrl;
    ProviderRequest providerRequest;
    something::domain::AppError error;

    bool succeeded() const noexcept
    {
        return appId != 0 && !error.isError() && providerRequest.url.isValid();
    }
};

// Builds a fixed-locale appdetails request after validating and canonicalizing
// the supplied Steam Store URL. The request uses the existing SteamStore
// host/path policy and does not read price-tracker settings.
SteamReleaseProviderRequest makeSteamReleaseProviderRequest(QString storeUrl);

} // namespace something::providers
