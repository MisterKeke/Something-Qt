#pragma once

#include "steam_game_catalog_provider.h"

namespace something::providers {

using SteamReleaseProviderRequest = SteamGameCatalogProviderRequest;

// Compatibility wrapper retained for existing Release Radar callers.
SteamReleaseProviderRequest makeSteamReleaseProviderRequest(QString storeUrl);

} // namespace something::providers
