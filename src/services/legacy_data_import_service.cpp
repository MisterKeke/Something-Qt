#include "legacy_data_import_service.h"

#include "../domain/favorite_rules.h"
#include "../domain/steam_game.h"
#include "../domain/task_rules.h"
#include "../domain/desktop_application_rules.h"
#include "../providers/feature_parsers.h"

#include <QDate>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QImageReader>
#include <QImage>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>

namespace something::services {
namespace {

constexpr qint64 maximumPlanBytes = 64LL * 1024LL * 1024LL;
constexpr qint64 maximumWallpaperBytes = 20LL * 1024LL * 1024LL;
constexpr qint64 maximumIconBytes = 5LL * 1024LL * 1024LL;
constexpr qint64 maximumSteamArtworkBytes = 2LL * 1024LL * 1024LL;

QString generatedId()
{
    return QUuid::createUuid().toString(QUuid::Id128).toLower();
}

bool validId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{32}$"));
    return pattern.match(value).hasMatch();
}

QString stageRoot(const QString &profileDirectory, const QString &importId)
{
    return QDir(profileDirectory).filePath(QStringLiteral(".legacy-import-staging/") + importId);
}

QString journalPath(const QString &profileDirectory)
{
    return QDir(profileDirectory).filePath(QStringLiteral(".legacy-import-journal.json"));
}

bool within(const QString &parent, const QString &path)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString base = QDir::cleanPath(parent);
    const QString candidate = QDir::cleanPath(path);
    const QString prefix = base.endsWith(QLatin1Char('/')) ? base : base + QLatin1Char('/');
    return candidate.compare(base, sensitivity) == 0
        || candidate.startsWith(prefix, sensitivity);
}

bool managedPath(const QString &profileDirectory, const QString &path)
{
    const QFileInfo profile(profileDirectory);
    const QString canonicalProfile = profile.canonicalFilePath();
    if (canonicalProfile.isEmpty() || !profile.isDir() || profile.isSymLink() || profile.isJunction()
        || !within(profile.absoluteFilePath(), canonicalProfile)
        || !within(canonicalProfile, profile.absoluteFilePath())
        || !within(profile.absoluteFilePath(), QFileInfo(path).absoluteFilePath())) return false;
    QFileInfo ancestor(path);
    while (!ancestor.exists()) {
        if (ancestor.isSymLink() || ancestor.isJunction()) return false;
        const QString parent = ancestor.absolutePath();
        if (parent == ancestor.absoluteFilePath()) return false;
        ancestor = QFileInfo(parent);
    }
    const QString canonical = ancestor.canonicalFilePath();
    return !ancestor.isSymLink() && !ancestor.isJunction() && within(canonicalProfile, canonical)
        && within(ancestor.absoluteFilePath(), canonical) && within(canonical, ancestor.absoluteFilePath());
}

bool ensureManagedDirectory(const QString &profileDirectory, const QString &relative)
{
    const QString path = QDir(profileDirectory).filePath(relative);
    return managedPath(profileDirectory, path) && QDir().mkpath(path)
        && managedPath(profileDirectory, path);
}

bool sameFileSnapshot(const QFileInfo &before, const QFileInfo &after)
{
    return before.exists() == after.exists()
        && (!before.exists() || (before.isFile() && after.isFile()
            && before.size() == after.size()
            && before.lastModified() == after.lastModified()));
}

bool copyClosedSnapshotFile(const QString &source, const QString &destination,
                            const application::CancellationToken &cancellation)
{
    const QFileInfo before(source);
    if (!before.exists() || !before.isFile() || before.isSymLink() || before.isJunction()) return false;
    if (cancellation.isCancellationRequested()) return false;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) return false;
    QFile sourceFile(source);
    if (!sourceFile.open(QIODevice::ReadOnly)) return false;
    QSaveFile target(destination);
    if (!target.open(QIODevice::WriteOnly)) return false;
    QByteArray buffer(64 * 1024, '\0');
    while (!sourceFile.atEnd()) {
        if (cancellation.isCancellationRequested()) {
            target.cancelWriting();
            return false;
        }
        const qint64 count = sourceFile.read(buffer.data(), buffer.size());
        if (count <= 0 || target.write(buffer.constData(), count) != count) {
            target.cancelWriting();
            return false;
        }
    }
    if (!target.commit()) return false;
    QFileInfo after(source);
    if (!sameFileSnapshot(before, after)) {
        QFile::remove(destination);
        return false;
    }
    QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QString mimeFromHeader(const QByteArray &header)
{
    if (header.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) return QStringLiteral("image/png");
    if (header.size() >= 3 && static_cast<unsigned char>(header[0]) == 0xff
        && static_cast<unsigned char>(header[1]) == 0xd8
        && static_cast<unsigned char>(header[2]) == 0xff) return QStringLiteral("image/jpeg");
    if (header.size() >= 12 && header.left(4) == QByteArrayLiteral("RIFF")
        && header.mid(8, 4) == QByteArrayLiteral("WEBP")) return QStringLiteral("image/webp");
    if (header.size() >= 4 && header[0] == '\0' && header[1] == '\0'
        && header[2] == '\1' && header[3] == '\0') return QStringLiteral("image/x-icon");
    return {};
}

QString extensionForMime(const QString &mime)
{
    if (mime == QStringLiteral("image/jpeg")) return QStringLiteral("jpg");
    if (mime == QStringLiteral("image/png")) return QStringLiteral("png");
    if (mime == QStringLiteral("image/webp")) return QStringLiteral("webp");
    if (mime == QStringLiteral("image/x-icon")) return QStringLiteral("ico");
    return {};
}

QString targetDirectoryForKind(const QString &kind)
{
    if (kind == QStringLiteral("wallpaper")) return QStringLiteral("assets/wallpapers");
    if (kind == QStringLiteral("desktop-icon")) return QStringLiteral("assets/application-icons");
    if (kind == QStringLiteral("setup-icon")) return QStringLiteral("assets/setup-icons");
    if (kind == QStringLiteral("steam-artwork")) return QStringLiteral("assets/steam-game-artwork");
    return {};
}

QString sourceDirectoryForKind(const QString &kind)
{
    if (kind == QStringLiteral("wallpaper")) return QStringLiteral("user-wallpapers");
    if (kind == QStringLiteral("desktop-icon")) return QStringLiteral("icons");
    if (kind == QStringLiteral("setup-icon")) return QStringLiteral("setup-icons");
    if (kind == QStringLiteral("steam-artwork")) return QStringLiteral("steam-game-images");
    return {};
}

bool safeRelativeAsset(const QString &importId, const QString &stageRelative,
                       const QString &targetRelative, const QString &kind)
{
    static const QRegularExpression filenamePattern(
        QStringLiteral("^[a-f0-9]{32}\\.(jpg|png|webp|ico)$"));
    static const QRegularExpression steamFilenamePattern(
        QStringLiteral("^[1-9][0-9]{0,9}-[a-f0-9]{64}\\.(jpg|png|webp)$"));
    const QString directory = targetDirectoryForKind(kind);
    if (directory.isEmpty()) return false;
    const QString stagePrefix = QStringLiteral(".legacy-import-staging/") + importId
        + QStringLiteral("/assets/");
    if (!stageRelative.startsWith(stagePrefix)
        || stageRelative.contains(QStringLiteral(".."))
        || stageRelative.contains(QLatin1Char('\\'))) return false;
    const QString expectedTargetPrefix = directory + QLatin1Char('/');
    if (!targetRelative.startsWith(expectedTargetPrefix)
        || targetRelative.contains(QStringLiteral(".."))
        || targetRelative.contains(QLatin1Char('\\'))) return false;
    const QString filename = targetRelative.mid(expectedTargetPrefix.size());
    return (kind == QStringLiteral("steam-artwork") ? steamFilenamePattern.match(filename).hasMatch()
                                                     : filenamePattern.match(filename).hasMatch())
        && stageRelative == stagePrefix + filename
        && targetRelative == expectedTargetPrefix + filename;
}

