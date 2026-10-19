#include "desktop_app_icon_store.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QRegularExpression>
#include <QUuid>

namespace something::services {
namespace {

QString iconDirectory(const QString &profileDirectory)
{
    return QDir(profileDirectory).filePath(QStringLiteral("assets/application-icons"));
}

QString extensionForFormat(QByteArray format)
{
    format = format.toLower();
    if (format == "ico") return QStringLiteral("ico");
    if (format == "jpg" || format == "jpeg") return QStringLiteral("jpg");
    if (format == "png") return QStringLiteral("png");
    if (format == "webp") return QStringLiteral("webp");
    return {};
}

QString mimeForExtension(const QString &extension)
{
    if (extension == QStringLiteral("ico")) return QStringLiteral("image/x-icon");
    if (extension == QStringLiteral("jpg")) return QStringLiteral("image/jpeg");
    if (extension == QStringLiteral("png")) return QStringLiteral("image/png");
    if (extension == QStringLiteral("webp")) return QStringLiteral("image/webp");
    return {};
}

bool validFilename(const QString &filename)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{32}\\.(ico|jpg|png|webp)$"));
    return pattern.match(filename).hasMatch();
}

bool validStagingFilename(const QString &filename)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\.app-icon-upload-[a-f0-9]{32}\\.tmp$"));
    return pattern.match(filename).hasMatch();
}

domain::AppError writeStaged(const QByteArray &bytes, const QString &extension,
                             const QString &mime, const QString &profileDirectory,
                             const application::CancellationToken &cancellation,
                             StagedDesktopAppIcon *result)
{
    if (!result || bytes.isEmpty() || bytes.size() > DesktopAppIconStore::maximumIconBytes
        || cancellation.isCancellationRequested())
        return cancellation.isCancellationRequested() ? domain::AppError::cancelled()
            : domain::AppError(domain::ErrorCategory::Validation);
    const QString directoryPath = iconDirectory(profileDirectory);
    if (!QDir().mkpath(directoryPath)) return domain::AppError(domain::ErrorCategory::Storage);
    const QString id = QUuid::createUuid().toString(QUuid::Id128).toLower();
    const QString stagingFilename = QStringLiteral(".app-icon-upload-%1.tmp").arg(id);
    QFile output(QDir(directoryPath).filePath(stagingFilename));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return domain::AppError(domain::ErrorCategory::Storage);
    output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const bool written = output.write(bytes) == bytes.size();
    const bool flushed = output.flush();
    output.close();
    if (!written || !flushed || cancellation.isCancellationRequested()) {
        QFile::remove(output.fileName());
        return cancellation.isCancellationRequested() ? domain::AppError::cancelled()
            : domain::AppError(domain::ErrorCategory::Storage);
    }
    result->asset = {id + QLatin1Char('.') + extension, mime, bytes.size()};
    result->stagingFilename = stagingFilename;
    return domain::AppError::none();
}

QByteArray readBounded(QFile &file)
{
    if (file.size() <= 0 || file.size() > DesktopAppIconStore::maximumIconBytes) return {};
    QByteArray bytes = file.read(DesktopAppIconStore::maximumIconBytes + 1);
    if (bytes.size() > DesktopAppIconStore::maximumIconBytes) return {};
    return bytes;
}

} // namespace

domain::AppError DesktopAppIconStore::stageImport(
    const QString &sourcePath, const QString &profileDirectory,
    const application::CancellationToken &cancellation, StagedDesktopAppIcon *result)
{
    if (!result || sourcePath.isEmpty() || profileDirectory.isEmpty())
        return domain::AppError(domain::ErrorCategory::Validation);
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile() || info.size() <= 0 || info.size() > maximumIconBytes)
        return domain::AppError(domain::ErrorCategory::Validation);
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) return domain::AppError(domain::ErrorCategory::Storage);
    QImageReader reader(&source);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QString extension = extensionForFormat(reader.format());
    if (extension.isEmpty() || !reader.canRead())
        return domain::AppError(domain::ErrorCategory::Validation);
    QSize decodeSize = reader.size();
    if (!decodeSize.isValid() || decodeSize.isEmpty()) return domain::AppError(domain::ErrorCategory::Validation);
    if (decodeSize.width() > 16384 || decodeSize.height() > 16384
        || static_cast<qint64>(decodeSize.width()) * decodeSize.height() > 128'000'000)
        return domain::AppError(domain::ErrorCategory::Validation);
    decodeSize.scale(QSize(128, 128), Qt::KeepAspectRatio);
    reader.setScaledSize(decodeSize);
    if (reader.read().isNull()) return domain::AppError(domain::ErrorCategory::Validation);
    if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
    if (!source.seek(0)) return domain::AppError(domain::ErrorCategory::Storage);
    const QByteArray bytes = readBounded(source);
    if (bytes.isEmpty()) return domain::AppError(domain::ErrorCategory::Validation);
    return writeStaged(bytes, extension, mimeForExtension(extension), profileDirectory,
                       cancellation, result);
}

