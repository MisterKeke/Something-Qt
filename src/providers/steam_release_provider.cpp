#include "steam_release_provider.h"

#include "../domain/steam_game.h"

#include <QUrl>
#include <QUrlQuery>

#include <utility>

namespace something::providers {

SteamReleaseProviderRequest makeSteamReleaseProviderRequest(QString storeUrl)
{
    SteamReleaseProviderRequest result;
    const something::domain::SteamStoreUrl normalized =
        something::domain::normalizeSteamStoreUrl(std::move(storeUrl));
    if (!normalized.isValid()) {
        result.error = normalized.error;
        return result;
    }

    QUrl url(QStringLiteral("https://store.steampowered.com/api/appdetails"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appids"), QString::number(normalized.appId));
    // Pin language independently of the price tracker's user-selected country.
    query.addQueryItem(QStringLiteral("cc"), QStringLiteral("us"));
    query.addQueryItem(QStringLiteral("l"), QStringLiteral("english"));
    url.setQuery(query);

    result.appId = normalized.appId;
    result.canonicalStoreUrl = normalized.canonicalUrl;
    result.providerRequest = {ProviderId::SteamStore, url};
    return result;
}

} // namespace something::providers
