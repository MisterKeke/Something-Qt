#include "wallpaper_service.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <utility>

namespace something::services {
namespace {

const QString defaultWallpaper = QStringLiteral("builtin:hu-tao");
const QStringList builtIns{
    QStringLiteral("builtin:original"),
    QStringLiteral("builtin:sandrone"),
    QStringLiteral("builtin:hu-tao"),
    QStringLiteral("builtin:skirk"),
};

} // namespace

QString WallpaperService::defaultSelection()
{
    return defaultWallpaper;
}

bool WallpaperService::isBuiltInSelection(const QString &selection)
{
    return builtIns.contains(selection);
}

bool WallpaperService::isCustomSelection(const QString &selection)
{
    return selection.startsWith(QStringLiteral("custom:"));
}

domain::AppError WallpaperService::validateSelection(
    const QString &selection,
    const QVector<storage::Wallpaper> &wallpapers,
    const QString &profileDirectory)
{
    if (isBuiltInSelection(selection)) return domain::AppError::none();
    if (!isCustomSelection(selection)) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }

    const QString id = selection.mid(7);
    static const QRegularExpression idPattern(QStringLiteral("^[a-f0-9]{32}$"));
    static const QRegularExpression filenamePattern(
        QStringLiteral("^[a-f0-9]{32}\\.(jpg|png|webp)$"));
    if (!idPattern.match(id).hasMatch()) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    for (const storage::Wallpaper &wallpaper : wallpapers) {
        if (wallpaper.id != id) continue;
        if (!filenamePattern.match(wallpaper.asset.filename).hasMatch()
            || !wallpaper.asset.filename.startsWith(id + QLatin1Char('.'))) {
            return domain::AppError(domain::ErrorCategory::Validation);
        }
        const QString path = QDir(profileDirectory).filePath(
            QStringLiteral("assets/wallpapers/") + wallpaper.asset.filename);
        const QFileInfo asset(path);
        return asset.isFile() && !asset.isSymLink()
            ? domain::AppError::none()
            : domain::AppError(domain::ErrorCategory::NotFound);
    }
    return domain::AppError(domain::ErrorCategory::NotFound);
}

storage::StorageCommand WallpaperService::loadCatalog()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        storage::StorageReply reply;
        if (cancelled()) {
            reply.error = domain::AppError::cancelled();
            return reply;
        }
        const auto records = repositories.wallpapers.list();
        if (!records.succeeded()) {
            reply.error = records.error;
            return reply;
        }
        QVariantList rows;
        for (const storage::Wallpaper &wallpaper : records.value) {
            rows.push_back(QVariantMap{
                {QStringLiteral("id"), wallpaper.id},
                {QStringLiteral("label"), wallpaper.displayName},
                {QStringLiteral("filename"), wallpaper.asset.filename},
                {QStringLiteral("mimeType"), wallpaper.asset.mimeType},
                {QStringLiteral("byteSize"), wallpaper.asset.byteSize},
                {QStringLiteral("createdAt"), wallpaper.createdAt},
            });
        }
        auto selected = repositories.preferences.selectedWallpaper();
        if (selected.error.category() == domain::ErrorCategory::NotFound) {
            selected.value = defaultSelection();
            selected.error = domain::AppError::none();
        }
        if (!selected.succeeded()) {
            reply.error = selected.error;
            return reply;
        }
        reply.values.insert(QStringLiteral("wallpapers"), rows);
        reply.values.insert(QStringLiteral("selected"), selected.value);
        return reply;
    };
}

storage::StorageCommand WallpaperService::persistSelection(QString selection)
{
    selection = selection.trimmed();
    return [selection = std::move(selection)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        storage::StorageReply reply;
        if (cancelled()) {
            reply.error = domain::AppError::cancelled();
            return reply;
        }
        const auto result = repositories.preferences.setSelectedWallpaper(selection);
        reply.error = result.error;
        if (result.succeeded()) reply.values.insert(QStringLiteral("selected"), selection);
        return reply;
    };
}

storage::StorageCommand WallpaperService::commitImport(QString profileDirectory,
                                                        StagedWallpaper wallpaper)
{
    return [profileDirectory = std::move(profileDirectory), wallpaper = std::move(wallpaper)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        storage::StorageReply reply;
        if (cancelled()) {
            reply.error = domain::AppError::cancelled();
            return reply;
        }
        if (!WallpaperAssetStore::installStaged(profileDirectory, wallpaper)) {
            reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        }
        const QString installedPath = QDir(profileDirectory).filePath(
            QStringLiteral("assets/wallpapers/") + wallpaper.asset.filename);
        if (cancelled()) {
            QFile::remove(installedPath);
            reply.error = domain::AppError::cancelled();
            return reply;
        }
        const storage::Wallpaper record{wallpaper.id, wallpaper.displayName,
                                        wallpaper.asset, {}};
        const QString selection = QStringLiteral("custom:") + wallpaper.id;
        const auto inserted = repositories.wallpapers.addAndSelect(record, selection, cancelled);
        if (!inserted.succeeded()) {
            QFile::remove(installedPath);
            reply.error = inserted.error;
            return reply;
        }
        reply.values.insert(QStringLiteral("selected"), selection);
        return reply;
    };
}

storage::StorageCommand WallpaperService::deleteWallpaper(QString profileDirectory,
                                                           storage::Wallpaper wallpaper)
{
    return [profileDirectory = std::move(profileDirectory), wallpaper = std::move(wallpaper)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        storage::StorageReply reply;
        const QString originalPath = QDir(profileDirectory).filePath(
            QStringLiteral("assets/wallpapers/") + wallpaper.asset.filename);
        QString stagedPath;
        if (cancelled()) {
            reply.error = domain::AppError::cancelled();
            return reply;
        }
        if (!WallpaperAssetStore::stageDeletion(profileDirectory, wallpaper, &stagedPath)) {
            reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        }
        if (cancelled()) {
            WallpaperAssetStore::restoreDeletion(originalPath, stagedPath);
            reply.error = domain::AppError::cancelled();
            return reply;
        }
        const auto removed = repositories.wallpapers.remove(wallpaper.id, cancelled);
        if (!removed.succeeded()) {
            WallpaperAssetStore::restoreDeletion(originalPath, stagedPath);
            reply.error = removed.error;
            return reply;
        }
        WallpaperAssetStore::finishDeletion(stagedPath);
        const auto selected = repositories.preferences.selectedWallpaper();
        reply.values.insert(QStringLiteral("selected"), selected.succeeded()
            ? selected.value : defaultSelection());
        reply.values.insert(QStringLiteral("deletedId"), wallpaper.id);
        return reply;
    };
}

} // namespace something::services
