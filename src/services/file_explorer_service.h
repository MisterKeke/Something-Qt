#pragma once

#include "../platform/file_explorer_platform.h"

#include <QReadWriteLock>
#include <QVector>
#include <QVariantMap>

namespace something::services {

class FileExplorerService final {
public:
    FileExplorerService(platform::FileExplorerPlatform *platform,
                        platform::FileExplorerShell *shell);

    QVariantMap loadStandardPlaces(const application::CancellationToken &cancellation);
    QVariantMap addSessionRoot(const QString &path,
                               const application::CancellationToken &cancellation);
    QVariantMap listDirectory(const QString &rootId, const QString &relativePath,
                              const QString &query, int offset, int limit,
                              const application::CancellationToken &cancellation) const;
    QVariantMap openFile(const QString &rootId, const QString &relativePath,
                         const application::CancellationToken &cancellation) const;
    QVariantMap recycleFile(const QString &rootId, const QString &relativePath,
                            const application::CancellationToken &cancellation) const;

private:
    struct RootGrant final {
        QString id;
        QString label;
        QString kind;
        QString canonicalPath;
    };

    QVariantMap registerRoot(const QString &label, const QString &kind, const QString &path,
                             QString *errorCode);
    RootGrant rootForId(const QString &rootId) const;
    QVariantMap placesValue() const;
    static QVariantMap errorValue(const QString &errorCode);

    platform::FileExplorerPlatform *m_platform{nullptr};
    platform::FileExplorerShell *m_shell{nullptr};
    mutable QReadWriteLock m_rootsLock;
    QVector<RootGrant> m_roots;
};

} // namespace something::services
