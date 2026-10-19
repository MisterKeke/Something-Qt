#pragma once

#include "../application/cancellation_token.h"
#include "../domain/app_error.h"
#include "../storage/repository_types.h"

#include <QImage>
#include <QString>

namespace something::services {

struct StagedDesktopAppIcon final {
    storage::AssetMetadata asset;
    QString stagingFilename;
};

struct StagedDesktopAppIconDeletion final {
    QString filename;
    QString stagedFilename;
    bool moved{false};
};

class DesktopAppIconStore final {
public:
    static constexpr qint64 maximumIconBytes = 5LL * 1024LL * 1024LL;

    static domain::AppError stageImport(const QString &sourcePath, const QString &profileDirectory,
                                        const application::CancellationToken &cancellation,
                                        StagedDesktopAppIcon *result);
    static domain::AppError stageImage(const QImage &image, const QString &profileDirectory,
                                       const application::CancellationToken &cancellation,
                                       StagedDesktopAppIcon *result);
    static bool install(const QString &profileDirectory, const StagedDesktopAppIcon &icon);
    static void discard(const QString &profileDirectory, const StagedDesktopAppIcon &icon);
    static bool stageDeletion(const QString &profileDirectory, const QString &filename,
                              StagedDesktopAppIconDeletion *result);
    static void restoreDeletion(const QString &profileDirectory,
                                const StagedDesktopAppIconDeletion &deletion);
    static void finishDeletion(const QString &profileDirectory,
                               const StagedDesktopAppIconDeletion &deletion);
    static bool remove(const QString &profileDirectory, const QString &filename);
    static void removeAbandonedStagingFiles(const QString &profileDirectory);
};

} // namespace something::services
