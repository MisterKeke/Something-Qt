#pragma once

#include "../application/cancellation_token.h"
#include "../domain/app_error.h"
#include "../storage/repository_types.h"

#include <QString>
#include <QStringList>

namespace something::services {

struct StagedWallpaper final {
    QString id;
    QString displayName;
    QString stagingFilename;
    storage::AssetMetadata asset;
};

class WallpaperAssetStore final {
public:
    static constexpr qint64 maximumWallpaperBytes = 20LL * 1024LL * 1024LL;

    static domain::AppError stageImport(const QString &sourcePath,
                                        const QString &profileDirectory,
                                        const application::CancellationToken &cancellation,
                                        StagedWallpaper *result);
    static bool installStaged(const QString &profileDirectory,
                              const StagedWallpaper &wallpaper);
    static void discardStaged(const QString &profileDirectory,
                              const StagedWallpaper &wallpaper);
    static bool stageDeletion(const QString &profileDirectory,
                              const storage::Wallpaper &wallpaper,
                              QString *stagedPath);
    static void restoreDeletion(const QString &originalPath, const QString &stagedPath);
    static void finishDeletion(const QString &stagedPath);
    static void removeAbandonedStagingFiles(const QString &profileDirectory);
    static void recoverStagedDeletions(const QString &profileDirectory,
                                       const QStringList &persistedFilenames);
    static void removeUnreferencedWallpaperFiles(const QString &profileDirectory,
                                                 const QStringList &persistedFilenames);
};

} // namespace something::services
