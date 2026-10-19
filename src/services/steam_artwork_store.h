#pragma once

#include "../application/cancellation_token.h"
#include "../domain/app_error.h"
#include "../storage/repository_types.h"

#include <QByteArray>
#include <QString>

namespace something::services {

struct StagedSteamArtwork final {
    storage::AssetMetadata asset;
    QString stagingFilename;
    bool stagedNewFile{false};
};

class SteamArtworkStore final {
public:
    static constexpr qint64 maximumArtworkBytes = 2LL * 1024LL * 1024LL;

    static domain::AppError stageResponse(const QString &profileDirectory, quint32 appId,
                                          const QByteArray &body,
                                          const application::CancellationToken &cancellation,
                                          StagedSteamArtwork *result);
    static bool install(const QString &profileDirectory, const StagedSteamArtwork &artwork,
                        bool *installedNewFile = nullptr);
    static void discard(const QString &profileDirectory, const StagedSteamArtwork &artwork);
    static QString imageSource(const QString &profileDirectory, const QString &filename);
    static bool stageDeletion(const QString &profileDirectory, const QString &filename,
                              QString *stagedPath, QString *originalPath = nullptr);
    static void restoreDeletion(const QString &originalPath, const QString &stagedPath);
    static void finishDeletion(const QString &stagedPath);
    static void removeAbandonedStagingFiles(const QString &profileDirectory);
    static void removeInstalledIfNew(const QString &profileDirectory,
                                     const StagedSteamArtwork &artwork,
                                     bool installedNewFile);
};

} // namespace something::services
