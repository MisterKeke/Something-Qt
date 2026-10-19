#include "wallpaper_asset_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QUuid>

namespace something::services {
namespace {

QString wallpaperDirectory(const QString &profileDirectory)
{
    return QDir(profileDirectory).filePath(QStringLiteral("assets/wallpapers"));
}

QString extensionForFormat(QByteArray format)
{
    format = format.toLower();
    if (format == "jpeg" || format == "jpg") return QStringLiteral("jpg");
    if (format == "png") return QStringLiteral("png");
    if (format == "webp") return QStringLiteral("webp");
    return {};
}

QString mimeForExtension(const QString &extension)
{
    if (extension == QStringLiteral("jpg")) return QStringLiteral("image/jpeg");
    if (extension == QStringLiteral("png")) return QStringLiteral("image/png");
    if (extension == QStringLiteral("webp")) return QStringLiteral("image/webp");
    return {};
}

QString cleanDisplayName(const QString &sourcePath)
{
    QString name = QFileInfo(sourcePath).completeBaseName().trimmed();
    if (name.isEmpty()) name = QStringLiteral("Uploaded wallpaper");
    const QList<uint> codePoints = name.toUcs4();
    if (codePoints.size() <= 120) return name;
    return QString::fromUcs4(codePoints.constData(), 120);
}

bool validGeneratedId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{32}$"));
    return pattern.match(id).hasMatch();
}

} // namespace

domain::AppError WallpaperAssetStore::stageImport(
    const QString &sourcePath,
    const QString &profileDirectory,
    const application::CancellationToken &cancellation,
    StagedWallpaper *result)
{
    if (!result || sourcePath.isEmpty() || profileDirectory.isEmpty()) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }

    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() || sourceInfo.isSymLink()
        || sourceInfo.size() <= 0 || sourceInfo.size() > maximumWallpaperBytes) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return domain::AppError(domain::ErrorCategory::Storage);
    }
    QImageReader reader(&source);
    reader.setAutoTransform(true);
    const QString extension = extensionForFormat(reader.format());
    if (extension.isEmpty() || !reader.canRead()) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }

    // Decode only a thumbnail before accepting the file. This rejects damaged
    // or unsupported payloads without allocating the full wallpaper bitmap.
    const QSize originalSize = reader.size();
    if (!originalSize.isValid() || originalSize.isEmpty()) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    QSize thumbnailSize = originalSize;
    thumbnailSize.scale(QSize(384, 216), Qt::KeepAspectRatio);
    thumbnailSize.setWidth(qMax(1, thumbnailSize.width()));
    thumbnailSize.setHeight(qMax(1, thumbnailSize.height()));
    reader.setScaledSize(thumbnailSize);
    const QImage preview = reader.read();
    if (preview.isNull()) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    if (cancellation.isCancellationRequested()) {
        return domain::AppError::cancelled();
    }
    if (!source.seek(0)) {
        return domain::AppError(domain::ErrorCategory::Storage);
    }

    const QString directoryPath = wallpaperDirectory(profileDirectory);
    if (!QDir().mkpath(directoryPath)) {
        return domain::AppError(domain::ErrorCategory::Storage);
    }
    const QString id = QUuid::createUuid().toString(QUuid::Id128).toLower();
    const QString stagingFilename = QStringLiteral(".wallpaper-upload-%1.tmp").arg(id);
    const QString stagingPath = QDir(directoryPath).filePath(stagingFilename);
    QFile stagedFile(stagingPath);
    if (!stagedFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return domain::AppError(domain::ErrorCategory::Storage);
    }
    stagedFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    qint64 copied = 0;
    QByteArray buffer(64 * 1024, '\0');
    while (!source.atEnd()) {
        if (cancellation.isCancellationRequested()) {
            stagedFile.close();
            QFile::remove(stagingPath);
            return domain::AppError::cancelled();
        }
        const qint64 read = source.read(buffer.data(), buffer.size());
        if (read < 0) {
            stagedFile.close();
            QFile::remove(stagingPath);
            return domain::AppError(domain::ErrorCategory::Storage);
        }
        if (read == 0) break;
        copied += read;
        if (copied > maximumWallpaperBytes
            || stagedFile.write(buffer.constData(), read) != read) {
            stagedFile.close();
            QFile::remove(stagingPath);
            return domain::AppError(copied > maximumWallpaperBytes
                ? domain::ErrorCategory::Validation : domain::ErrorCategory::Storage);
        }
    }
    const bool flushed = stagedFile.flush();
    stagedFile.close();
    if (!flushed) {
        QFile::remove(stagingPath);
        return domain::AppError(domain::ErrorCategory::Storage);
    }
    if (copied <= 0 || cancellation.isCancellationRequested()) {
        QFile::remove(stagingPath);
        return cancellation.isCancellationRequested()
            ? domain::AppError::cancelled()
            : domain::AppError(domain::ErrorCategory::Validation);
    }

    result->id = id;
    result->displayName = cleanDisplayName(sourcePath);
    result->stagingFilename = stagingFilename;
    result->asset = {id + QLatin1Char('.') + extension,
                     mimeForExtension(extension), copied};
    return domain::AppError::none();
}

