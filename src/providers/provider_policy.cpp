#include "provider_policy.h"

#include <algorithm>
#include <utility>

namespace something::providers {
namespace {

ProviderPolicy commonPolicy(QStringList hosts,
                            QStringList paths,
                            QByteArray accept,
                            QByteArray userAgent,
                            QByteArray language = {})
{
    ProviderPolicy policy;
    policy.exactHosts = std::move(hosts);
    policy.exactPaths = std::move(paths);
    policy.accept = std::move(accept);
    policy.userAgent = std::move(userAgent);
    policy.acceptLanguage = std::move(language);
    return policy;
}

bool containsCaseInsensitive(const QStringList &values, const QString &value)
{
    return std::any_of(values.cbegin(), values.cend(), [&value](const QString &candidate) {
        return candidate.compare(value, Qt::CaseInsensitive) == 0;
    });
}

bool matchesHostSuffix(const QString &host, const QString &suffix)
{
    const QString normalized = suffix.toLower();
    return host == normalized || host.endsWith(QLatin1Char('.') + normalized);
}

} // namespace

ProviderPolicy providerPolicy(ProviderId provider)
{
    static const QByteArray browserAgent = QByteArrayLiteral(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    static const QByteArray appAgent = QByteArrayLiteral("Something/1.0");

    switch (provider) {
    case ProviderId::Telegram: {
        auto policy = commonPolicy({QStringLiteral("t.me")}, {},
                                   QByteArrayLiteral("text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8"),
                                   browserAgent, QByteArrayLiteral("en-US,en;q=0.5"));
        policy.pathPrefixes = {QStringLiteral("/s/")};
        return policy;
    }
    case ProviderId::YouTubePage: {
        auto policy = commonPolicy({QStringLiteral("www.youtube.com")}, {},
                                   QByteArrayLiteral("text/html,application/xhtml+xml"),
                                   browserAgent, QByteArrayLiteral("en-US,en;q=0.9"));
        policy.pathPrefixes = {QStringLiteral("/@"), QStringLiteral("/channel/")};
        return policy;
    }
    case ProviderId::YouTubeFeed:
        return commonPolicy({QStringLiteral("www.youtube.com")},
                            {QStringLiteral("/feeds/videos.xml")},
                            QByteArrayLiteral("application/xml, text/xml, */*;q=0.8"),
                            browserAgent);
    case ProviderId::OpenMeteoForecast:
        return commonPolicy({QStringLiteral("api.open-meteo.com")},
                            {QStringLiteral("/v1/forecast")},
                            QByteArrayLiteral("application/json"), appAgent);
    case ProviderId::OpenMeteoGeocoding:
        return commonPolicy({QStringLiteral("geocoding-api.open-meteo.com")},
                            {QStringLiteral("/v1/search")},
                            QByteArrayLiteral("application/json"), appAgent);
    case ProviderId::Frankfurter: {
        auto policy = commonPolicy({QStringLiteral("api.frankfurter.dev")},
                                   {QStringLiteral("/v2/rates")},
                                   QByteArrayLiteral("application/json"), appAgent);
        policy.pathPrefixes = {QStringLiteral("/v2/rate/")};
        return policy;
    }
    case ProviderId::SteamStore:
        return commonPolicy({QStringLiteral("store.steampowered.com")},
                            {QStringLiteral("/api/appdetails")},
                            QByteArrayLiteral("application/json"), appAgent);
    case ProviderId::SteamNews: {
        auto policy = commonPolicy({QStringLiteral("api.steampowered.com")},
                                   {QStringLiteral("/ISteamNews/GetNewsForApp/v2/")},
                                   QByteArrayLiteral("application/json"), appAgent);
        policy.timeoutMs = 15000;
        policy.maximumResponseBytes = 2 * 1024 * 1024;
        policy.maximumRedirects = 1;
        policy.maximumConcurrentRequests = 3;
        return policy;
    }
    case ProviderId::SteamArtwork: {
        auto policy = commonPolicy({}, {},
                                   QByteArrayLiteral("image/avif,image/webp,image/png,image/jpeg,*/*;q=0.5"),
                                   appAgent);
        policy.hostSuffixes = {QStringLiteral("steamstatic.com")};
        policy.exactHosts = {QStringLiteral("steamcdn-a.akamaihd.net")};
        policy.pathPrefixes = {QStringLiteral("/")};
        return policy;
    }
    case ProviderId::TelegramMedia: {
        auto policy = commonPolicy({}, {},
                                   QByteArrayLiteral("image/avif,image/webp,image/png,image/jpeg,*/*;q=0.5"),
                                   appAgent);
        policy.hostSuffixes = {QStringLiteral("telesco.pe")};
        policy.pathPrefixes = {QStringLiteral("/")};
        return policy;
    }
    case ProviderId::YouTubeMedia: {
        auto policy = commonPolicy({QStringLiteral("i.ytimg.com")}, {},
                                   QByteArrayLiteral("image/avif,image/webp,image/png,image/jpeg,*/*;q=0.5"),
                                   appAgent);
        policy.exactHosts.push_back(QStringLiteral("yt3.ggpht.com"));
        policy.pathPrefixes = {QStringLiteral("/")};
        return policy;
    }
    }
    return {};
}

bool isAllowedProviderUrl(ProviderId provider, const QUrl &url, const QString &originalHost)
{
    const ProviderPolicy policy = providerPolicy(provider);
    const QString host = url.host().toLower();
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        host.isEmpty() || !url.userInfo().isEmpty() ||
        (url.port(-1) != -1 && url.port(-1) != 443) || !url.fragment().isEmpty()) {
        return false;
    }

    const bool hostAllowed = containsCaseInsensitive(policy.exactHosts, host) ||
        std::any_of(policy.hostSuffixes.cbegin(), policy.hostSuffixes.cend(),
                    [&host](const QString &suffix) { return matchesHostSuffix(host, suffix); });
    if (!hostAllowed || (!originalHost.isEmpty() && host != originalHost.toLower())) {
        return false;
    }

    const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    const bool pathAllowed = containsCaseInsensitive(policy.exactPaths, path) ||
        std::any_of(policy.pathPrefixes.cbegin(), policy.pathPrefixes.cend(),
                    [&path](const QString &prefix) { return path.startsWith(prefix); });
    return pathAllowed;
}

QString providerCachePartitionName(ProviderId provider)
{
    switch (provider) {
    case ProviderId::Telegram: return QStringLiteral("telegram");
    case ProviderId::YouTubePage:
    case ProviderId::YouTubeFeed: return QStringLiteral("youtube");
    case ProviderId::OpenMeteoForecast:
    case ProviderId::OpenMeteoGeocoding: return QStringLiteral("open-meteo");
    case ProviderId::Frankfurter: return QStringLiteral("frankfurter");
    case ProviderId::SteamStore: return QStringLiteral("steam-store");
    case ProviderId::SteamNews: return QStringLiteral("steam-news");
    case ProviderId::SteamArtwork: return QStringLiteral("steam-artwork");
    case ProviderId::TelegramMedia: return QStringLiteral("telegram-media");
    case ProviderId::YouTubeMedia: return QStringLiteral("youtube-media");
    }
    return QStringLiteral("unknown-provider");
}

} // namespace something::providers
