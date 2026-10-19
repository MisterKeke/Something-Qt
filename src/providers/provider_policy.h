#pragma once

#include <QByteArray>
#include <QStringList>
#include <QUrl>

namespace something::providers {

// These are the only remote provider families the desktop application may
// contact. User-entered URLs are never passed through this boundary.
enum class ProviderId {
    Telegram,
    YouTubePage,
    YouTubeFeed,
    OpenMeteoForecast,
    OpenMeteoGeocoding,
    Frankfurter,
    SteamStore,
    SteamArtwork,
    TelegramMedia,
    YouTubeMedia,
};

struct ProviderPolicy final {
    QStringList exactHosts;
    QStringList hostSuffixes;
    QStringList exactPaths;
    QStringList pathPrefixes;
    QByteArray accept;
    QByteArray userAgent;
    QByteArray acceptLanguage;
    int timeoutMs{25000};
    qint64 maximumResponseBytes{2 * 1024 * 1024};
    // Reference policy rejects a redirect when there are already three
    // requests in the chain, so at most two redirect follow-ups are allowed.
    int maximumRedirects{2};
    int maximumConcurrentRequests{4};
};

ProviderPolicy providerPolicy(ProviderId provider);
bool isAllowedProviderUrl(ProviderId provider,
                          const QUrl &url,
                          const QString &originalHost = {});
QString providerCachePartitionName(ProviderId provider);

} // namespace something::providers