bool validJournal(const QJsonObject &journal, const QString &importId)
{
    static const QRegularExpression digest(QStringLiteral("^[a-f0-9]{64}$"));
    if (!validId(importId) || journal.value(QStringLiteral("importId")).toString() != importId
        || !journal.value(QStringLiteral("assets")).isArray()) return false;
    QSet<QString> targets;
    for (const QJsonValue &value : journal.value(QStringLiteral("assets")).toArray()) {
        if (!value.isObject()) return false;
        const QJsonObject asset = value.toObject();
        const QString target = asset.value(QStringLiteral("targetRelative")).toString();
        bool sizeOk = false;
        const qint64 bytes = asset.value(QStringLiteral("byteSize")).toString().toLongLong(&sizeOk);
        if (!safeRelativeAsset(importId, asset.value(QStringLiteral("stageRelative")).toString(),
                                target, asset.value(QStringLiteral("kind")).toString())
            || targets.contains(target) || !sizeOk || bytes <= 0 || bytes > maximumWallpaperBytes
            || !digest.match(asset.value(QStringLiteral("sha256")).toString()).hasMatch()) return false;
        targets.insert(target);
    }
    return true;
}

QJsonObject stringRow(const QSqlQuery &query, const QStringList &columns)
{
    QJsonObject row;
    for (qsizetype index = 0; index < columns.size(); ++index) {
        const QVariant value = query.value(index);
        const QString text = value.metaType().id() == QMetaType::Double
            ? QString::number(value.toDouble(), 'g', 17) : value.toString();
        row.insert(columns.at(index), value.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(text));
    }
    return row;
}

bool readRows(const QSqlDatabase &database, const QString &sql,
              const QStringList &columns, QJsonArray *rows, qint64 *remainingBytes,
              const application::CancellationToken &cancellation)
{
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(sql)) return false;
    while (query.next()) {
        if (cancellation.isCancellationRequested()) return false;
        const QJsonObject row = stringRow(query, columns);
        const qint64 bytes = QJsonDocument(row).toJson(QJsonDocument::Compact).size();
        if (bytes > *remainingBytes) { *remainingBytes = -1; return false; }
        *remainingBytes -= bytes;
        rows->append(row);
    }
    return !query.lastError().isValid();
}

bool readSourceVersion(const QSqlDatabase &database, int *version)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT COUNT(*),MIN(version),MAX(version) FROM schema_migrations")) || !query.next()) return false;
    const int count = query.value(0).toInt();
    const int minimum = query.value(1).toInt();
    *version = query.value(2).toInt();
    return (*version == 24 || *version == 25) && minimum == 1 && count == *version;
}

bool retainedRelationshipsAreValid(const QSqlDatabase &database)
{
    static const QStringList tables{
        QStringLiteral("todo_tags"), QStringLiteral("todo_subtasks"),
        QStringLiteral("telegram_favorites"), QStringLiteral("youtube_favorites"),
        QStringLiteral("desktop_app_icons"), QStringLiteral("setup_apps"),
        QStringLiteral("setup_icons"), QStringLiteral("steam_game_images")};
    for (const QString &table : tables) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("PRAGMA foreign_key_check(") + table + QLatin1Char(')'))) return false;
        if (query.next()) return false;
    }
    return true;
}

bool copyAsset(const QString &sourceRoot, const QString &profileDirectory,
               const QString &importId, const QString &kind, const QString &ownerId,
               const QString &sourceFilename, const QString &expectedMime,
               qint64 expectedSize, const application::CancellationToken &cancellation,
               QJsonArray *assets, QJsonObject *metadata, QString *failureReason)
{
    *failureReason = QStringLiteral("unsafe-reference-or-invalid-metadata");
    const QString sourceDirectory = sourceDirectoryForKind(kind);
    const QString targetDirectory = targetDirectoryForKind(kind);
    if (sourceDirectory.isEmpty() || targetDirectory.isEmpty() || sourceFilename.isEmpty()
        || QFileInfo(sourceFilename).fileName() != sourceFilename
        || sourceFilename.contains(QLatin1Char(':')) || sourceFilename.contains(QLatin1Char('\\'))
        || sourceFilename.contains(QLatin1Char('/'))
        || sourceFilename == QStringLiteral(".") || sourceFilename == QStringLiteral("..")) return false;

    const QString sourceBase = QDir(sourceRoot).filePath(sourceDirectory);
    const QFileInfo baseInfo(sourceBase);
    const QFileInfo sourceInfo(QDir(sourceBase).filePath(sourceFilename));
    if (!sourceInfo.exists() || !baseInfo.exists()) {
        *failureReason = QStringLiteral("missing");
        return false;
    }
    const QString canonicalBase = baseInfo.canonicalFilePath();
    const QString canonicalFile = sourceInfo.canonicalFilePath();
    if (canonicalBase.isEmpty() || canonicalFile.isEmpty() || baseInfo.isSymLink() || baseInfo.isJunction()
        || !within(sourceRoot, canonicalBase)
        || sourceInfo.isSymLink() || sourceInfo.isJunction() || !sourceInfo.isFile()
        || QFileInfo(canonicalFile).absolutePath().compare(canonicalBase, Qt::CaseInsensitive) != 0
        || sourceInfo.size() <= 0) return false;

    const qint64 maximum = kind == QStringLiteral("wallpaper") ? maximumWallpaperBytes
        : kind == QStringLiteral("steam-artwork") ? maximumSteamArtworkBytes : maximumIconBytes;
    if (sourceInfo.size() > maximum || expectedSize <= 0 || sourceInfo.size() != expectedSize) return false;
    QFile source(canonicalFile);
    *failureReason = QStringLiteral("unreadable");
    if (!source.open(QIODevice::ReadOnly)) return false;
    const QByteArray header = source.read(16);
    const QString detectedMime = mimeFromHeader(header);
    *failureReason = QStringLiteral("unsupported-or-invalid-image");
    if (detectedMime.isEmpty() || detectedMime != expectedMime
        || (kind != QStringLiteral("desktop-icon") && kind != QStringLiteral("setup-icon")
            && detectedMime == QStringLiteral("image/x-icon"))) return false;
    if (!source.seek(0)) return false;

    QImageReader reader(&source);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead() || !reader.size().isValid() || reader.size().isEmpty()) return false;
    const QSize original = reader.size();
    if (original.width() > 32768 || original.height() > 32768
        || static_cast<qint64>(original.width()) * original.height() > 100'000'000) return false;
    QSize thumbnail = reader.size();
    thumbnail.scale(128, 128, Qt::KeepAspectRatio);
    reader.setScaledSize(QSize(qMax(1, thumbnail.width()), qMax(1, thumbnail.height())));
    if (reader.read().isNull() || !source.seek(0)) return false;

    if (kind == QStringLiteral("wallpaper") && !validId(ownerId)) return false;
    QString steamDigest;
    if (kind == QStringLiteral("steam-artwork")) {
        bool idOk = false;
        const quint64 appId = ownerId.toULongLong(&idOk);
        if (!idOk || appId == 0 || appId > 0xffffffffULL) return false;
        QCryptographicHash digest(QCryptographicHash::Sha256);
        if (!digest.addData(&source) || !source.seek(0)) return false;
        steamDigest = QString::fromLatin1(digest.result().toHex());
    }
    const QString assetId = kind == QStringLiteral("wallpaper") ? ownerId : generatedId();
    const QString filename = (kind == QStringLiteral("steam-artwork")
        ? ownerId + QLatin1Char('-') + steamDigest : assetId)
        + QLatin1Char('.') + extensionForMime(detectedMime);
    const QString stageRelative = QStringLiteral(".legacy-import-staging/") + importId
        + QStringLiteral("/assets/") + filename;
    const QString targetRelative = targetDirectory + QLatin1Char('/') + filename;
    if (!safeRelativeAsset(importId, stageRelative, targetRelative, kind)) return false;
    const QString stagedPath = QDir(profileDirectory).filePath(stageRelative);
    *failureReason = QStringLiteral("target-storage-failure");
    if (!ensureManagedDirectory(profileDirectory, QStringLiteral(".legacy-import-staging/")
                                  + importId + QStringLiteral("/assets"))) return false;
    QSaveFile target(stagedPath);
    if (!target.open(QIODevice::WriteOnly)) return false;
    QByteArray buffer(64 * 1024, '\0');
    qint64 copied = 0;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!source.atEnd()) {
        if (cancellation.isCancellationRequested()) {
            target.cancelWriting();
            return false;
        }
        const qint64 bytes = source.read(buffer.data(), buffer.size());
        if (bytes <= 0 || (copied += bytes) > maximum
            || target.write(buffer.constData(), bytes) != bytes) {
            target.cancelWriting();
            return false;
        }
        hash.addData(QByteArrayView(buffer.constData(), bytes));
    }
    if (copied != sourceInfo.size() || (!steamDigest.isEmpty() && QString::fromLatin1(hash.result().toHex()) != steamDigest)
        || !sameFileSnapshot(sourceInfo, QFileInfo(canonicalFile))
        || !target.commit()) return false;

    QJsonObject asset{
        {QStringLiteral("kind"), kind}, {QStringLiteral("ownerId"), ownerId},
        {QStringLiteral("stageRelative"), stageRelative},
        {QStringLiteral("targetRelative"), targetRelative},
        {QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex())},
        {QStringLiteral("byteSize"), QString::number(copied)}};
    assets->append(asset);
    *metadata = {{QStringLiteral("filename"), filename},
                 {QStringLiteral("mime_type"), detectedMime},
                 {QStringLiteral("byte_size"), QString::number(copied)}};
    failureReason->clear();
    return true;
}

