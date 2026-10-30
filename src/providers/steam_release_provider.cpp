#include "steam_release_provider.h"

#include <utility>

namespace something::providers {

SteamReleaseProviderRequest makeSteamReleaseProviderRequest(QString storeUrl)
{
    return makeSteamGameCatalogProviderRequest(std::move(storeUrl));
}

} // namespace something::providers
