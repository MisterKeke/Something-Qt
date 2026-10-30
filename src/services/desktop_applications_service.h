#pragma once

#include "../platform/desktop_applications_platform.h"
#include "../application/cancellation_token.h"
#include "../storage/storage_worker.h"

#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <optional>

namespace something::services {

class DesktopApplicationsService final {
public:
    static storage::StorageCommand list();
    static storage::StorageCommand listByCategory(qint64 categoryId);
    static storage::StorageCommand listUncategorized();
    static storage::StorageCommand get(qint64 id);
    static storage::StorageCommand listCategories();
    static storage::StorageCommand createCategory(QString name, QString description,
                                                   QString color, QString iconIdentifier);
    static storage::StorageCommand updateCategory(qint64 id, QString name, QString description,
                                                   QString color, QString iconIdentifier);
    static storage::StorageCommand reorderCategories(QVector<qint64> orderedIds);
    static storage::StorageCommand assignCategory(qint64 applicationId, qint64 categoryId);
    static storage::StorageCommand clearCategory(qint64 applicationId);
    static storage::StorageCommand deleteCategory(qint64 categoryId);
    static storage::StorageCommand add(QString displayName, QString executablePath,
                                       std::optional<storage::AssetMetadata> icon = std::nullopt);
    static storage::StorageCommand rename(qint64 id, QString displayName);
    static storage::StorageCommand relocate(qint64 id, QString executablePath);
    static storage::StorageCommand setIcon(qint64 id, storage::AssetMetadata icon);
    static storage::StorageCommand removeIcon(qint64 id);
    static storage::StorageCommand remove(qint64 id);

    static QVariantMap toVariantMap(const storage::DesktopApplication &application);
    static QVariantList toVariantList(const QVector<storage::DesktopApplication> &applications);
    static domain::AppError resolveLaunchPath(const QVariantMap &record,
                                              platform::DesktopApplicationsPlatform *platform,
                                              QString *resolvedPath);
    static domain::AppError launchSavedRecord(
        const QVariantMap &record, platform::DesktopApplicationsPlatform *platform,
        const application::CancellationToken &cancellation,
        const application::CancellationToken *additionalCancellation = nullptr);
};

} // namespace something::services
