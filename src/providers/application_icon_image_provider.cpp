#include "application_icon_image_provider.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>

namespace something::providers {

ApplicationIconImageProvider::ApplicationIconImageProvider(QString profileDirectory)
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading)
    , m_directory(QDir(std::move(profileDirectory)).filePath(QStringLiteral("assets/application-icons")))
    , m_cache(8 * 1024)
{
}

QImage ApplicationIconImageProvider::requestImage(const QString &id, QSize *size,
                                                   const QSize &requestedSize)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{32}\\.(ico|jpg|png|webp)$"));
    if (!pattern.match(id).hasMatch()) return {};
    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty()) target = QSize(96, 96);
    target.setWidth(std::clamp(target.width(), 1, 256));
    target.setHeight(std::clamp(target.height(), 1, 256));
    const QString key = id + QLatin1Char(':') + QString::number(target.width())
        + QLatin1Char('x') + QString::number(target.height());
    {
        QMutexLocker lock(&m_mutex);
        if (const QImage *cached = m_cache.object(key)) {
            if (size) *size = cached->size();
            return *cached;
        }
    }
    const QString path = QDir(m_directory).filePath(id);
    const QFileInfo file(path);
    if (!file.isFile() || file.isSymLink()) return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QSize original = reader.size();
    if (!original.isValid() || original.isEmpty() || original.width() > 16384
        || original.height() > 16384
        || static_cast<qint64>(original.width()) * original.height() > 128'000'000) return {};
    QSize scaled = original;
    scaled.scale(target, Qt::KeepAspectRatio);
    scaled.setWidth(std::clamp(scaled.width(), 1, target.width()));
    scaled.setHeight(std::clamp(scaled.height(), 1, target.height()));
    reader.setScaledSize(scaled);
    QImage image = reader.read();
    if (image.isNull()) return {};
    if (size) *size = image.size();
    const int costKiB = std::max(1, static_cast<int>(
        (static_cast<qint64>(image.width()) * image.height() * 4 + 1023) / 1024));
    {
        QMutexLocker lock(&m_mutex);
        m_cache.insert(key, new QImage(image), costKiB);
    }
    return image;
}

void ApplicationIconImageProvider::invalidate(const QString &filename)
{
    QMutexLocker lock(&m_mutex);
    const auto keys = m_cache.keys();
    for (const QString &key : keys)
        if (key.startsWith(filename + QLatin1Char(':'))) m_cache.remove(key);
}

} // namespace something::providers
