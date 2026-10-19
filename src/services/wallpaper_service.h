#pragma once

#include "../domain/app_error.h"
#include "wallpaper_asset_store.h"
#include "../storage/repository_types.h"
#include "../storage/storage_worker.h"

#include <QString>
#include <QVector>

namespace something::services {

class WallpaperService final {
public:
    static QString defaultSelection();
    static bool isBuiltInSelection(const QString &selection);
    static bool isCustomSelection(const QString &selection);
    static domain::AppError validateSelection(const QString &selection,
                                              const QVector<storage::Wallpaper> &wallpapers,
                                              const QString &profileDirectory);
    static storage::StorageCommand loadCatalog();
    static storage::StorageCommand persistSelection(QString selection);
    static storage::StorageCommand commitImport(QString profileDirectory,
                                                StagedWallpaper wallpaper);
    static storage::StorageCommand deleteWallpaper(QString profileDirectory,
                                                    storage::Wallpaper wallpaper);
};

} // namespace something::services
