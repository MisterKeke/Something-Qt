#include "steam_news_provider.h"

#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>

namespace something::providers {
namespace {

constexpr int kSteamNewsPageSize = 20;
constexpr int kSteamNewsMaximumLength = 6000;

} // namespace

SteamNewsProviderRequest makeSteamNewsProviderRequest(
    quint32 appId, std::optional<qint64> olderThanUnixSeconds)
{
    SteamNewsProviderRequest result;
    if (appId == 0) {
        result.error = something::domain::AppError(
            something::domain::ErrorCategory::Validation);
        return result;
    }

    if (olderThanUnixSeconds) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (*olderThanUnixSeconds <= 0 || *olderThanUnixSeconds > now) {
            result.error = something::domain::AppError(
                something::domain::ErrorCategory::Validation);
            return result;
        }
    }

    QUrl url(QStringLiteral(
        "https://api.steampowered.com/ISteamNews/GetNewsForApp/v2/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appid"), QString::number(appId));
    query.addQueryItem(QStringLiteral("count"), QString::number(kSteamNewsPageSize));
    query.addQueryItem(QStringLiteral("maxlength"), QString::number(kSteamNewsMaximumLength));
    if (olderThanUnixSeconds) {
        query.addQueryItem(QStringLiteral("enddate"), QString::number(*olderThanUnixSeconds));
    }
    url.setQuery(query);

    result.appId = appId;
    result.olderThanUnixSeconds = olderThanUnixSeconds;
    result.providerRequest.provider = ProviderId::SteamNews;
    result.providerRequest.url = url;
    // A manual update check should not be satisfied by a cached latest page.
    result.providerRequest.bypassCache = !olderThanUnixSeconds.has_value();
    return result;
}

} // namespace something::providers
