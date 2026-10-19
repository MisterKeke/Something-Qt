#include "desktop_applications_service.h"

#include "../domain/desktop_application_rules.h"

#include <QFileInfo>

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

storage::StorageReply withApplication(const storage::RepositoryResult<storage::DesktopApplication> &result)
{
    if (!result.succeeded()) return failed(result.error);
    storage::StorageReply reply;
    reply.values.insert(QStringLiteral("application"), DesktopApplicationsService::toVariantMap(result.value));
    return reply;
}

} // namespace

storage::StorageCommand DesktopApplicationsService::list()
{
    return [](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto result = repositories.applications.list();
        if (!result.succeeded()) return failed(result.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("applications"), toVariantList(result.value));
        return reply;
    };
}

storage::StorageCommand DesktopApplicationsService::get(qint64 id)
{
    return [id](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (id <= 0) return failed(domain::AppError(domain::ErrorCategory::Validation));
        return withApplication(repositories.applications.get(id));
    };
}

storage::StorageCommand DesktopApplicationsService::add(
    QString displayName, QString executablePath, std::optional<storage::AssetMetadata> icon)
{
    return [displayName = std::move(displayName), executablePath = std::move(executablePath),
            icon = std::move(icon)](storage::StorageRepositories &repositories,
                                    const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto name = domain::normalizeDesktopApplicationName(displayName);
        if (!name || executablePath.trimmed().isEmpty())
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto added = repositories.applications.add(*name, executablePath, icon);
        if (!added.succeeded()) return failed(added.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("application"), toVariantMap(added.value));
        return reply;
    };
}

storage::StorageCommand DesktopApplicationsService::rename(qint64 id, QString displayName)
{
    return [id, displayName = std::move(displayName)](storage::StorageRepositories &repositories,
                                                       const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto name = domain::normalizeDesktopApplicationName(displayName);
        if (id <= 0 || !name) return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto existing = repositories.applications.get(id);
        if (!existing.succeeded()) return failed(existing.error);
        return withApplication(repositories.applications.update(id, *name,
            existing.value.executablePath));
    };
}

storage::StorageCommand DesktopApplicationsService::relocate(qint64 id, QString executablePath)
{
    return [id, executablePath = std::move(executablePath)](
               storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (id <= 0 || executablePath.trimmed().isEmpty())
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto existing = repositories.applications.get(id);
        if (!existing.succeeded()) return failed(existing.error);
        return withApplication(repositories.applications.update(
            id, existing.value.displayName, executablePath));
    };
}

storage::StorageCommand DesktopApplicationsService::setIcon(qint64 id, storage::AssetMetadata icon)
{
    return [id, icon = std::move(icon)](storage::StorageRepositories &repositories,
                                        const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (id <= 0) return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto result = repositories.applications.setIcon(id, icon);
        if (!result.succeeded()) return failed(result.error);
        return withApplication(repositories.applications.get(id));
    };
}

storage::StorageCommand DesktopApplicationsService::removeIcon(qint64 id)
{
    return [id](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (id <= 0) return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto result = repositories.applications.removeIcon(id);
        if (!result.succeeded()) return failed(result.error);
        return withApplication(repositories.applications.get(id));
    };
}

storage::StorageCommand DesktopApplicationsService::remove(qint64 id)
{
    return [id](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (id <= 0) return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto result = repositories.applications.remove(id);
        return result.succeeded() ? storage::StorageReply{} : failed(result.error);
    };
}

QVariantMap DesktopApplicationsService::toVariantMap(const storage::DesktopApplication &application)
{
    QVariantMap row{{QStringLiteral("id"), application.id},
                    {QStringLiteral("displayName"), application.displayName},
                    {QStringLiteral("executablePath"), application.executablePath},
                    {QStringLiteral("createdAt"), application.createdAt},
                    {QStringLiteral("updatedAt"), application.updatedAt},
                    {QStringLiteral("available"), false},
                    {QStringLiteral("nativeIconDataUrl"), QString()},
                    {QStringLiteral("iconFilename"), application.icon
                        ? application.icon->filename : QString()}};
    return row;
}

QVariantList DesktopApplicationsService::toVariantList(
    const QVector<storage::DesktopApplication> &applications)
{
    QVariantList rows;
    rows.reserve(applications.size());
    for (const auto &application : applications) rows.push_back(toVariantMap(application));
    return rows;
}

domain::AppError DesktopApplicationsService::resolveLaunchPath(
    const QVariantMap &record, platform::DesktopApplicationsPlatform *platform,
    QString *resolvedPath)
{
    if (!platform || !resolvedPath || record.value(QStringLiteral("id")).toLongLong() <= 0)
        return domain::AppError(domain::ErrorCategory::Validation);
    if (!platform->supported()) return domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
    const QString storedPath = record.value(QStringLiteral("executablePath")).toString();
    if (storedPath.isEmpty()) return domain::AppError(domain::ErrorCategory::Validation);
    return platform->resolveExecutable(storedPath, resolvedPath);
}

domain::AppError DesktopApplicationsService::launchSavedRecord(
    const QVariantMap &record, platform::DesktopApplicationsPlatform *platform,
    const application::CancellationToken &cancellation,
    const application::CancellationToken *additionalCancellation)
{
    const auto cancelled = [&] {
        return cancellation.isCancellationRequested()
            || (additionalCancellation && additionalCancellation->isCancellationRequested());
    };
    if (cancelled()) return domain::AppError::cancelled();
    if (record.value(QStringLiteral("missing")).toBool())
        return domain::AppError(domain::ErrorCategory::NotFound);
    QString path;
    domain::AppError error = resolveLaunchPath(record, platform, &path);
    if (error.isError()) return error;
    if (cancelled()) return domain::AppError::cancelled();
    return platform->launchExecutable(path, cancellation);
}

} // namespace something::services
