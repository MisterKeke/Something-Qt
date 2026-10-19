#pragma once

#include "../application/cancellation_token.h"
#include "../domain/app_error.h"

#include <QString>
#include <QVariantMap>

namespace something::services {

class LegacyDataImportService final {
public:
    // Source access is restricted to an explicitly selected database.db and
    // requires the caller to confirm that the source application is closed.
    static domain::AppError prepare(const QString &sourceDatabasePath,
                                    const QString &profileDirectory,
                                    bool sourceClosedConfirmed,
                                    const application::CancellationToken &cancellation,
                                    QVariantMap *result);
    static domain::AppError installAssets(const QString &profileDirectory,
                                          const QString &importId,
                                          const QString &planPath,
                                          const application::CancellationToken &cancellation);
    static domain::AppError rollback(const QString &profileDirectory, const QString &importId);
    static domain::AppError finish(const QString &profileDirectory, const QString &importId);
    static domain::AppError saveReport(const QString &profileDirectory, const QString &importId,
                                       const QString &reportJson);
    static domain::AppError journalImportId(const QString &profileDirectory,
                                            QString *importId, bool *exists);
    static void cleanupUnjournaledStages(const QString &profileDirectory);
    static domain::AppError recover(const QString &profileDirectory, const QString &importId,
                                    bool databaseCommitExists);
};

} // namespace something::services
