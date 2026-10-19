#include "setup_icon_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QUuid>

namespace something::services {
namespace {

QString directory(const QString &profile)
{
    return QDir(profile).filePath(QStringLiteral("assets/setup-icons"));
}

QString extensionFor(QByteArray format)
{
    format = format.toLower();
    if (format == "ico") return QStringLiteral("ico");
    if (format == "jpg" || format == "jpeg") return QStringLiteral("jpg");
    if (format == "png") return QStringLiteral("png");
    if (format == "webp") return QStringLiteral("webp");
    return {};
}

QString mimeFor(const QString &extension)
{
    if (extension == QStringLiteral("ico")) return QStringLiteral("image/x-icon");
    if (extension == QStringLiteral("jpg")) return QStringLiteral("image/jpeg");
    if (extension == QStringLiteral("png")) return QStringLiteral("image/png");
    if (extension == QStringLiteral("webp")) return QStringLiteral("image/webp");
    return {};
}

bool validAssetName(const QString &filename)
{
    static const QRegularExpression re(QStringLiteral("^[a-f0-9]{32}\\.(ico|jpg|png|webp)$"));
    return re.match(filename).hasMatch();
}

bool validStagingName(const QString &filename)
{
    static const QRegularExpression re(QStringLiteral("^\\.setup-icon-upload-[a-f0-9]{32}\\.tmp$"));
    return re.match(filename).hasMatch();
}

} // namespace

domain::AppError SetupIconStore::stageImport(
    const QString &sourcePath, const QString &profileDirectory,
    const application::CancellationToken &cancellation, StagedSetupIcon *result)
{
    if (!result || sourcePath.isEmpty() || profileDirectory.isEmpty())
        return domain::AppError(domain::ErrorCategory::Validation);
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile() || info.size() < 1 || info.size() > maximumIconBytes)
        return domain::AppError(domain::ErrorCategory::Validation);
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) return domain::AppError(domain::ErrorCategory::Storage);
    QImageReader reader(&source);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QString extension = extensionFor(reader.format());
    if (extension.isEmpty() || !reader.canRead())
        return domain::AppError(domain::ErrorCategory::Validation);
    QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > 16384 || size.height() > 16384
        || static_cast<qint64>(size.width()) * size.height() > 128'000'000)
        return domain::AppError(domain::ErrorCategory::Validation);
    size.scale(QSize(128, 128), Qt::KeepAspectRatio);
    reader.setScaledSize(size);
    if (reader.read().isNull()) return domain::AppError(domain::ErrorCategory::Validation);
    if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
    if (!source.seek(0)) return domain::AppError(domain::ErrorCategory::Storage);
    const QByteArray bytes = source.read(maximumIconBytes + 1);
    if (bytes.isEmpty() || bytes.size() > maximumIconBytes)
        return domain::AppError(domain::ErrorCategory::Validation);

    const QString dir = directory(profileDirectory);
    if (!QDir().mkpath(dir)) return domain::AppError(domain::ErrorCategory::Storage);
    const QString id = QUuid::createUuid().toString(QUuid::Id128).toLower();
    const QString staging = QStringLiteral(".setup-icon-upload-%1.tmp").arg(id);
    QFile output(QDir(dir).filePath(staging));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return domain::AppError(domain::ErrorCategory::Storage);
    output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const bool written = output.write(bytes) == bytes.size() && output.flush();
    output.close();
    if (!written || cancellation.isCancellationRequested()) {
        QFile::remove(output.fileName());
        return cancellation.isCancellationRequested() ? domain::AppError::cancelled()
            : domain::AppError(domain::ErrorCategory::Storage);
    }
    result->asset = {id + QLatin1Char('.') + extension, mimeFor(extension), bytes.size()};
    result->stagingFilename = staging;
    return domain::AppError::none();
}

bool SetupIconStore::install(const QString &profileDirectory, const StagedSetupIcon &icon)
{
    if (!validAssetName(icon.asset.filename) || !validStagingName(icon.stagingFilename)) return false;
    const QDir dir(directory(profileDirectory));
    const QString source = dir.filePath(icon.stagingFilename);
    const QString destination = dir.filePath(icon.asset.filename);
    return QFileInfo::exists(source) && !QFileInfo::exists(destination)
        && QFile::rename(source, destination);
}

void SetupIconStore::discard(const QString &profileDirectory, const StagedSetupIcon &icon)
{
    if (validStagingName(icon.stagingFilename))
        QFile::remove(QDir(directory(profileDirectory)).filePath(icon.stagingFilename));
}

bool SetupIconStore::remove(const QString &profileDirectory, const QString &filename)
{
    if (!validAssetName(filename)) return false;
    const QString path = QDir(directory(profileDirectory)).filePath(filename);
    return !QFileInfo::exists(path) || QFile::remove(path);
}

void SetupIconStore::removeAbandonedStagingFiles(const QString &profileDirectory)
{
    const QDir dir(directory(profileDirectory));
    if (!dir.exists()) return;
    const QFileInfoList files = dir.entryInfoList({QStringLiteral(".setup-icon-upload-*.tmp")},
        QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &file : files) QFile::remove(file.absoluteFilePath());
}

} // namespace something::services