bool writeJsonFile(const QString &path, const QJsonObject &object)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > maximumPlanBytes) { file.cancelWriting(); return false; }
    return file.write(bytes) == bytes.size() && file.commit();
}

bool loadJsonFile(const QString &path, QJsonObject *object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumPlanBytes) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    *object = document.object();
    return true;
}

bool matchesAssetFile(const QString &profileDirectory, const QString &path,
                       const QJsonObject &asset)
{
    if (!managedPath(profileDirectory, path)) return false;
    const QFileInfo info(path);
    const qint64 expectedSize = asset.value(QStringLiteral("byteSize")).toString().toLongLong();
    const QString expectedHash = asset.value(QStringLiteral("sha256")).toString();
    static const QRegularExpression hashPattern(QStringLiteral("^[a-f0-9]{64}$"));
    if (!info.isFile() || info.isSymLink() || expectedSize <= 0
        || expectedSize > maximumWallpaperBytes || info.size() != expectedSize
        || !hashPattern.match(expectedHash).hasMatch()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex()) == expectedHash;
}

bool removeStageTree(const QString &profileDirectory, const QString &path, int depth)
{
    const QFileInfo entry(path);
    if (depth > 4 || entry.isSymLink() || entry.isJunction()
        || !managedPath(profileDirectory, path)) return false;
    if (entry.isFile()) return QFile::remove(path);
    if (!entry.isDir()) return false;
    const QFileInfoList children = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &child : children) {
        if (!removeStageTree(profileDirectory, child.absoluteFilePath(), depth + 1)) return false;
    }
    return QDir().rmdir(path);
}

bool removeStageDirectory(const QString &profileDirectory, const QString &importId)
{
    if (!validId(importId)) return false;
    const QString path = stageRoot(profileDirectory, importId);
    if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction()) return false;
    if (!QFileInfo::exists(path)) return true;
    return removeStageTree(profileDirectory, path, 0);
}

} // namespace

