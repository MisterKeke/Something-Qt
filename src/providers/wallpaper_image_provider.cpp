#include "wallpaper_image_provider.h"

#include <QDir>
#include <QImage>
#include <QFileInfo>
#include <QImageReader>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace something::providers {

WallpaperImageProvider::WallpaperImageProvider(QString profileDirectory)
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading)
    , m_directory(QDir(std::move(profileDirectory)).filePath(QStringLiteral("assets/wallpapers")))
    , m_cache(32 * 1024) // QCache cost units are KiB; cap decoded wallpapers near 32 MiB.
{
}

QImage WallpaperImageProvider::requestImage(const QString &id, QSize *size,
                                             const QSize &requestedSize)
{
    static const QRegularExpression idPattern(QStringLiteral("^[a-f0-9]{32}$"));
    if (!idPattern.match(id).hasMatch()) return {};

    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty()) target = QSize(1920, 1080);
    target.setWidth(std::clamp(target.width(), 1, 2560));
    target.setHeight(std::clamp(target.height(), 1, 1600));
    const QString cacheKey = id + QLatin1Char(':')
        + QString::number(target.width()) + QLatin1Char('x') + QString::number(target.height());
    {
        QMutexLocker lock(&m_cacheMutex);
        if (const QImage *cached = m_cache.object(cacheKey)) {
            if (size) *size = cached->size();
            return *cached;
        }
    }

    QString path;
    for (const QString &extension : {QStringLiteral("jpg"), QStringLiteral("png"),
                                     QStringLiteral("webp")}) {
        const QString candidate = QDir(m_directory).filePath(id + QLatin1Char('.') + extension);
        const QFileInfo info(candidate);
        if (info.isFile() && !info.isSymLink()) {
            path = candidate;
            break;
        }
    }
    if (path.isEmpty()) return {};

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize originalSize = reader.size();
    if (!originalSize.isValid() || originalSize.isEmpty()) return {};
    QSize scaled = originalSize;
    scaled.scale(target, Qt::KeepAspectRatio);
    scaled.setWidth(std::clamp(scaled.width(), 1, target.width()));
    scaled.setHeight(std::clamp(scaled.height(), 1, target.height()));
    reader.setScaledSize(scaled);
    QImage image = reader.read();
    if (image.isNull()) return {};
    if (size) *size = originalSize.isValid() ? originalSize : image.size();

    const int costKiB = std::max(1, static_cast<int>(
        (static_cast<qint64>(image.width()) * image.height() * 4 + 1023) / 1024));
    {
        QMutexLocker lock(&m_cacheMutex);
        m_cache.insert(cacheKey, new QImage(image), costKiB);
    }
    return image;
}

void WallpaperImageProvider::invalidate(const QString &id)
{
    QMutexLocker lock(&m_cacheMutex);
    const auto keys = m_cache.keys();
    for (const QString &key : keys) {
        if (key.startsWith(id + QLatin1Char(':'))) m_cache.remove(key);
    }
}

} // namespace something::providers
