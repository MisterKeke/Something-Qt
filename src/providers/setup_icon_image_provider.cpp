#include "setup_icon_image_provider.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace something::providers {

SetupIconImageProvider::SetupIconImageProvider(QString profileDirectory)
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading),
      m_directory(QDir(std::move(profileDirectory)).filePath(QStringLiteral("assets/setup-icons"))),
      m_cache(8 * 1024)
{
}

QImage SetupIconImageProvider::requestImage(const QString &id, QSize *size,
                                               const QSize &requestedSize)
{
    static const QRegularExpression valid(QStringLiteral("^[a-f0-9]{32}\\.(ico|jpg|png|webp)$"));
    if (!valid.match(id).hasMatch()) return {};
    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty()) target = QSize(128, 128);
    target.setWidth(std::clamp(target.width(), 1, 256));
    target.setHeight(std::clamp(target.height(), 1, 256));
    const QString key = id + QLatin1Char(':') + QString::number(target.width())
        + QLatin1Char('x') + QString::number(target.height());
    QMutexLocker locker(&m_mutex);
    if (QImage *cached = m_cache.object(key)) {
        if (size) *size = cached->size();
        return *cached;
    }
    const QString path = QDir(m_directory).filePath(id);
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.size() <= 0 || info.size() > 5LL * 1024LL * 1024LL)
        return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize decode = reader.size();
    if (!decode.isValid() || decode.isEmpty() || decode.width() > 16384 || decode.height() > 16384
        || static_cast<qint64>(decode.width()) * decode.height() > 128'000'000) return {};
    decode.scale(target, Qt::KeepAspectRatio);
    reader.setScaledSize(decode);
    QImage image = reader.read();
    if (image.isNull()) return {};
    if (size) *size = image.size();
    const int cost = qMax(1, static_cast<int>(image.sizeInBytes() / 1024));
    m_cache.insert(key, new QImage(image), cost);
    return image;
}

void SetupIconImageProvider::invalidate(const QString &filename)
{
    QMutexLocker locker(&m_mutex);
    const QString prefix = filename + QLatin1Char(':');
    for (const QString &key : m_cache.keys())
        if (key.startsWith(prefix)) m_cache.remove(key);
}

} // namespace something::providers
