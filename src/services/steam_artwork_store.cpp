#include "steam_artwork_store.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QUuid>

namespace something::services {
namespace {

QString artworkDirectory(const QString &profileDirectory)
{
    return QDir(profileDirectory).filePath(QStringLiteral("assets/steam-game-artwork"));
}

bool ensureArtworkDirectory(const QString &profileDirectory, QString *path)
{
    if (profileDirectory.isEmpty() || !path) return false;
    const QFileInfo profile(profileDirectory);
    if (!profile.isDir() || profile.isSymLink()) return false;
    const QString canonicalProfile = profile.canonicalFilePath();
    if (canonicalProfile.isEmpty()) return false;
    const QString assetsPath = QDir(canonicalProfile).filePath(QStringLiteral("assets"));
    const QFileInfo existingAssets(assetsPath);
    if (existingAssets.isSymLink() || (existingAssets.exists() && !existingAssets.isDir()))
        return false;
    const QString expected = QDir::cleanPath(QDir(canonicalProfile).filePath(
        QStringLiteral("assets/steam-game-artwork")));
    if (!QDir(canonicalProfile).mkpath(QStringLiteral("assets/steam-game-artwork")))
        return false;
    const QFileInfo assets(QDir(canonicalProfile).filePath(QStringLiteral("assets")));
    const QFileInfo directory(expected);
    if (!assets.isDir() || assets.isSymLink() || !directory.isDir()
        || directory.isSymLink() || directory.canonicalFilePath() != expected)
        return false;
    *path = expected;
    return true;
}

QString mimeForExtension(const QString &extension)
{
    if (extension == QStringLiteral("jpg")) return QStringLiteral("image/jpeg");
    if (extension == QStringLiteral("png")) return QStringLiteral("image/png");
    if (extension == QStringLiteral("webp")) return QStringLiteral("image/webp");
    return {};
}

QString extensionForPayload(const QByteArray &body)
{
    static const QByteArray pngMagic = QByteArray::fromHex("89504e470d0a1a0a");
    if (body.startsWith(pngMagic)) return QStringLiteral("png");
    if (body.size() >= 3 && static_cast<uchar>(body.at(0)) == 0xff
        && static_cast<uchar>(body.at(1)) == 0xd8
        && static_cast<uchar>(body.at(2)) == 0xff) return QStringLiteral("jpg");
    if (body.size() >= 12 && body.left(4) == QByteArrayLiteral("RIFF")
        && body.mid(8, 4) == QByteArrayLiteral("WEBP")) return QStringLiteral("webp");
    return {};
}

bool validFilename(const QString &filename)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[1-9][0-9]{0,9}-[a-f0-9]{64}\\.(jpg|png|webp)$"));
    return pattern.match(filename).hasMatch();
}

bool validStagingFilename(const QString &filename)
{
    static const QRegularExpression pattern(QStringLiteral("^\\.steam-artwork-[a-f0-9]{32}\\.tmp$"));
    return pattern.match(filename).hasMatch();
}

bool safeRegularFile(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile() && !info.isSymLink()
        && info.size() > 0 && info.size() <= SteamArtworkStore::maximumArtworkBytes;
}

bool hasSupportedDecoder(const QString &extension)
{
    const QByteArray format = extension == QStringLiteral("jpg")
        ? QByteArrayLiteral("jpeg") : extension.toLatin1();
    const auto formats = QImageReader::supportedImageFormats();
    for (const QByteArray &candidate : formats)
        if (candidate.compare(format, Qt::CaseInsensitive) == 0) return true;
    return false;
}

bool validateDecodableImage(const QByteArray &body, const QString &extension)
{
    if (!hasSupportedDecoder(extension)) return true;
    QBuffer buffer;
    buffer.setData(body);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return false;
    const QSize sourceSize = reader.size();
    if (!sourceSize.isValid() || sourceSize.isEmpty() || sourceSize.width() > 32768
        || sourceSize.height() > 32768
        || qint64(sourceSize.width()) * qint64(sourceSize.height()) > 100000000) return false;
    QSize previewSize = sourceSize;
    previewSize.scale(QSize(256, 144), Qt::KeepAspectRatio);
    previewSize.setWidth(qMax(1, previewSize.width()));
    previewSize.setHeight(qMax(1, previewSize.height()));
    reader.setScaledSize(previewSize);
    return !reader.read().isNull();
}

QString finalPath(const QString &profileDirectory, const QString &filename)
{
    if (!validFilename(filename)) return {};
    return QDir(artworkDirectory(profileDirectory)).filePath(filename);
}

bool sameBytesAsName(const QString &path, const QString &filename)
{
    if (!safeRegularFile(path)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray digest = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    const QString expected = filename.section(QLatin1Char('-'), 1).section(QLatin1Char('.'), 0, 0).toLatin1();
    return QString::fromLatin1(digest) == expected;
}

} // namespace

domain::AppError SteamArtworkStore::stageResponse(
    const QString &profileDirectory, quint32 appId, const QByteArray &body,
    const application::CancellationToken &cancellation, StagedSteamArtwork *result)
{
    if (!result || profileDirectory.isEmpty() || appId == 0 || body.isEmpty()
        || body.size() > maximumArtworkBytes)
        return domain::AppError(domain::ErrorCategory::Validation);
    if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();

    const QString extension = extensionForPayload(body);
    if (extension.isEmpty() || !validateDecodableImage(body, extension))
        return domain::AppError(domain::ErrorCategory::InvalidResponse);

    const QByteArray digest = QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex();
    const QString filename = QStringLiteral("%1-%2.%3")
        .arg(appId).arg(QString::fromLatin1(digest), extension);
    if (!validFilename(filename)) return domain::AppError(domain::ErrorCategory::Validation);
    QString directoryPath;
    if (!ensureArtworkDirectory(profileDirectory, &directoryPath))
        return domain::AppError(domain::ErrorCategory::Storage);
    const QString destination = QDir(directoryPath).filePath(filename);
    const QFileInfo destinationInfo(destination);
    if (destinationInfo.exists()) {
        if (!sameBytesAsName(destination, filename))
            return domain::AppError(domain::ErrorCategory::Storage);
        result->asset = {filename, mimeForExtension(extension), body.size()};
        result->stagingFilename.clear();
        result->stagedNewFile = false;
        return {};
    }
    if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();

    const QString stagingFilename = QStringLiteral(".steam-artwork-%1.tmp")
        .arg(QUuid::createUuid().toString(QUuid::Id128).toLower());
    const QString stagingPath = QDir(directoryPath).filePath(stagingFilename);
    QFile staged(stagingPath);
    if (!staged.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return domain::AppError(domain::ErrorCategory::Storage);
    staged.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const bool wrote = staged.write(body) == body.size() && staged.flush();
    staged.close();
    if (!wrote || cancellation.isCancellationRequested()) {
        QFile::remove(stagingPath);
        return cancellation.isCancellationRequested() ? domain::AppError::cancelled()
            : domain::AppError(domain::ErrorCategory::Storage);
    }
    result->asset = {filename, mimeForExtension(extension), body.size()};
    result->stagingFilename = stagingFilename;
    result->stagedNewFile = true;
    return {};
}

bool SteamArtworkStore::install(const QString &profileDirectory,
                                const StagedSteamArtwork &artwork,
                                bool *installedNewFile)
{
    if (installedNewFile) *installedNewFile = false;
    if (!validFilename(artwork.asset.filename)) return false;
    QString directoryPath;
    if (!ensureArtworkDirectory(profileDirectory, &directoryPath)) return false;
    if (!artwork.stagedNewFile) {
        const QString existing = finalPath(profileDirectory, artwork.asset.filename);
        return sameBytesAsName(existing, artwork.asset.filename);
    }
    if (!validStagingFilename(artwork.stagingFilename)) return false;
    const QString source = QDir(directoryPath).filePath(artwork.stagingFilename);
    const QString destination = finalPath(profileDirectory, artwork.asset.filename);
    if (!safeRegularFile(source)) return false;
    if (QFileInfo::exists(destination))
    {
        const bool validExisting = sameBytesAsName(destination, artwork.asset.filename);
        if (validExisting) QFile::remove(source);
        return validExisting;
    }
    if (!QFile::rename(source, destination)) {
        if (sameBytesAsName(destination, artwork.asset.filename)) {
            QFile::remove(source);
            return true;
        }
        return false;
    }
    if (installedNewFile) *installedNewFile = true;
    return true;
}

void SteamArtworkStore::discard(const QString &profileDirectory,
                                const StagedSteamArtwork &artwork)
{
    if (!validStagingFilename(artwork.stagingFilename)) return;
    QString directoryPath;
    if (!ensureArtworkDirectory(profileDirectory, &directoryPath)) return;
    QFile::remove(QDir(directoryPath).filePath(artwork.stagingFilename));
}

QString SteamArtworkStore::imageSource(const QString &profileDirectory,
                                       const QString &filename)
{
    QString directoryPath;
    if (!ensureArtworkDirectory(profileDirectory, &directoryPath)) return {};
    const QString path = finalPath(profileDirectory, filename);
    if (path.isEmpty() || !safeRegularFile(path)) return {};
    return QStringLiteral("image://steam-artwork/") + filename;
}

bool SteamArtworkStore::stageDeletion(const QString &profileDirectory,
                                      const QString &filename, QString *stagedPath,
                                      QString *originalPath)
{
    if (!stagedPath) return false;
    *stagedPath = {};
    QString directoryPath;
    if (!ensureArtworkDirectory(profileDirectory, &directoryPath)) return false;
    const QString original = finalPath(profileDirectory, filename);
    if (original.isEmpty()) return false;
    if (originalPath) *originalPath = original;
    if (!QFileInfo::exists(original)) return true;
    if (!safeRegularFile(original)) return false;
    const QString stagedName = QStringLiteral(".steam-artwork-%1.deleting")
        .arg(QUuid::createUuid().toString(QUuid::Id128).toLower());
    const QString staged = QDir(directoryPath).filePath(stagedName);
    if (!QFile::rename(original, staged)) return false;
    *stagedPath = staged;
    return true;
}

void SteamArtworkStore::restoreDeletion(const QString &originalPath, const QString &stagedPath)
{
    if (originalPath.isEmpty() || stagedPath.isEmpty() || !safeRegularFile(stagedPath)) return;
    if (!QFileInfo::exists(originalPath)) QFile::rename(stagedPath, originalPath);
}

void SteamArtworkStore::finishDeletion(const QString &stagedPath)
{
    if (!stagedPath.isEmpty() && safeRegularFile(stagedPath)) QFile::remove(stagedPath);
}

void SteamArtworkStore::removeAbandonedStagingFiles(const QString &profileDirectory)
{
    QString directoryPath;
    if (!ensureArtworkDirectory(profileDirectory, &directoryPath)) return;
    const QDir directory(directoryPath);
    if (!directory.exists()) return;
    static const QRegularExpression temporaryPattern(
        QStringLiteral("^\\.steam-artwork-[a-f0-9]{32}\\.(tmp|deleting)$"));
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral(".steam-artwork-*")}, QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &file : files)
        if (temporaryPattern.match(file.fileName()).hasMatch() && safeRegularFile(file.absoluteFilePath()))
            QFile::remove(file.absoluteFilePath());
}

void SteamArtworkStore::removeInstalledIfNew(const QString &profileDirectory,
                                            const StagedSteamArtwork &artwork,
                                            bool installedNewFile)
{
    if (!installedNewFile) return;
    QString directoryPath;
    if (!ensureArtworkDirectory(profileDirectory, &directoryPath)) return;
    const QString path = finalPath(profileDirectory, artwork.asset.filename);
    if (!path.isEmpty() && safeRegularFile(path)) QFile::remove(path);
}

} // namespace something::services