bool WallpaperAssetStore::installStaged(const QString &profileDirectory,
                                        const StagedWallpaper &wallpaper)
{
    if (!validGeneratedId(wallpaper.id)
        || wallpaper.stagingFilename != QStringLiteral(".wallpaper-upload-%1.tmp").arg(wallpaper.id)
        || wallpaper.asset.filename.left(32) != wallpaper.id) {
        return false;
    }
    const QString directoryPath = wallpaperDirectory(profileDirectory);
    const QString sourcePath = QDir(directoryPath).filePath(wallpaper.stagingFilename);
    const QString destinationPath = QDir(directoryPath).filePath(wallpaper.asset.filename);
    return QFileInfo::exists(sourcePath) && !QFileInfo::exists(destinationPath)
        && QFile::rename(sourcePath, destinationPath);
}

void WallpaperAssetStore::discardStaged(const QString &profileDirectory,
                                        const StagedWallpaper &wallpaper)
{
    if (!validGeneratedId(wallpaper.id)) return;
    QFile::remove(QDir(wallpaperDirectory(profileDirectory)).filePath(
        QStringLiteral(".wallpaper-upload-%1.tmp").arg(wallpaper.id)));
}

bool WallpaperAssetStore::stageDeletion(const QString &profileDirectory,
                                        const storage::Wallpaper &wallpaper,
                                        QString *stagedPath)
{
    static const QRegularExpression idPattern(QStringLiteral("^[a-f0-9]{32}$"));
    static const QRegularExpression filenamePattern(
        QStringLiteral("^[a-f0-9]{32}\\.(jpg|png|webp)$"));
    if (!stagedPath || !idPattern.match(wallpaper.id).hasMatch()
        || !filenamePattern.match(wallpaper.asset.filename).hasMatch()
        || !wallpaper.asset.filename.startsWith(wallpaper.id + QLatin1Char('.'))) {
        return false;
    }
    const QString directoryPath = wallpaperDirectory(profileDirectory);
    const QString originalPath = QDir(directoryPath).filePath(wallpaper.asset.filename);
    if (!QFileInfo::exists(originalPath)) {
        stagedPath->clear();
        return true;
    }
    const QString stagedFilename = wallpaper.asset.filename + QStringLiteral(".deleting");
    *stagedPath = QDir(directoryPath).filePath(stagedFilename);
    if (!QFile::rename(originalPath, *stagedPath)) {
        stagedPath->clear();
        return false;
    }
    return true;
}

void WallpaperAssetStore::restoreDeletion(const QString &originalPath,
                                          const QString &stagedPath)
{
    if (!originalPath.isEmpty() && !stagedPath.isEmpty() && QFileInfo::exists(stagedPath)) {
        QFile::rename(stagedPath, originalPath);
    }
}

void WallpaperAssetStore::finishDeletion(const QString &stagedPath)
{
    if (!stagedPath.isEmpty()) QFile::remove(stagedPath);
}

void WallpaperAssetStore::removeAbandonedStagingFiles(const QString &profileDirectory)
{
    const QDir directory(wallpaperDirectory(profileDirectory));
    if (!directory.exists()) return;
    const QFileInfoList stagedFiles = directory.entryInfoList(
        {QStringLiteral(".wallpaper-upload-*.tmp")}, QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &file : stagedFiles) QFile::remove(file.absoluteFilePath());
}

void WallpaperAssetStore::recoverStagedDeletions(
    const QString &profileDirectory, const QStringList &persistedFilenames)
{
    static const QRegularExpression filenamePattern(
        QStringLiteral("^[a-f0-9]{32}\\.(jpg|png|webp)\\.deleting$"));
    const QDir directory(wallpaperDirectory(profileDirectory));
    if (!directory.exists()) return;
    const QFileInfoList stagedFiles = directory.entryInfoList(
        {QStringLiteral("*.deleting")}, QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &stagedFile : stagedFiles) {
        const QString stagedFilename = stagedFile.fileName();
        if (!filenamePattern.match(stagedFilename).hasMatch()) continue;
        const QString originalFilename = stagedFilename.left(stagedFilename.size()
                                                              - QStringLiteral(".deleting").size());
        const QString originalPath = directory.filePath(originalFilename);
        if (persistedFilenames.contains(originalFilename)) {
            if (!QFileInfo::exists(originalPath)) QFile::rename(stagedFile.absoluteFilePath(), originalPath);
            else QFile::remove(stagedFile.absoluteFilePath());
        } else {
            QFile::remove(stagedFile.absoluteFilePath());
        }
    }
}

void WallpaperAssetStore::removeUnreferencedWallpaperFiles(
    const QString &profileDirectory, const QStringList &persistedFilenames)
{
    static const QRegularExpression filenamePattern(
        QStringLiteral("^[a-f0-9]{32}\\.(jpg|png|webp)$"));
    const QDir directory(wallpaperDirectory(profileDirectory));
    if (!directory.exists()) return;
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.jpg"), QStringLiteral("*.png"), QStringLiteral("*.webp")},
        QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &file : files) {
        if (!filenamePattern.match(file.fileName()).hasMatch()
            || persistedFilenames.contains(file.fileName())) {
            continue;
        }
        QFile::remove(file.absoluteFilePath());
    }
}

} // namespace something::services
