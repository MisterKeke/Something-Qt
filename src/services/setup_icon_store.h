#pragma once

#include "../application/cancellation_token.h"
#include "../domain/app_error.h"
#include "../storage/repository_types.h"

namespace something::services {

struct StagedSetupIcon final {
    storage::AssetMetadata asset;
    QString stagingFilename;
};

class SetupIconStore final {
public:
    static constexpr qint64 maximumIconBytes = 5LL * 1024LL * 1024LL;
    static domain::AppError stageImport(const QString &sourcePath, const QString &profileDirectory,
                                        const application::CancellationToken &cancellation,
                                        StagedSetupIcon *result);
    static bool install(const QString &profileDirectory, const StagedSetupIcon &icon);
    static void discard(const QString &profileDirectory, const StagedSetupIcon &icon);
    static bool remove(const QString &profileDirectory, const QString &filename);
    static void removeAbandonedStagingFiles(const QString &profileDirectory);
};

} // namespace something::services