domain::AppError LegacyDataImportService::prepare(
    const QString &sourceDatabasePath, const QString &profileDirectory,
    bool sourceClosedConfirmed, const application::CancellationToken &cancellation,
    QVariantMap *result)
{
    if (!result) return domain::AppError(domain::ErrorCategory::Validation);
    result->clear();
    const QString importId = generatedId();
    const QJsonObject initialReport{{QStringLiteral("committed"), false},
        {QStringLiteral("sourceCounts"), QJsonObject{}}, {QStringLiteral("skippedCounts"), QJsonObject{}},
        {QStringLiteral("missingAssets"), QJsonArray{}},
        {QStringLiteral("failures"), QJsonArray{QStringLiteral("The source snapshot, schema, or retained relationships failed validation.")}}};
    result->insert(QStringLiteral("importId"), importId);
    result->insert(QStringLiteral("reportJson"), QString::fromUtf8(QJsonDocument(initialReport).toJson(QJsonDocument::Compact)));
    if (!sourceClosedConfirmed || sourceDatabasePath.isEmpty()
        || profileDirectory.isEmpty()) return domain::AppError(domain::ErrorCategory::Validation);
    const QFileInfo sourceInfo(sourceDatabasePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() || sourceInfo.isSymLink()
        || sourceInfo.fileName().compare(QStringLiteral("database.db"), Qt::CaseInsensitive) != 0) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    const QString sourceDb = sourceInfo.canonicalFilePath();
    const QString sourceRoot = QFileInfo(sourceDb).absolutePath();
    const QString canonicalProfile = QFileInfo(profileDirectory).canonicalFilePath();
    if (canonicalProfile.isEmpty() || within(sourceRoot, canonicalProfile)
        || within(canonicalProfile, sourceRoot)) return domain::AppError(domain::ErrorCategory::Validation);
    const QString stage = stageRoot(profileDirectory, importId);
    if (QFileInfo::exists(stage) || QFileInfo(stage).isSymLink() || QFileInfo(stage).isJunction())
        return domain::AppError(domain::ErrorCategory::Conflict);
    const QString snapshot = QDir(stage).filePath(QStringLiteral("snapshot/database.db"));
    if (!ensureManagedDirectory(profileDirectory, QStringLiteral(".legacy-import-staging/")
                                  + importId + QStringLiteral("/snapshot")))
        return domain::AppError(domain::ErrorCategory::Storage);

    const QString sourceBasename = QFileInfo(sourceDb).fileName();
    const QStringList suffixes{QString(), QStringLiteral("-wal"), QStringLiteral("-shm")};
    QStringList sourceNames;
    for (const QString &suffix : suffixes) sourceNames.push_back(sourceBasename + suffix);
    QList<QFileInfo> before;
    before.reserve(sourceNames.size());
    for (const QString &name : sourceNames) before.push_back(QFileInfo(QDir(sourceRoot).filePath(name)));
    for (qsizetype i = 0; i < sourceNames.size(); ++i) {
        if (!before.at(i).exists()) continue;
        const QString copiedName = QStringLiteral("database.db") + suffixes.at(i);
        if (!copyClosedSnapshotFile(before.at(i).absoluteFilePath(),
                                    QDir(QFileInfo(snapshot).absolutePath()).filePath(copiedName),
                                    cancellation)) {
            removeStageDirectory(profileDirectory, importId);
            return cancellation.isCancellationRequested() ? domain::AppError::cancelled()
                : domain::AppError(domain::ErrorCategory::Storage);
        }
    }
    for (qsizetype i = 0; i < sourceNames.size(); ++i) {
        const QFileInfo after(QDir(sourceRoot).filePath(sourceNames.at(i)));
        if (!sameFileSnapshot(before.at(i), after)) {
            removeStageDirectory(profileDirectory, importId);
            return domain::AppError(domain::ErrorCategory::Conflict);
        }
    }
    if (cancellation.isCancellationRequested()) {
        removeStageDirectory(profileDirectory, importId);
        return domain::AppError::cancelled();
    }

    const QString connectionName = QStringLiteral("legacy-snapshot-") + importId;
    QJsonObject allRows;
    qint64 remainingMetadataBytes = maximumPlanBytes;
    QJsonObject report{{QStringLiteral("sourceCounts"), QJsonObject{}},
                       {QStringLiteral("skippedCounts"), QJsonObject{}},
                       {QStringLiteral("missingAssets"), QJsonArray{}},
                       {QStringLiteral("failures"), QJsonArray{}}};
    int schemaVersion = 0;
    bool readOk = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(snapshot);
        if (database.open()) {
            QSqlQuery integrity(database);
            QSqlQuery checkpoint(database);
            readOk = integrity.exec(QStringLiteral("PRAGMA integrity_check"))
                && integrity.next() && integrity.value(0).toString() == QStringLiteral("ok");
            integrity.finish();
            if (readOk) readOk = readSourceVersion(database, &schemaVersion)
                && retainedRelationshipsAreValid(database);
            if (readOk) {
                readOk = checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"))
                    && checkpoint.next() && checkpoint.value(0).toInt() == 0;
                checkpoint.finish();
            }
            if (readOk) {
                const QList<QPair<QString, QStringList>> sourceQueries{
                    {"tasks", {"id","title","description","is_completed","created_at","updated_at","due_date","priority","difficulty","revision"}},
                    {"tags", {"id","name"}},
                    {"task_tags", {"task_id","tag_id"}},
                    {"task_subtasks", {"id","task_id","title","is_completed","position","created_at"}},
                    {"currency_favorites", {"base_code","quote_code"}},
                    {"favorite_categories", {"id","name","source","color","created_at","display_order","updated_at"}},
                    {"telegram_favorites", {"username","added_at","category_id"}},
                    {"youtube_favorites", {"channel_id","added_at","category_id","username"}},
                    {"favorite_update_checkpoints", {"source","source_id","checked_through","last_success_at","last_attempted_at","last_error","updated_at"}},
                    {"favorite_update_seen_items", {"source","source_id","item_id","published_at","first_seen_at","updated_at"}},
                    {"favorite_news_items", {"source","source_id","item_id","published_at","discovered_at","payload_json","updated_at"}},
                    {"favorite_news_state", {"id","scan_started_at","errors_json","updated_at"}},
                    {"saved_weather", {"id","latitude","longitude","created_at","updated_at","forecast_json","forecast_updated_at"}},
                    {"desktop_apps", {"id","display_name","executable_path","created_at","updated_at"}},
                    {"setups", {"id","name","description","created_at","updated_at"}},
                    {"setup_apps", {"setup_id","app_id","position"}},
                    {"steam_game_settings", {"id","country_code","updated_at"}},
                    {"steam_games", {"id","steam_app_id","store_url","name","image_source_url","price_status","currency","regular_price_minor","current_price_minor","discount_percent","price_country_code","last_checked_at","last_attempted_at","last_refresh_error","created_at","updated_at"}},
                    {"user_wallpapers", {"id","display_name","filename","mime_type","byte_size","created_at"}},
                    {"desktop_app_icons", {"app_id","filename","mime_type","byte_size","updated_at"}},
                    {"setup_icons", {"setup_id","filename","mime_type","byte_size","updated_at"}},
                    {"steam_game_images", {"game_id","filename","mime_type","byte_size","updated_at"}},
                };
                for (const auto &entry : sourceQueries) {
                    QString sql = QStringLiteral("SELECT ") + entry.second.join(QLatin1Char(','))
                        + QStringLiteral(" FROM ") + entry.first;
                    if (entry.first == QStringLiteral("tasks")) sql += QStringLiteral(" ORDER BY id");
                    else if (entry.first == QStringLiteral("tags")) sql += QStringLiteral(" ORDER BY id");
                    else if (entry.first == QStringLiteral("task_tags")) sql += QStringLiteral(" ORDER BY todo_id,tag_id");
                    else if (entry.first == QStringLiteral("task_subtasks")) sql += QStringLiteral(" ORDER BY todo_id,position,id");
                    else if (entry.first == QStringLiteral("favorite_categories")) sql += QStringLiteral(" ORDER BY source,display_order,id");
                    else if (entry.first == QStringLiteral("setup_apps")) sql += QStringLiteral(" ORDER BY setup_id,position,app_id");
                    else sql += QStringLiteral(" ORDER BY 1");
                    QStringList aliases = entry.second;
                    QJsonArray tableRows;
                    if (entry.first == QStringLiteral("tasks")) {
                        sql = QStringLiteral("SELECT id,title,COALESCE(description,''),is_completed,created_at,updated_at,due_date,priority,difficulty,revision FROM todos ORDER BY id");
                        aliases = {"id","title","description","is_completed","created_at","updated_at","due_date","priority","difficulty","revision"};
                    } else if (entry.first == QStringLiteral("task_tags")) {
                        sql = QStringLiteral("SELECT todo_id,tag_id FROM todo_tags ORDER BY todo_id,tag_id");
                        aliases = {"task_id","tag_id"};
                    } else if (entry.first == QStringLiteral("task_subtasks")) {
                        sql = QStringLiteral("SELECT id,todo_id,title,is_completed,position,created_at FROM todo_subtasks ORDER BY todo_id,position,id");
                        aliases = {"id","task_id","title","is_completed","position","created_at"};
                    } else if (entry.first == QStringLiteral("currency_favorites")) {
                        sql = QStringLiteral("SELECT base,quote FROM favorite_rates ORDER BY base,quote");
                        aliases = {"base_code","quote_code"};
                    } else if (entry.first == QStringLiteral("saved_weather")) {
                        sql = QStringLiteral("SELECT id,latitude,longitude,created_at,updated_at,forecast_json,forecast_updated_at FROM location ORDER BY id");
                    } else if (entry.first == QStringLiteral("user_wallpapers")) {
                        sql = QStringLiteral("SELECT id,display_name,filename,mime_type,byte_size,created_at FROM user_wallpapers ORDER BY created_at,id");
                        aliases = {"id","display_name","filename","mime_type","byte_size","created_at"};
                    } else if (entry.first == QStringLiteral("steam_game_settings")) {
                        sql = QStringLiteral("SELECT id,country_code,updated_at FROM steam_game_settings ORDER BY id");
                    }
                    readOk = readRows(database, sql, aliases, &tableRows, &remainingMetadataBytes, cancellation);
                    if (!readOk) break;
                    const QString key = entry.first == QStringLiteral("user_wallpapers")
                        ? QStringLiteral("source_wallpapers") : entry.first;
                    allRows.insert(key, tableRows);
                }
                if (readOk) {
                    QJsonArray preferences;
                    QSqlQuery state(database);
                    state.prepare(QStringLiteral(
                        "SELECT key,value FROM app_state WHERE key IN (?,?,?,?,?,?) ORDER BY key"));
                    state.addBindValue(QStringLiteral("todos.week_start"));
                    state.addBindValue(QStringLiteral("selected_wallpaper"));
                    state.addBindValue(QStringLiteral("previous_opened_at"));
                    state.addBindValue(QStringLiteral("current_opened_at"));
                    state.addBindValue(QStringLiteral("previous_refresh_at"));
                    state.addBindValue(QStringLiteral("last_refresh_at"));
                    readOk = state.exec();
                    while (readOk && state.next()) {
                        QString key = state.value(0).toString();
                        if (key == QStringLiteral("todos.week_start")) key = QStringLiteral("tasks.week_start");
                        else if (key == QStringLiteral("selected_wallpaper")) key = QStringLiteral("appearance.selected_wallpaper");
                        else key = QStringLiteral("favorite_updates.") + key;
                        preferences.append(QJsonObject{{QStringLiteral("key"), key},
                                                       {QStringLiteral("value"), state.value(1).toString()}});
                    }
                    if (readOk) allRows.insert(QStringLiteral("preferences"), preferences);
                }
            }
            database.close();
        }
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }

    if (!readOk || cancellation.isCancellationRequested()) {
        if (remainingMetadataBytes < 0) {
            QJsonObject limitedReport = initialReport;
            limitedReport.insert(QStringLiteral("failures"), QJsonArray{QStringLiteral("Retained import metadata exceeds the 64 MiB plan limit.")});
            result->insert(QStringLiteral("reportJson"), QString::fromUtf8(QJsonDocument(limitedReport).toJson(QJsonDocument::Compact)));
        }
        removeStageDirectory(profileDirectory, importId);
        return cancellation.isCancellationRequested() ? domain::AppError::cancelled()
            : domain::AppError(domain::ErrorCategory::Validation);
    }

    QFile snapshotFile(snapshot);
    QCryptographicHash snapshotHash(QCryptographicHash::Sha256);
    if (!snapshotFile.open(QIODevice::ReadOnly) || !snapshotHash.addData(&snapshotFile)) {
        removeStageDirectory(profileDirectory, importId);
        return domain::AppError(domain::ErrorCategory::Storage);
    }
    snapshotFile.close();
    report.insert(QStringLiteral("sourceSchemaVersion"), schemaVersion);
    report.insert(QStringLiteral("snapshotSha256"), QString::fromLatin1(snapshotHash.result().toHex()));

    QJsonObject sourceCounts;
    for (auto it = allRows.constBegin(); it != allRows.constEnd(); ++it) {
        if (it.value().isArray()) sourceCounts.insert(
            it.key() == QStringLiteral("source_wallpapers") ? QStringLiteral("wallpapers") : it.key(),
            it.value().toArray().size());
    }
    report.insert(QStringLiteral("sourceCounts"), sourceCounts);
    auto invalidData = [&](const QString &message, domain::ErrorCategory category = domain::ErrorCategory::Validation) {
        report.insert(QStringLiteral("committed"), false);
        report.insert(QStringLiteral("failures"), QJsonArray{message});
        result->insert(QStringLiteral("reportJson"), QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Compact)));
        removeStageDirectory(profileDirectory, importId);
        return domain::AppError(category);
    };

    // Apply the source-to-target column and normalization rules after reading
    // only retained tables. Source IDs remain stable where their target keys permit it.
    QJsonArray tasks = allRows.value(QStringLiteral("tasks")).toArray();
    QJsonArray normalizedTasks;
    QHash<QString, QString> taskDifficulties;
    for (const QJsonValue &value : tasks) {
        QJsonObject task = value.toObject();
        const QString normalizedDifficulty = task.value(QStringLiteral("difficulty")).toString().trimmed().toLower();
        task.insert(QStringLiteral("difficulty"), normalizedDifficulty.isEmpty()
            ? QJsonValue(QJsonValue::Null) : QJsonValue(normalizedDifficulty));
        const QString dueDate = task.value(QStringLiteral("due_date")).toString();
        const QDate date = QDate::fromString(dueDate, Qt::ISODate);
        QString priority = task.value(QStringLiteral("priority")).toString().trimmed().toLower();
        if (priority.isEmpty()) priority = QStringLiteral("medium");
        const QString difficulty = task.value(QStringLiteral("difficulty")).toString();
        if (task.value(QStringLiteral("id")).toString().toLongLong() <= 0
            || task.value(QStringLiteral("title")).toString().trimmed().isEmpty()
            || (!dueDate.isEmpty() && (!date.isValid() || date.toString(Qt::ISODate) != dueDate))
            || !domain::isValidIsoDate(dueDate)
            || (priority != QStringLiteral("low") && priority != QStringLiteral("medium") && priority != QStringLiteral("high"))
            || (!difficulty.isEmpty() && difficulty != QStringLiteral("easy") && difficulty != QStringLiteral("medium") && difficulty != QStringLiteral("hard"))
            || task.value(QStringLiteral("revision")).toString().toLongLong() <= 0) {
            return invalidData(QStringLiteral("A retained task failed validation."));
        }
        task.insert(QStringLiteral("priority"), priority);
        if (dueDate.isEmpty()) task.insert(QStringLiteral("due_date"), QJsonValue(QJsonValue::Null));
        taskDifficulties.insert(task.value(QStringLiteral("id")).toString(), difficulty);
        normalizedTasks.append(task);
    }
    allRows.insert(QStringLiteral("tasks"), normalizedTasks);

    QJsonArray tags = allRows.value(QStringLiteral("tags")).toArray();
    QJsonArray normalizedTags;
    QSet<QString> tagKeys;
    for (QJsonValue value : tags) {
        QJsonObject tag = value.toObject();
        const QString name = tag.value(QStringLiteral("name")).toString().trimmed();
        if (tag.value(QStringLiteral("id")).toString().toLongLong() <= 0
            || name.isEmpty() || name.toUcs4().size() > 64 || tagKeys.contains(name.toCaseFolded()))
            return invalidData(QStringLiteral("A task tag is invalid or collides after normalization."));
        tagKeys.insert(name.toCaseFolded());
        tag.insert(QStringLiteral("name"), name);
        tag.insert(QStringLiteral("name_normalized"), name.toCaseFolded());
        normalizedTags.append(tag);
    }
    allRows.insert(QStringLiteral("tags"), normalizedTags);

    QJsonArray categories = allRows.value(QStringLiteral("favorite_categories")).toArray();
    QSet<QString> categoryKeys;
    QHash<QString, QString> categorySources;
    for (qsizetype i = 0; i < categories.size(); ++i) {
        QJsonObject category = categories.at(i).toObject();
        const QString source = category.value(QStringLiteral("source")).toString().toLower();
        const QString name = category.value(QStringLiteral("name")).toString().trimmed();
        bool orderOk = false;
        const int order = category.value(QStringLiteral("display_order")).toString().toInt(&orderOk);
        if (category.value(QStringLiteral("id")).toString().toLongLong() <= 0
            || (source != QStringLiteral("telegram") && source != QStringLiteral("youtube"))
            || name.isEmpty() || name.toUcs4().size() > 80 || !orderOk || order < 0 || order > 1'000'000) {
            return invalidData(QStringLiteral("A favorite category failed validation."));
        }
        const QString key = source + QLatin1Char(':') + name.toCaseFolded();
        if (categoryKeys.contains(key)) return invalidData(QStringLiteral("Favorite categories collide after normalization."));
        categoryKeys.insert(key);
        categorySources.insert(category.value(QStringLiteral("id")).toString(), source);
        const QString color = category.value(QStringLiteral("color")).toString();
        const QString normalizedColor = domain::normalizeFavoriteCategoryColor(color);
        if (!color.trimmed().isEmpty() && normalizedColor.isEmpty())
            return invalidData(QStringLiteral("A favorite category color is invalid."));
        category.insert(QStringLiteral("color"), normalizedColor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(normalizedColor));
        category.insert(QStringLiteral("source"), source);
        category.insert(QStringLiteral("name"), name);
        category.insert(QStringLiteral("name_normalized"), name.toCaseFolded());
        categories.replace(i, category);
    }
    allRows.insert(QStringLiteral("favorite_categories"), categories);

    QJsonObject skippedCounts;
    QJsonArray missingAssets;
    QJsonArray assetFailures;
    QJsonArray warnings;
    QHash<QString, int> tagCounts;
    for (const QJsonValue &value : allRows.value(QStringLiteral("task_tags")).toArray()) {
        const QString taskId = value.toObject().value(QStringLiteral("task_id")).toString();
        if (++tagCounts[taskId] > 32) return invalidData(QStringLiteral("A task exceeds the retained tag limit."));
    }
    QHash<QString, int> subtaskCounts;
    for (const QJsonValue &value : allRows.value(QStringLiteral("task_subtasks")).toArray()) {
        const QJsonObject row = value.toObject();
        const QString taskId = row.value(QStringLiteral("task_id")).toString();
        bool positionOk = false;
        const int position = row.value(QStringLiteral("position")).toString().toInt(&positionOk);
        if (taskDifficulties.value(taskId) != QStringLiteral("hard")
            || row.value(QStringLiteral("id")).toString().toLongLong() <= 0
            || row.value(QStringLiteral("title")).toString().trimmed().isEmpty()
            || !positionOk || position < 0
            || ++subtaskCounts[taskId] > 100)
            return invalidData(QStringLiteral("Task subtasks violate the retained hard-task rules."));
    }
    for (const QString &source : {QStringLiteral("telegram"), QStringLiteral("youtube")}) {
        const QString table = source + QStringLiteral("_favorites");
        QJsonArray favorites = allRows.value(table).toArray();
        QSet<QString> identities;
        for (qsizetype i = 0; i < favorites.size(); ++i) {
            QJsonObject row = favorites.at(i).toObject();
            const QString identityColumn = source == QStringLiteral("telegram")
                ? QStringLiteral("username") : QStringLiteral("channel_id");
            QString identity = row.value(identityColumn).toString();
            bool channelId = false;
            identity = source == QStringLiteral("telegram") ? domain::normalizeTelegramChannel(identity)
                : domain::normalizeYouTubeReference(identity, &channelId);
            if (identity.isEmpty() || (source == QStringLiteral("youtube") && !channelId)
                || identities.contains(identity)) return invalidData(QStringLiteral("A source favorite has an invalid or duplicate identity."));
            identities.insert(identity);
            const QJsonValue category = row.value(QStringLiteral("category_id"));
            if (!category.isNull() && categorySources.value(category.toString()) != source)
                return invalidData(QStringLiteral("A favorite references a category for another source."));
            row.insert(identityColumn, identity);
            favorites.replace(i, row);
        }
        allRows.insert(table, favorites);
    }
    for (const QString &table : {QStringLiteral("favorite_update_checkpoints"),
                                  QStringLiteral("favorite_update_seen_items"), QStringLiteral("favorite_news_items")}) {
        QJsonArray providerRows = allRows.value(table).toArray();
        for (qsizetype i = 0; i < providerRows.size(); ++i) {
            QJsonObject row = providerRows.at(i).toObject();
            const QString source = row.value(QStringLiteral("source")).toString();
            if (domain::normalizeFavoriteSource(source).isEmpty())
                return invalidData(QStringLiteral("Provider state has an unsupported source."));
            if (source == QStringLiteral("telegram")) {
                const QString identity = domain::normalizeTelegramChannel(row.value(QStringLiteral("source_id")).toString());
                if (identity.isEmpty()) return invalidData(QStringLiteral("Provider state has an invalid Telegram identity."));
                row.insert(QStringLiteral("source_id"), identity);
            }
            if (table == QStringLiteral("favorite_news_items")) {
                QJsonParseError error;
                const QJsonDocument payload = QJsonDocument::fromJson(row.value(QStringLiteral("payload_json")).toString().toUtf8(), &error);
                if (error.error != QJsonParseError::NoError || !payload.isObject())
                    return invalidData(QStringLiteral("A stored favorite-news payload is malformed."));
                const QJsonObject fields = payload.object();
                const QUrl link = source == QStringLiteral("telegram")
                    ? domain::telegramPostUrl(row.value(QStringLiteral("source_id")).toString(), fields.value(QStringLiteral("postId")).toString())
                    : domain::youTubeVideoUrl(fields.value(QStringLiteral("videoId")).toString());
                if (link.isEmpty()) return invalidData(QStringLiteral("A favorite-news payload is missing its stable post/video identity."));
            }
            providerRows.replace(i, row);
        }
        allRows.insert(table, providerRows);
    }
    QJsonArray locations = allRows.value(QStringLiteral("saved_weather")).toArray();
    for (qsizetype i = 0; i < locations.size(); ++i) {
        QJsonObject row = locations.at(i).toObject();
        bool latitudeOk = false, longitudeOk = false;
        const double latitude = row.value(QStringLiteral("latitude")).toString().toDouble(&latitudeOk);
        const double longitude = row.value(QStringLiteral("longitude")).toString().toDouble(&longitudeOk);
        if (!latitudeOk || !longitudeOk || !domain::isValidWeatherCoordinates(latitude, longitude))
            return invalidData(QStringLiteral("The saved weather location has invalid coordinates."));
        const QString forecast = row.value(QStringLiteral("forecast_json")).toString();
        if (!forecast.isEmpty() && !providers::parseWeatherForecast(forecast.toUtf8()).succeeded()) {
            row.insert(QStringLiteral("forecast_json"), QJsonValue(QJsonValue::Null));
            row.insert(QStringLiteral("forecast_updated_at"), QJsonValue(QJsonValue::Null));
            skippedCounts.insert(QStringLiteral("weather_forecast_cache"), 1);
            warnings.append(QStringLiteral("The saved location was retained; its malformed forecast cache was skipped."));
        }
        locations.replace(i, row);
    }
    allRows.insert(QStringLiteral("saved_weather"), locations);

    for (const QJsonValue &value : allRows.value(QStringLiteral("favorite_news_state")).toArray()) {
        QJsonParseError error;
        const QJsonDocument errors = QJsonDocument::fromJson(value.toObject().value(QStringLiteral("errors_json")).toString().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !errors.isArray())
            return invalidData(QStringLiteral("The favorite-news scan state is malformed."));
    }
    QSet<QString> setupNames;
    for (const QJsonValue &value : allRows.value(QStringLiteral("setups")).toArray()) {
        const QJsonObject row = value.toObject();
        const QString name = row.value(QStringLiteral("name")).toString().trimmed();
        if (row.value(QStringLiteral("id")).toString().toLongLong() <= 0
            || name.isEmpty() || name.toUcs4().size() > 120
            || row.value(QStringLiteral("description")).toString().toUcs4().size() > 300
            || setupNames.contains(name.toCaseFolded()))
            return invalidData(QStringLiteral("A Setup name, description, or identity is invalid or collides after normalization."));
        setupNames.insert(name.toCaseFolded());
    }
    for (const QJsonValue &value : allRows.value(QStringLiteral("setup_apps")).toArray()) {
        bool ok = false;
        const int position = value.toObject().value(QStringLiteral("position")).toString().toInt(&ok);
        if (!ok || position < 0) return invalidData(QStringLiteral("A Setup membership has an invalid position."));
    }

    for (const QJsonValue &value : allRows.value(QStringLiteral("desktop_apps")).toArray()) {
        const QJsonObject row = value.toObject();
        const QString path = row.value(QStringLiteral("executable_path")).toString();
        const QFileInfo executable(path);
        static const QRegularExpression windowsAbsolute(QStringLiteral("^(?:[A-Za-z]:[\\\\/]|\\\\\\\\)"));
        if (row.value(QStringLiteral("id")).toString().toLongLong() <= 0
            || !domain::normalizeDesktopApplicationName(row.value(QStringLiteral("display_name")).toString())
            || (!executable.isAbsolute() && !windowsAbsolute.match(path).hasMatch())
            || executable.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0
            || path.contains(QChar::Null) || path.contains(QLatin1Char('\n')))
            return invalidData(QStringLiteral("A saved application has an invalid display name or executable path."));
        if (!executable.exists()) warnings.append(QStringLiteral("Saved application %1 is currently unavailable; its record and Setup membership were retained.")
            .arg(row.value(QStringLiteral("id")).toString()));
    }
    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    QJsonArray games = allRows.value(QStringLiteral("steam_games")).toArray();
    QHash<QString, QString> steamAppIds;
    for (qsizetype i = 0; i < games.size(); ++i) {
        QJsonObject game = games.at(i).toObject();
        const auto store = domain::normalizeSteamStoreUrl(game.value(QStringLiteral("store_url")).toString());
        const QString artwork = game.value(QStringLiteral("image_source_url")).toString();
        const QString status = game.value(QStringLiteral("price_status")).toString();
        const QString currency = game.value(QStringLiteral("currency")).toString();
        const QString country = game.value(QStringLiteral("price_country_code")).toString();
        const QString name = game.value(QStringLiteral("name")).toString();
        bool discountOk = false;
        const int discount = game.value(QStringLiteral("discount_percent")).toString().toInt(&discountOk);
        auto validMinor = [&](const QString &key) {
            if (game.value(key).isNull()) return true;
            bool ok = false;
            const qint64 amount = game.value(key).toString().toLongLong(&ok);
            return ok && amount >= 0;
        };
        if (game.value(QStringLiteral("id")).toString().toLongLong() <= 0
            || !store.isValid() || QString::number(store.appId) != game.value(QStringLiteral("steam_app_id")).toString()
            || name.trimmed().isEmpty() || name.toUcs4().size() > 300
            || (!artwork.isEmpty() && domain::normalizeSteamArtworkUrl(artwork).isEmpty())
            || !countryPattern.match(country).hasMatch()
            || !discountOk || discount < 0 || discount > 100
            || (!currency.isEmpty() && !currencyPattern.match(currency).hasMatch())
            || !validMinor(QStringLiteral("regular_price_minor")) || !validMinor(QStringLiteral("current_price_minor"))
            || (status != QStringLiteral("priced") && status != QStringLiteral("free") && status != QStringLiteral("unavailable"))
            || (status == QStringLiteral("priced") && (currency.isEmpty()
                || game.value(QStringLiteral("regular_price_minor")).isNull()
                || game.value(QStringLiteral("current_price_minor")).isNull())))
            return invalidData(QStringLiteral("A Steam record has invalid identity, price, currency, country, or artwork fields."));
        game.insert(QStringLiteral("store_url"), store.canonicalUrl);
        steamAppIds.insert(game.value(QStringLiteral("id")).toString(), QString::number(store.appId));
        games.replace(i, game);
    }
    allRows.insert(QStringLiteral("steam_games"), games);
    const QJsonArray settings = allRows.value(QStringLiteral("steam_game_settings")).toArray();
    if (settings.size() != 1 || settings.first().toObject().value(QStringLiteral("id")).toString() != QStringLiteral("1")
        || !domain::isSupportedSteamCountry(settings.first().toObject().value(QStringLiteral("country_code")).toString()))
        return invalidData(QStringLiteral("Steam settings contain an unsupported country."));

    QJsonArray assets;
    bool targetStorageFailed = false;
    const QJsonArray wallpaperRows = allRows.take(QStringLiteral("source_wallpapers")).toArray();
    QJsonArray wallpapers;
    for (const QJsonValue &value : wallpaperRows) {
        const QJsonObject source = value.toObject();
        QJsonObject metadata;
        QString failureReason;
        const QString ownerId = source.value(QStringLiteral("id")).toString();
        if (!copyAsset(sourceRoot, profileDirectory, importId, QStringLiteral("wallpaper"), ownerId,
                       source.value(QStringLiteral("filename")).toString(),
                       source.value(QStringLiteral("mime_type")).toString(),
                       source.value(QStringLiteral("byte_size")).toString().toLongLong(),
                       cancellation, &assets, &metadata, &failureReason)) {
            if (failureReason == QStringLiteral("target-storage-failure")) targetStorageFailed = true;
            if (failureReason == QStringLiteral("missing")) missingAssets.append(QStringLiteral("wallpaper:") + ownerId);
            else assetFailures.append(QJsonObject{{"kind","wallpaper"},{"ownerId",ownerId},{"reason",failureReason}});
            skippedCounts.insert(QStringLiteral("wallpapers"), skippedCounts.value(QStringLiteral("wallpapers")).toInt() + 1);
            continue;
        }
        QJsonObject target = source;
        target.insert(QStringLiteral("filename"), metadata.value(QStringLiteral("filename")));
        target.insert(QStringLiteral("mime_type"), metadata.value(QStringLiteral("mime_type")));
        target.insert(QStringLiteral("byte_size"), metadata.value(QStringLiteral("byte_size")));
        wallpapers.append(target);
    }
    allRows.insert(QStringLiteral("wallpapers"), wallpapers);

    auto stageMetadataRows = [&](const QString &sourceKey, const QString &targetKey,
                                  const QString &kind, const QString &ownerField) {
        const QJsonArray sourceRows = allRows.value(sourceKey).toArray();
        QJsonArray targetRows;
        for (const QJsonValue &value : sourceRows) {
            const QJsonObject source = value.toObject();
            const QString owner = source.value(ownerField).toString();
            QJsonObject metadata;
            QString failureReason;
            QString assetIdentity = owner;
            if (kind == QStringLiteral("steam-artwork")) assetIdentity = steamAppIds.value(owner);
            if (!copyAsset(sourceRoot, profileDirectory, importId, kind, assetIdentity,
                           source.value(QStringLiteral("filename")).toString(),
                           source.value(QStringLiteral("mime_type")).toString(),
                           source.value(QStringLiteral("byte_size")).toString().toLongLong(),
                           cancellation, &assets, &metadata, &failureReason)) {
                if (failureReason == QStringLiteral("target-storage-failure")) targetStorageFailed = true;
                if (failureReason == QStringLiteral("missing")) missingAssets.append(kind + QLatin1Char(':') + owner);
                else assetFailures.append(QJsonObject{{"kind",kind},{"ownerId",owner},{"reason",failureReason}});
                skippedCounts.insert(targetKey, skippedCounts.value(targetKey).toInt() + 1);
                continue;
            }
            QJsonObject target = source;
            for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) target.insert(it.key(), it.value());
            targetRows.append(target);
        }
        if (sourceKey != targetKey) allRows.remove(sourceKey);
        allRows.insert(targetKey, targetRows);
    };
    stageMetadataRows(QStringLiteral("desktop_app_icons"), QStringLiteral("desktop_app_icons"),
                      QStringLiteral("desktop-icon"), QStringLiteral("app_id"));
    stageMetadataRows(QStringLiteral("setup_icons"), QStringLiteral("setup_icons"),
                      QStringLiteral("setup-icon"), QStringLiteral("setup_id"));
    stageMetadataRows(QStringLiteral("steam_game_images"), QStringLiteral("steam_game_images"),
                      QStringLiteral("steam-artwork"), QStringLiteral("game_id"));
    report.insert(QStringLiteral("skippedCounts"), skippedCounts);
    report.insert(QStringLiteral("missingAssets"), missingAssets);
    report.insert(QStringLiteral("assetFailures"), assetFailures);
    if (cancellation.isCancellationRequested()) {
        removeStageDirectory(profileDirectory, importId);
        return domain::AppError::cancelled();
    }
    if (targetStorageFailed) return invalidData(QStringLiteral("Retained asset staging failed in the Qt profile."), domain::ErrorCategory::Storage);

    // Source favorite currencies are case-sensitive while the target stores canonical pairs.
    QJsonArray currencies;
    QSet<QString> currencyPairs;
    for (const QJsonValue &value : allRows.value(QStringLiteral("currency_favorites")).toArray()) {
        QJsonObject row = value.toObject();
        const QString base = row.value(QStringLiteral("base_code")).toString().trimmed().toUpper();
        const QString quote = row.value(QStringLiteral("quote_code")).toString().trimmed().toUpper();
        static const QRegularExpression code(QStringLiteral("^[A-Z]{3}$"));
        const QString pair = base + QLatin1Char('/') + quote;
        if (!code.match(base).hasMatch() || !code.match(quote).hasMatch() || currencyPairs.contains(pair)) {
            skippedCounts.insert(QStringLiteral("currency_favorites"), skippedCounts.value(QStringLiteral("currency_favorites")).toInt() + 1);
            continue;
        }
        currencyPairs.insert(pair);
        row.insert(QStringLiteral("base_code"), base);
        row.insert(QStringLiteral("quote_code"), quote);
        currencies.append(row);
    }
    allRows.insert(QStringLiteral("currency_favorites"), currencies);

    const QSet<QString> builtinSelections{QStringLiteral("builtin:original"), QStringLiteral("builtin:sandrone"),
                                        QStringLiteral("builtin:hu-tao"), QStringLiteral("builtin:skirk")};
    QSet<QString> wallpaperSelections;
    for (const QJsonValue &value : wallpapers)
        wallpaperSelections.insert(QStringLiteral("custom:") + value.toObject().value(QStringLiteral("id")).toString());
    QJsonArray preferences;
    for (const QJsonValue &value : allRows.value(QStringLiteral("preferences")).toArray()) {
        QJsonObject preference = value.toObject();
        const QString key = preference.value(QStringLiteral("key")).toString();
        QString stored = preference.value(QStringLiteral("value")).toString();
        if (key == QStringLiteral("appearance.selected_wallpaper")
            && !builtinSelections.contains(stored) && !wallpaperSelections.contains(stored)) {
            skippedCounts.insert(QStringLiteral("selected_wallpaper"), 1);
            warnings.append(QStringLiteral("The selected wallpaper was unavailable; Qt will use its default selection."));
            continue;
        }
        if (key == QStringLiteral("tasks.week_start")) {
            bool ok = false;
            const int weekday = stored.toInt(&ok);
            if (!ok || weekday < 0 || weekday > 6) {
                stored = QStringLiteral("1");
                warnings.append(QStringLiteral("An invalid week-start preference used the reference Monday default."));
            }
            preference.insert(QStringLiteral("value"), stored);
        }
        preferences.append(preference);
    }
    allRows.insert(QStringLiteral("preferences"), preferences);

    report.insert(QStringLiteral("skippedCounts"), skippedCounts);
    report.insert(QStringLiteral("missingAssets"), missingAssets);
    report.insert(QStringLiteral("assetFailures"), assetFailures);
    report.insert(QStringLiteral("warnings"), warnings);
    report.insert(QStringLiteral("browserStorageImported"), false);
    report.insert(QStringLiteral("committed"), false);
    report.insert(QStringLiteral("failures"), QJsonArray{});
    result->insert(QStringLiteral("reportJson"), QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Compact)));
    const QString planPath = QDir(stage).filePath(QStringLiteral("plan.json"));
    const QJsonObject plan{{QStringLiteral("importId"), importId},
                           {QStringLiteral("sourceSchemaVersion"), schemaVersion},
                           {QStringLiteral("rows"), allRows},
                           {QStringLiteral("assets"), assets},
                           {QStringLiteral("report"), report}};
    if (cancellation.isCancellationRequested() || !writeJsonFile(planPath, plan)) {
        removeStageDirectory(profileDirectory, importId);
        return cancellation.isCancellationRequested() ? domain::AppError::cancelled()
            : domain::AppError(domain::ErrorCategory::Storage);
    }
    result->insert(QStringLiteral("importId"), importId);
    result->insert(QStringLiteral("planPath"), planPath);
    result->insert(QStringLiteral("stageDirectory"), stage);
    result->insert(QStringLiteral("sourceSchemaVersion"), schemaVersion);
    result->insert(QStringLiteral("reportJson"), QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Compact)));
    return domain::AppError::none();
}

