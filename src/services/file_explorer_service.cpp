#include "file_explorer_service.h"

#include <QUuid>
#include <QReadLocker>
#include <QWriteLocker>
#include <QFileInfo>
#include <utility>

namespace something::services {
namespace {

QVariantMap entryValue(const platform::FileExplorerEntry &entry)
{
    return {{QStringLiteral("name"), entry.name},
            {QStringLiteral("relativePath"), entry.relativePath},
            {QStringLiteral("type"), entry.type},
            {QStringLiteral("modified"), entry.modified},
            {QStringLiteral("size"), QVariant::fromValue<qulonglong>(entry.size)},
            {QStringLiteral("directory"), entry.directory},
            {QStringLiteral("canNavigate"), entry.canNavigate},
            {QStringLiteral("canOpen"), entry.canOpen},
            {QStringLiteral("canRecycle"), entry.canRecycle}};
}

} // namespace

FileExplorerService::FileExplorerService(platform::FileExplorerPlatform *platform,
                                         platform::FileExplorerShell *shell)
    : m_platform(platform), m_shell(shell)
{
    Q_ASSERT(m_platform && m_shell);
}

QVariantMap FileExplorerService::errorValue(const QString &errorCode)
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("errorCode"), errorCode}};
}

QVariantMap FileExplorerService::registerRoot(const QString &label, const QString &kind,
                                              const QString &path, QString *errorCode)
{
    QString canonicalError;
    const QString canonical = m_platform->canonicalDirectory(path, &canonicalError);
    if (canonical.isEmpty()) {
        if (errorCode) *errorCode = canonicalError.isEmpty()
            ? QStringLiteral("unavailable-root") : canonicalError;
        return {};
    }

    QWriteLocker locker(&m_rootsLock);
    for (const RootGrant &root : std::as_const(m_roots)) {
#if defined(Q_OS_WIN)
        const bool same = root.canonicalPath.compare(canonical, Qt::CaseInsensitive) == 0;
#else
        const bool same = root.canonicalPath == canonical;
#endif
        if (same) {
            if (errorCode) errorCode->clear();
            return {{QStringLiteral("rootId"), root.id},
                    {QStringLiteral("label"), root.label},
                    {QStringLiteral("kind"), root.kind}};
        }
    }
    RootGrant root;
    root.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString folderName = QFileInfo(canonical).fileName();
    root.label = label.trimmed().isEmpty()
        ? (folderName.isEmpty() ? QStringLiteral("Folder root") : folderName) : label.trimmed();
    root.kind = kind;
    root.canonicalPath = canonical;
    m_roots.push_back(root);
    if (errorCode) errorCode->clear();
    return {{QStringLiteral("rootId"), root.id},
            {QStringLiteral("label"), root.label},
            {QStringLiteral("kind"), root.kind}};
}

FileExplorerService::RootGrant FileExplorerService::rootForId(const QString &rootId) const
{
    QReadLocker locker(&m_rootsLock);
    for (const RootGrant &root : m_roots)
        if (root.id == rootId) return root;
    return {};
}

QVariantMap FileExplorerService::placesValue() const
{
    QVariantList places;
    QReadLocker locker(&m_rootsLock);
    places.reserve(m_roots.size());
    for (const RootGrant &root : m_roots) {
        places.push_back(QVariantMap{{QStringLiteral("rootId"), root.id},
                                     {QStringLiteral("label"), root.label},
                                     {QStringLiteral("kind"), root.kind}});
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("places"), places}};
}

QVariantMap FileExplorerService::loadStandardPlaces(
    const application::CancellationToken &cancellation)
{
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    for (const auto &candidate : m_platform->standardRoots()) {
        if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
        QString errorCode;
        registerRoot(candidate.label, candidate.kind, candidate.path, &errorCode);
    }
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    return placesValue();
}

QVariantMap FileExplorerService::addSessionRoot(
    const QString &path, const application::CancellationToken &cancellation)
{
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    QString errorCode;
    const QVariantMap root = registerRoot(QString(), QStringLiteral("session"), path, &errorCode);
    if (root.isEmpty()) return errorValue(errorCode);
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("root"), root}};
}

QVariantMap FileExplorerService::listDirectory(
    const QString &rootId, const QString &relativePath, const QString &query,
    int offset, int limit, const application::CancellationToken &cancellation) const
{
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    const RootGrant root = rootForId(rootId);
    if (root.id.isEmpty()) return errorValue(QStringLiteral("invalid-root"));
    const platform::FileExplorerListing listing = m_platform->listDirectory(
        root.canonicalPath, root.label, relativePath, query, offset, limit, cancellation);
    if (!listing.errorCode.isEmpty()) return errorValue(listing.errorCode);

    QVariantList breadcrumbs;
    QVariantList entries;
    breadcrumbs.reserve(listing.breadcrumbs.size());
    entries.reserve(listing.entries.size());
    for (const auto &breadcrumb : listing.breadcrumbs) {
        breadcrumbs.push_back(QVariantMap{{QStringLiteral("label"), breadcrumb.label},
                                          {QStringLiteral("relativePath"), breadcrumb.relativePath}});
    }
    for (const auto &entry : listing.entries) entries.push_back(entryValue(entry));
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("rootId"), root.id},
            {QStringLiteral("relativePath"), listing.relativePath},
            {QStringLiteral("parentRelativePath"), listing.parentRelativePath},
            {QStringLiteral("breadcrumbs"), breadcrumbs},
            {QStringLiteral("entries"), entries},
            {QStringLiteral("hasMore"), listing.hasMore},
            {QStringLiteral("truncated"), listing.truncated}};
}

QVariantMap FileExplorerService::openFile(
    const QString &rootId, const QString &relativePath,
    const application::CancellationToken &cancellation) const
{
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    const RootGrant root = rootForId(rootId);
    if (root.id.isEmpty()) return errorValue(QStringLiteral("invalid-root"));
    QString errorCode;
    const QString canonical = m_platform->resolveRegularFile(root.canonicalPath, relativePath,
                                                              &errorCode);
    if (canonical.isEmpty()) return errorValue(errorCode);
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    const domain::AppError error = m_shell->supportsNativeActions()
        ? m_shell->openFile(canonical)
        : domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
    if (error.isError()) return errorValue(error.category() == domain::ErrorCategory::CapabilityUnavailable
                                           ? QStringLiteral("unsupported")
                                           : QStringLiteral("open-failed"));
    return {{QStringLiteral("ok"), true}};
}

QVariantMap FileExplorerService::recycleFile(
    const QString &rootId, const QString &relativePath,
    const application::CancellationToken &cancellation) const
{
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    const RootGrant root = rootForId(rootId);
    if (root.id.isEmpty()) return errorValue(QStringLiteral("invalid-root"));
    QString errorCode;
    const QString canonical = m_platform->resolveRegularFile(root.canonicalPath, relativePath,
                                                              &errorCode);
    if (canonical.isEmpty()) return errorValue(errorCode);
    if (cancellation.isCancellationRequested()) return errorValue(QStringLiteral("cancelled"));
    const domain::AppError error = m_shell->supportsNativeActions()
        ? m_shell->recycleFile(canonical)
        : domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
    if (error.isError()) return errorValue(error.category() == domain::ErrorCategory::CapabilityUnavailable
                                           ? QStringLiteral("unsupported")
                                           : QStringLiteral("recycle-failed"));
    return {{QStringLiteral("ok"), true}};
}

} // namespace something::services
