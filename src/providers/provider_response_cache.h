#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>

#include <chrono>
#include <functional>

namespace something::providers {

enum class ProviderCachePartition {
    TelegramPosts,
    YouTubeVideos,
    YouTubeHandleResolution,
};

// Stores only caller-validated/parsed payloads. List callers pass the item
// count so a legitimate empty feed remains a successful result but is not
// cached, matching the reference behavior.
class ProviderResponseCache final {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit ProviderResponseCache(Clock clock = {});

    bool get(ProviderCachePartition partition,
             const QString &key,
             QByteArray *payload,
             bool bypass = false);
    void put(ProviderCachePartition partition,
             const QString &key,
             QByteArray parsedPayload,
             qsizetype itemCount = 1);
    void invalidate(ProviderCachePartition partition, const QString &key);
    void clear();
    qsizetype size(ProviderCachePartition partition) const;

    static constexpr int ttlMilliseconds() noexcept { return 5 * 60 * 1000; }
    static constexpr qsizetype capacity(ProviderCachePartition partition) noexcept
    {
        return partition == ProviderCachePartition::YouTubeHandleResolution ? 256 : 128;
    }

private:
    struct Entry final {
        QByteArray payload;
        std::chrono::steady_clock::time_point expiresAt;
        quint64 lastAccess{0};
        ProviderCachePartition partition{ProviderCachePartition::TelegramPosts};
    };

    static QString compositeKey(ProviderCachePartition partition, const QString &key);
    void removeExpired(std::chrono::steady_clock::time_point now);
    void evictLeastRecentlyUsed(ProviderCachePartition partition);

    Clock m_clock;
    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    quint64 m_accessSequence{0};
};

} // namespace something::providers
