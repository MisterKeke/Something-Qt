#include "steam_artwork_image_provider.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace something::providers {

SteamArtworkImageProvider::SteamArtworkImageProvider(QString profileDirectory)
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading),
      m_directory(QDir(std::move(profileDirectory)).filePath(
          QStringLiteral("assets/steam-game-artwork"))),
      m_cache(24 * 1024)
{
}

QImage SteamArtworkImageProvider::requestImage(const QString &id, QSize *size,
                                                const QSize &requestedSize)
{
    static const QRegularExpression valid(
        QStringLiteral("^[1-9][0-9]{0,9}-[a-f0-9]{64}\\.(jpg|png|webp)$"));
    if (!valid.match(id).hasMatch()) return {};

    const QFileInfo artworkDirectory(m_directory);
    const QFileInfo assetsDirectory(QDir(m_directory).filePath(QStringLiteral("..")));
    const QFileInfo profileDirectory(QDir(m_directory).filePath(QStringLiteral("../..")));
    if (!artworkDirectory.isDir() || artworkDirectory.isSymLink()
        || !assetsDirectory.isDir() || assetsDirectory.isSymLink()
        || !profileDirectory.isDir() || profileDirectory.isSymLink()) return {};
    const QString expectedDirectory = QDir::cleanPath(QDir(profileDirectory.canonicalFilePath())
        .filePath(QStringLiteral("assets/steam-game-artwork")));
    if (artworkDirectory.canonicalFilePath() != expectedDirectory
        || QDir::cleanPath(artworkDirectory.absoluteFilePath()) != expectedDirectory) return {};

    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty()) target = QSize(384, 216);
    target.setWidth(std::clamp(target.width(), 1, 512));
    target.setHeight(std::clamp(target.height(), 1, 512));
    const QString key = id + QLatin1Char(':') + QString::number(target.width())
        + QLatin1Char('x') + QString::number(target.height());
    {
        QMutexLocker lock(&m_mutex);
        if (const QImage *cached = m_cache.object(key)) {
            if (size) *size = cached->size();
            return *cached;
        }
    }

    const QFileInfo file(QDir(m_directory).filePath(id));
    if (!file.isFile() || file.isSymLink() || file.size() <= 0
        || file.size() > 2LL * 1024LL * 1024LL) return {};
    QFile content(file.filePath());
    if (!content.open(QIODevice::ReadOnly)) return {};
    const QByteArray digest = QCryptographicHash::hash(content.readAll(),
        QCryptographicHash::Sha256).toHex();
    const QString expectedDigest = id.section(QLatin1Char('-'), 1)
        .section(QLatin1Char('.'), 0, 0);
    if (QString::fromLatin1(digest) != expectedDigest) return {};

    QImageReader reader(file.filePath());
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QSize original = reader.size();
    if (!original.isValid() || original.isEmpty() || original.width() > 32768
        || original.height() > 32768
        || static_cast<qint64>(original.width()) * original.height() > 100'000'000) return {};
    QSize decoded = original;
    decoded.scale(target, Qt::KeepAspectRatio);
    decoded.setWidth(std::clamp(decoded.width(), 1, target.width()));
    decoded.setHeight(std::clamp(decoded.height(), 1, target.height()));
    reader.setScaledSize(decoded);
    const QByteArray extension = id.section(QLatin1Char('.'), -1).toLatin1();
    const QByteArray format = reader.format().toLower();
    if (!format.isEmpty()
        && !(extension == QByteArrayLiteral("jpg") && format == QByteArrayLiteral("jpeg"))
        && extension != format) return {};
    const QImage image = reader.read();
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

void SteamArtworkImageProvider::invalidate(const QString &filename)
{
    QMutexLocker lock(&m_mutex);
    const QString prefix = filename + QLatin1Char(':');
    const auto keys = m_cache.keys();
    for (const QString &key : keys)
        if (key.startsWith(prefix)) m_cache.remove(key);
}

} // namespace something::providers
