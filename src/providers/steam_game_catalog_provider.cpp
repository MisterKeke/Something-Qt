#include "steam_game_catalog_provider.h"

#include "../domain/steam_game.h"

#include <QUrl>
#include <QUrlQuery>

#include <utility>

namespace something::providers {

SteamGameCatalogProviderRequest makeSteamGameCatalogProviderRequest(QString storeUrl)
{
    SteamGameCatalogProviderRequest result;
    const something::domain::SteamStoreUrl normalized =
        something::domain::normalizeSteamStoreUrl(std::move(storeUrl));
    if (!normalized.isValid()) {
        result.error = normalized.error;
        return result;
    }

    QUrl url(QStringLiteral("https://store.steampowered.com/api/appdetails"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appids"), QString::number(normalized.appId));
    query.addQueryItem(QStringLiteral("cc"), QStringLiteral("us"));
    query.addQueryItem(QStringLiteral("l"), QStringLiteral("english"));
    url.setQuery(query);

    result.appId = normalized.appId;
    result.canonicalStoreUrl = normalized.canonicalUrl;
    result.providerRequest = {ProviderId::SteamStore, url};
    return result;
}

} // namespace something::providers
