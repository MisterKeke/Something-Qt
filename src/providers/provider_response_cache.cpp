#include "provider_response_cache.h"

#include <QMutexLocker>

namespace something::providers {

ProviderResponseCache::ProviderResponseCache(Clock clock)
    : m_clock(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); }))
{
}

bool ProviderResponseCache::get(ProviderCachePartition partition,
                                const QString &key,
                                QByteArray *payload,
                                bool bypass)
{
    if (payload) {
        payload->clear();
    }
    if (bypass || key.isEmpty() || !payload) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    const auto now = m_clock();
    removeExpired(now);
    auto it = m_entries.find(compositeKey(partition, key));
    if (it == m_entries.end()) {
        return false;
    }
    it->lastAccess = ++m_accessSequence;
    *payload = it->payload;
    return true;
}

void ProviderResponseCache::put(ProviderCachePartition partition,
                                const QString &key,
                                QByteArray parsedPayload,
                                qsizetype itemCount)
{
    if (key.isEmpty() || parsedPayload.isEmpty() || itemCount <= 0) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    const auto now = m_clock();
    removeExpired(now);
    const QString cacheKey = compositeKey(partition, key);
    if (!m_entries.contains(cacheKey)) {
        qsizetype partitionCount = 0;
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
            if (it->partition == partition) {
                ++partitionCount;
            }
        }
        if (partitionCount >= capacity(partition)) {
            evictLeastRecentlyUsed(partition);
        }
    }
    m_entries.insert(cacheKey, Entry{std::move(parsedPayload),
                                     now + std::chrono::milliseconds(ttlMilliseconds()),
                                     ++m_accessSequence, partition});
}

void ProviderResponseCache::invalidate(ProviderCachePartition partition, const QString &key)
{
    QMutexLocker locker(&m_mutex);
    m_entries.remove(compositeKey(partition, key));
}

void ProviderResponseCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
}

qsizetype ProviderResponseCache::size(ProviderCachePartition partition) const
{
    QMutexLocker locker(&m_mutex);
    const auto now = m_clock();
    const_cast<ProviderResponseCache *>(this)->removeExpired(now);
    qsizetype result = 0;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        result += it->partition == partition ? 1 : 0;
    }
    return result;
}

QString ProviderResponseCache::compositeKey(ProviderCachePartition partition, const QString &key)
{
    return QString::number(static_cast<int>(partition)) + QLatin1Char(':') + key;
}

void ProviderResponseCache::removeExpired(std::chrono::steady_clock::time_point now)
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (now >= it->expiresAt) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void ProviderResponseCache::evictLeastRecentlyUsed(ProviderCachePartition partition)
{
    auto oldest = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->partition != partition) {
            continue;
        }
        if (oldest == m_entries.end() || it->lastAccess < oldest->lastAccess) {
            oldest = it;
        }
    }
    if (oldest != m_entries.end()) {
        m_entries.erase(oldest);
    }
}

} // namespace something::providers
