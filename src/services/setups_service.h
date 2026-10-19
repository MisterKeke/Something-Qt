#pragma once

#include "../application/cancellation_token.h"
#include "../application/operation_coordinator.h"
#include "../domain/app_error.h"
#include "../platform/desktop_applications_platform.h"
#include "../services/setup_icon_store.h"
#include "../storage/storage_worker.h"

#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <optional>

namespace something::services {

struct SetupLaunchFailure final {
    qint64 applicationId{0};
    QString applicationName;
    QString message;
};

struct SetupLaunchSummary final {
    int attempted{0};
    int launched{0};
    bool cancelled{false};
    QVector<SetupLaunchFailure> failures;
    QVariantMap toVariantMap() const;
};

class SetupsService final {
public:
    using LaunchFunction = std::function<domain::AppError(
        const QVariantMap &, const application::CancellationToken &,
        const application::CancellationToken &)>;

    static storage::StorageCommand list();
    static storage::StorageCommand launchData(qint64 setupId);
    static storage::StorageCommand save(storage::Setup setup, bool removeIcon,
        QString profileDirectory, std::optional<StagedSetupIcon> stagedIcon = std::nullopt);
    static storage::StorageCommand remove(qint64 setupId, QString profileDirectory);
    static QVariantMap toVariantMap(const storage::Setup &setup);
    static QVariantList toVariantList(const QVector<storage::Setup> &setups);
    static SetupLaunchSummary launchMembers(const QVariantList &orderedApplications,
        const application::CancellationToken &applicationCancellation,
        const application::CancellationSource &userCancellation,
        const LaunchFunction &launch);
    static QString safeLaunchFailure(domain::AppError error);
};

} // namespace something::services