domain::AppError LegacyDataImportService::installAssets(
    const QString &profileDirectory, const QString &importId, const QString &planPath,
    const application::CancellationToken &cancellation)
{
    if (!validId(importId)) return domain::AppError(domain::ErrorCategory::Validation);
    const QString expectedPlan = QDir(stageRoot(profileDirectory, importId)).filePath(QStringLiteral("plan.json"));
    if (QDir::cleanPath(planPath) != QDir::cleanPath(expectedPlan)
        || !managedPath(profileDirectory, planPath)
        || QFileInfo::exists(journalPath(profileDirectory)))
        return domain::AppError(domain::ErrorCategory::Conflict);
    QJsonObject plan;
    if (!loadJsonFile(planPath, &plan) || plan.value(QStringLiteral("importId")).toString() != importId
        || !plan.value(QStringLiteral("assets")).isArray()) return domain::AppError(domain::ErrorCategory::Validation);
    const QJsonArray assets = plan.value(QStringLiteral("assets")).toArray();
    QSet<QString> targetFiles;
    for (const QJsonValue &value : assets) {
        if (!value.isObject()) return domain::AppError(domain::ErrorCategory::Validation);
        const QJsonObject asset = value.toObject();
        const QString kind = asset.value(QStringLiteral("kind")).toString();
        const QString stageRelative = asset.value(QStringLiteral("stageRelative")).toString();
        const QString targetRelative = asset.value(QStringLiteral("targetRelative")).toString();
        if (!safeRelativeAsset(importId, stageRelative, targetRelative, kind))
            return domain::AppError(domain::ErrorCategory::Validation);
        const QString stagedPath = QDir(profileDirectory).filePath(stageRelative);
        const QString targetPath = QDir(profileDirectory).filePath(targetRelative);
        if (targetFiles.contains(targetRelative) || QFileInfo::exists(targetPath)
            || !matchesAssetFile(profileDirectory, stagedPath, asset)
            || !ensureManagedDirectory(profileDirectory, targetDirectoryForKind(kind)))
            return domain::AppError(domain::ErrorCategory::Validation);
        targetFiles.insert(targetRelative);
    }
    const QJsonObject journal{{QStringLiteral("importId"), importId},
                              {QStringLiteral("stageRelative"), QStringLiteral(".legacy-import-staging/") + importId},
                              {QStringLiteral("assets"), assets}};
    if (!managedPath(profileDirectory, journalPath(profileDirectory))
        || !writeJsonFile(journalPath(profileDirectory), journal))
        return domain::AppError(domain::ErrorCategory::Storage);

    for (const QJsonValue &value : assets) {
        if (cancellation.isCancellationRequested()) {
            rollback(profileDirectory, importId);
            return domain::AppError::cancelled();
        }
        const QJsonObject asset = value.toObject();
        const QString source = QDir(profileDirectory).filePath(asset.value(QStringLiteral("stageRelative")).toString());
        const QString target = QDir(profileDirectory).filePath(asset.value(QStringLiteral("targetRelative")).toString());
        if (!managedPath(profileDirectory, source) || !managedPath(profileDirectory, target)
            || !QFileInfo::exists(source) || QFileInfo::exists(target) || !QFile::rename(source, target)) {
            rollback(profileDirectory, importId);
            return domain::AppError(domain::ErrorCategory::Storage);
        }
    }
    if (cancellation.isCancellationRequested()) {
        rollback(profileDirectory, importId);
        return domain::AppError::cancelled();
    }
    return domain::AppError::none();
}