domain::AppError DesktopAppIconStore::stageImage(
    const QImage &image, const QString &profileDirectory,
    const application::CancellationToken &cancellation, StagedDesktopAppIcon *result)
{
    if (image.isNull()) return domain::AppError(domain::ErrorCategory::Validation);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return domain::AppError(domain::ErrorCategory::Storage);
    return writeStaged(bytes, QStringLiteral("png"), QStringLiteral("image/png"),
                       profileDirectory, cancellation, result);
}

bool DesktopAppIconStore::install(const QString &profileDirectory,
                                  const StagedDesktopAppIcon &icon)
{
    if (!validFilename(icon.asset.filename) || !validStagingFilename(icon.stagingFilename)) return false;
    const QDir directory(iconDirectory(profileDirectory));
    const QString staged = directory.filePath(icon.stagingFilename);
    const QString destination = directory.filePath(icon.asset.filename);
    return QFileInfo::exists(staged) && !QFileInfo::exists(destination)
        && QFile::rename(staged, destination);
}

void DesktopAppIconStore::discard(const QString &profileDirectory,
                                  const StagedDesktopAppIcon &icon)
{
    if (validStagingFilename(icon.stagingFilename))
        QFile::remove(QDir(iconDirectory(profileDirectory)).filePath(icon.stagingFilename));
}

bool DesktopAppIconStore::stageDeletion(const QString &profileDirectory, const QString &filename,
                                        StagedDesktopAppIconDeletion *result)
{
    if (!result || !validFilename(filename)) return false;
    const QDir directory(iconDirectory(profileDirectory));
    const QString original = directory.filePath(filename);
    if (!QFileInfo::exists(original)) {
        *result = {filename, {}, false};
        return true;
    }
    const QString stagedFilename = filename + QStringLiteral(".deleting");
    if (!QFile::rename(original, directory.filePath(stagedFilename))) return false;
    *result = {filename, stagedFilename, true};
    return true;
}

void DesktopAppIconStore::restoreDeletion(const QString &profileDirectory,
                                          const StagedDesktopAppIconDeletion &deletion)
{
    if (!deletion.moved || !validFilename(deletion.filename)
        || deletion.stagedFilename != deletion.filename + QStringLiteral(".deleting")) return;
    const QDir directory(iconDirectory(profileDirectory));
    QFile::rename(directory.filePath(deletion.stagedFilename), directory.filePath(deletion.filename));
}

void DesktopAppIconStore::finishDeletion(const QString &profileDirectory,
                                          const StagedDesktopAppIconDeletion &deletion)
{
    if (!deletion.moved || !validFilename(deletion.filename)
        || deletion.stagedFilename != deletion.filename + QStringLiteral(".deleting")) return;
    QFile::remove(QDir(iconDirectory(profileDirectory)).filePath(deletion.stagedFilename));
}

bool DesktopAppIconStore::remove(const QString &profileDirectory, const QString &filename)
{
    if (!validFilename(filename)) return false;
    const QString path = QDir(iconDirectory(profileDirectory)).filePath(filename);
    return !QFileInfo::exists(path) || QFile::remove(path);
}

void DesktopAppIconStore::removeAbandonedStagingFiles(const QString &profileDirectory)
{
    const QDir directory(iconDirectory(profileDirectory));
    if (!directory.exists()) return;
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral(".app-icon-upload-*.tmp")}, QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &file : files) QFile::remove(file.absoluteFilePath());

    const QFileInfoList deletions = directory.entryInfoList(
        {QStringLiteral("*.deleting")}, QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &file : deletions) {
        const QString stagedFilename = file.fileName();
        const QString filename = stagedFilename.left(stagedFilename.size()
                                                       - QStringLiteral(".deleting").size());
        if (!validFilename(filename)) {
            QFile::remove(file.absoluteFilePath());
            continue;
        }
        const QString originalPath = directory.filePath(filename);
        if (!QFileInfo::exists(originalPath)) QFile::rename(file.absoluteFilePath(), originalPath);
        else QFile::remove(file.absoluteFilePath());
    }
}

} // namespace something::services
