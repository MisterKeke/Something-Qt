#include "application_usage_icon_image_provider.h"

#include "../application/cancellation_token.h"
#include "../platform/desktop_applications_platform.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QUrl>

namespace something::providers {

ApplicationUsageIconImageProvider::ApplicationUsageIconImageProvider(
    platform::DesktopApplicationsPlatform *platform)
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading)
    , m_cache(128)
    , m_platform(platform)
{
}

QImage ApplicationUsageIconImageProvider::requestImage(
    const QString &id, QSize *size, const QSize &requestedSize)
{
    const QString path = QUrl::fromPercentEncoding(id.toUtf8());
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile()) return {};
    const QString key = QDir::cleanPath(file.absoluteFilePath()).toCaseFolded();
    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty()) target = QSize(64, 64);

    {
        QMutexLocker lock(&m_mutex);
        if (const QImage *cached = m_cache.object(key)) {
            if (size) *size = cached->size();
            return cached->scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    if (!m_platform) return {};
    QImage image = m_platform->extractExecutableIcon(
        file.absoluteFilePath(), application::CancellationToken{});
    if (image.isNull()) return {};
    image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size) *size = image.size();
    {
        QMutexLocker lock(&m_mutex);
        if (const QImage *cached = m_cache.object(key))
            return cached->scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_cache.insert(key, new QImage(image));
    }
    return image;
}

} // namespace something::providers
