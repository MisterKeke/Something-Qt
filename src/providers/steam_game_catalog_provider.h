#pragma once

#include "network_worker.h"
#include "../domain/app_error.h"

#include <QString>
#include <QtGlobal>

namespace something::providers {

struct SteamGameCatalogProviderRequest final {
    quint32 appId{0};
    QString canonicalStoreUrl;
    ProviderRequest providerRequest;
    something::domain::AppError error;

    bool succeeded() const noexcept
    {
        return appId != 0 && !error.isError() && providerRequest.url.isValid();
    }
};

// Builds the fixed-locale Store details request from a validated Steam URL.
// It does not depend on price-tracker settings or feature membership.
SteamGameCatalogProviderRequest makeSteamGameCatalogProviderRequest(QString storeUrl);

} // namespace something::providers