domain::AppError LegacyDataImportService::rollback(const QString &profileDirectory, const QString &importId)
{
    if (!validId(importId)) return domain::AppError(domain::ErrorCategory::Validation);
    const QString path = journalPath(profileDirectory);
    if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction())
        return domain::AppError(domain::ErrorCategory::Validation);
    if (!QFileInfo::exists(path)) {
        return removeStageDirectory(profileDirectory, importId) ? domain::AppError::none()
            : domain::AppError(domain::ErrorCategory::Storage);
    }
    QJsonObject journal;
    if (!managedPath(profileDirectory, path) || !loadJsonFile(path, &journal)
        || !validJournal(journal, importId))
        return domain::AppError(domain::ErrorCategory::Validation);
    bool cleaned = true;
    for (const QJsonValue &value : journal.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        const QString relative = asset.value(QStringLiteral("targetRelative")).toString();
        if (!safeRelativeAsset(importId, asset.value(QStringLiteral("stageRelative")).toString(),
                               relative, asset.value(QStringLiteral("kind")).toString()))
            return domain::AppError(domain::ErrorCategory::Validation);
        const QString target = QDir(profileDirectory).filePath(relative);
        if (!QFileInfo::exists(target)) continue;
        if (!matchesAssetFile(profileDirectory, target, asset) || !QFile::remove(target)) cleaned = false;
    }
    if (!cleaned || !removeStageDirectory(profileDirectory, importId) || !QFile::remove(path))
        return domain::AppError(domain::ErrorCategory::Storage);
    return domain::AppError::none();
}

