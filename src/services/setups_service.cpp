#include "setups_service.h"

#include "desktop_applications_service.h"

#include <QFileInfo>

#include <utility>

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

domain::AppError repositoryError(const domain::AppError &error)
{
    return error;
}

} // namespace

QVariantMap SetupLaunchSummary::toVariantMap() const
{
    QVariantList rows;
    rows.reserve(failures.size());
    for (const auto &failure : failures) {
        rows.push_back(QVariantMap{{QStringLiteral("applicationId"), failure.applicationId},
            {QStringLiteral("applicationName"), failure.applicationName},
            {QStringLiteral("message"), failure.message}});
    }
    return {{QStringLiteral("attempted"), attempted}, {QStringLiteral("launched"), launched},
            {QStringLiteral("cancelled"), cancelled}, {QStringLiteral("failures"), rows}};
}

storage::StorageCommand SetupsService::list()
{
    return [](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto setups = repositories.setups.list();
        if (!setups.succeeded()) return failed(setups.error);
        const auto applications = repositories.applications.list();
        if (!applications.succeeded()) return failed(applications.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("setups"), toVariantList(setups.value));
        reply.values.insert(QStringLiteral("applications"),
                            DesktopApplicationsService::toVariantList(applications.value));
        return reply;
    };
}

storage::StorageCommand SetupsService::launchData(qint64 setupId)
{
    return [setupId](storage::StorageRepositories &repositories,
                     const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (setupId <= 0) return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto setup = repositories.setups.get(setupId);
        if (!setup.succeeded()) return failed(setup.error);
        if (setup.value.applicationIds.isEmpty())
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        QVariantList applications;
        applications.reserve(setup.value.applicationIds.size());
        for (qint64 appId : setup.value.applicationIds) {
            if (cancelled()) return failed(domain::AppError::cancelled());
            const auto app = repositories.applications.get(appId);
            if (app.succeeded()) {
                applications.push_back(DesktopApplicationsService::toVariantMap(app.value));
            } else if (app.error.category() == domain::ErrorCategory::NotFound) {
                applications.push_back(QVariantMap{{QStringLiteral("id"), appId},
                    {QStringLiteral("displayName"), QStringLiteral("Application")},
                    {QStringLiteral("missing"), true}});
            } else {
                return failed(app.error);
            }
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("setup"), toVariantMap(setup.value));
        reply.values.insert(QStringLiteral("applications"), applications);
        return reply;
    };
}

storage::StorageCommand SetupsService::save(storage::Setup setup, bool removeIcon,
    QString profileDirectory, std::optional<StagedSetupIcon> stagedIcon)
{
    return [setup = std::move(setup), removeIcon, profileDirectory = std::move(profileDirectory),
            stagedIcon = std::move(stagedIcon)](storage::StorageRepositories &repositories,
                                                  const std::function<bool()> &cancelled) mutable {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (setup.icon && !stagedIcon)
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        if (removeIcon && stagedIcon) {
            if (stagedIcon) SetupIconStore::discard(profileDirectory, *stagedIcon);
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }

        QString previousIcon;
        if (setup.id > 0) {
            const auto previous = repositories.setups.get(setup.id);
            if (!previous.succeeded()) return failed(previous.error);
            if (previous.value.icon) previousIcon = previous.value.icon->filename;
        }
        if (stagedIcon) {
            if (cancelled()) {
                SetupIconStore::discard(profileDirectory, *stagedIcon);
                return failed(domain::AppError::cancelled());
            }
            setup.icon = stagedIcon->asset;
            if (!SetupIconStore::install(profileDirectory, *stagedIcon)) {
                SetupIconStore::discard(profileDirectory, *stagedIcon);
                return failed(domain::AppError(domain::ErrorCategory::Storage));
            }
        }

        const auto saved = repositories.setups.save(setup, removeIcon);
        if (!saved.succeeded()) {
            if (stagedIcon) SetupIconStore::remove(profileDirectory, stagedIcon->asset.filename);
            return failed(repositoryError(saved.error));
        }
        const QString currentIcon = saved.value.icon ? saved.value.icon->filename : QString();
        if (!previousIcon.isEmpty() && previousIcon != currentIcon)
            SetupIconStore::remove(profileDirectory, previousIcon);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("setup"), toVariantMap(saved.value));
        return reply;
    };
}

storage::StorageCommand SetupsService::remove(qint64 setupId, QString profileDirectory)
{
    return [setupId, profileDirectory = std::move(profileDirectory)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (setupId <= 0) return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto existing = repositories.setups.get(setupId);
        if (!existing.succeeded()) return failed(existing.error);
        const auto removed = repositories.setups.remove(setupId);
        if (!removed.succeeded()) return failed(removed.error);
        if (existing.value.icon)
            SetupIconStore::remove(profileDirectory, existing.value.icon->filename);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("setupId"), setupId);
        return reply;
    };
}

QVariantMap SetupsService::toVariantMap(const storage::Setup &setup)
{
    QVariantList applicationIds;
    applicationIds.reserve(setup.applicationIds.size());
    for (qint64 id : setup.applicationIds) applicationIds.push_back(id);
    return {{QStringLiteral("id"), setup.id}, {QStringLiteral("name"), setup.name},
        {QStringLiteral("description"), setup.description},
        {QStringLiteral("createdAt"), setup.createdAt}, {QStringLiteral("updatedAt"), setup.updatedAt},
        {QStringLiteral("applicationIds"), applicationIds},
        {QStringLiteral("iconFilename"), setup.icon ? setup.icon->filename : QString()}};
}

QVariantList SetupsService::toVariantList(const QVector<storage::Setup> &setups)
{
    QVariantList rows;
    rows.reserve(setups.size());
    for (const auto &setup : setups) rows.push_back(toVariantMap(setup));
    return rows;
}

SetupLaunchSummary SetupsService::launchMembers(const QVariantList &orderedApplications,
    const application::CancellationToken &applicationCancellation,
    const application::CancellationSource &userCancellation, const LaunchFunction &launch)
{
    SetupLaunchSummary summary;
    const auto userToken = userCancellation.token();
    for (const QVariant &value : orderedApplications) {
        if (applicationCancellation.isCancellationRequested()
            || userToken.isCancellationRequested()) {
            summary.cancelled = true;
            break;
        }
        const QVariantMap application = value.toMap();
        const qint64 id = application.value(QStringLiteral("id")).toLongLong();
        const QString name = application.value(QStringLiteral("displayName")).toString();
        ++summary.attempted;
        const domain::AppError error = launch
            ? launch(application, applicationCancellation, userToken)
            : domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
        if (error.isCancelled()) {
            if (applicationCancellation.isCancellationRequested()
                || userToken.isCancellationRequested()) {
                summary.cancelled = true;
                break;
            }
        }
        if (error.isError() || error.isCancelled()) {
            summary.failures.push_back({id, name, safeLaunchFailure(error)});
        } else {
            ++summary.launched;
        }
    }
    if (applicationCancellation.isCancellationRequested()
        || userToken.isCancellationRequested()) summary.cancelled = true;
    return summary;
}

QString SetupsService::safeLaunchFailure(domain::AppError error)
{
    switch (error.category()) {
    case domain::ErrorCategory::Validation:
        return QStringLiteral("This application's saved executable is unavailable.");
    case domain::ErrorCategory::NotFound:
        return QStringLiteral("This application is no longer saved.");
    case domain::ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("Application launching is unavailable on this device.");
    case domain::ErrorCategory::Platform:
        return QStringLiteral("The operating system could not open this application.");
    default:
        return QStringLiteral("This application could not be prepared for launch.");
    }
}

} // namespace something::services
