#include "provider_image_provider.h"

#include <QCryptographicHash>
#include <QMutexLocker>

#include <limits>

namespace something::providers {

ProviderImageProvider::ProviderImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading)
    , m_images(24 * 1024 * 1024)
{
}

QString ProviderImageProvider::cacheImage(const QString &contentKey, QImage image)
{
    if (contentKey.isEmpty() || image.isNull()) return {};
    constexpr int maxWidth = 1280;
    constexpr int maxHeight = 900;
    if (image.width() > maxWidth || image.height() > maxHeight)
        image = image.scaled(QSize(maxWidth, maxHeight), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString id = QString::fromLatin1(QCryptographicHash::hash(
        contentKey.toUtf8(), QCryptographicHash::Sha256).toHex());
    const int cost = qMax(1, int(qMin<qsizetype>(image.sizeInBytes(), std::numeric_limits<int>::max())));
    QMutexLocker lock(&m_mutex);
    m_images.insert(id, new QImage(std::move(image)), cost);
    return QStringLiteral("image://provider-media/") + id;
}

QString ProviderImageProvider::cachedSource(const QString &contentKey)
{
    if (contentKey.isEmpty()) return {};
    const QString id = QString::fromLatin1(QCryptographicHash::hash(
        contentKey.toUtf8(), QCryptographicHash::Sha256).toHex());
    QMutexLocker lock(&m_mutex);
    return m_images.object(id) ? QStringLiteral("image://provider-media/") + id : QString();
}

QImage ProviderImageProvider::requestImage(const QString &id, QSize *size,
                                           const QSize &requestedSize)
{
    QMutexLocker lock(&m_mutex);
    const QImage *cached = m_images.object(id);
    if (!cached) return {};
    QImage result = *cached;
    if (size) *size = result.size();
    if (requestedSize.isValid() && !requestedSize.isEmpty())
        result = result.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return result;
}

} // namespace something::providers