domain::AppError LegacyDataImportService::finish(const QString &profileDirectory, const QString &importId)
{
    if (!validId(importId)) return domain::AppError(domain::ErrorCategory::Validation);
    const QString path = journalPath(profileDirectory);
    if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction())
        return domain::AppError(domain::ErrorCategory::Validation);
    if (!QFileInfo::exists(path)) return removeStageDirectory(profileDirectory, importId)
        ? domain::AppError::none() : domain::AppError(domain::ErrorCategory::Storage);
    QJsonObject journal;
    if (!managedPath(profileDirectory, path) || !loadJsonFile(path, &journal)
        || !validJournal(journal, importId))
        return domain::AppError(domain::ErrorCategory::Validation);
    if (!removeStageDirectory(profileDirectory, importId))
        return domain::AppError(domain::ErrorCategory::Storage);
    return QFile::remove(path) ? domain::AppError::none() : domain::AppError(domain::ErrorCategory::Storage);
}

domain::AppError LegacyDataImportService::journalImportId(
    const QString &profileDirectory, QString *importId, bool *exists)
{
    if (!importId || !exists) return domain::AppError(domain::ErrorCategory::Validation);
    const QString path = journalPath(profileDirectory);
    if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction())
        return domain::AppError(domain::ErrorCategory::Validation);
    if (!QFileInfo::exists(path)) {
        *exists = false;
        importId->clear();
        return domain::AppError::none();
    }
    QJsonObject journal;
    if (!managedPath(profileDirectory, path) || !loadJsonFile(path, &journal))
        return domain::AppError(domain::ErrorCategory::Validation);
    const QString value = journal.value(QStringLiteral("importId")).toString();
    if (!validJournal(journal, value))
        return domain::AppError(domain::ErrorCategory::Validation);
    *exists = true;
    *importId = value;
    return domain::AppError::none();
}

void LegacyDataImportService::cleanupUnjournaledStages(const QString &profileDirectory)
{
    if (QFileInfo::exists(journalPath(profileDirectory))) return;
    const QString base = QDir(profileDirectory).filePath(QStringLiteral(".legacy-import-staging"));
    const QFileInfo baseInfo(base);
    if (baseInfo.isSymLink() || !baseInfo.isDir()) return;
    static const QRegularExpression childPattern(QStringLiteral("^[a-f0-9]{32}$"));
    QDir directory(base);
    const QFileInfoList children = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo &child : children) {
        if (child.isSymLink() || !child.isDir() || !childPattern.match(child.fileName()).hasMatch()) continue;
        removeStageDirectory(profileDirectory, child.fileName());
    }
}

domain::AppError LegacyDataImportService::recover(const QString &profileDirectory, const QString &importId,
                                      bool databaseCommitExists)
{
    if (!validId(importId)) return domain::AppError(domain::ErrorCategory::Validation);
    return databaseCommitExists ? finish(profileDirectory, importId) : rollback(profileDirectory, importId);
}

domain::AppError LegacyDataImportService::saveReport(
    const QString &profileDirectory, const QString &importId, const QString &reportJson)
{
    if (!validId(importId) || reportJson.isEmpty()) return domain::AppError(domain::ErrorCategory::Validation);
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(reportJson.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !ensureManagedDirectory(profileDirectory, QStringLiteral("import-reports")))
        return domain::AppError(domain::ErrorCategory::Validation);
    const QString path = QDir(profileDirectory).filePath(QStringLiteral("import-reports/") + importId + QStringLiteral(".json"));
    return managedPath(profileDirectory, path) && writeJsonFile(path, document.object())
        ? domain::AppError::none() : domain::AppError(domain::ErrorCategory::Storage);
}

} // namespace something::services
