#pragma once

#include "../application/cancellation_token.h"
#include "../domain/app_error.h"

#include <QVector>
#include <QString>

namespace something::platform {

struct FileExplorerRootCandidate final {
    QString label;
    QString kind;
    QString path;
};

struct FileExplorerEntry final {
    QString name;
    QString relativePath;
    QString type;
    QString modified;
    quint64 size{0};
    bool directory{false};
    bool canNavigate{false};
    bool canOpen{false};
    bool canRecycle{false};
};

struct FileExplorerBreadcrumb final {
    QString label;
    QString relativePath;
};

struct FileExplorerListing final {
    QString errorCode;
    QString relativePath;
    QString parentRelativePath;
    QVector<FileExplorerBreadcrumb> breadcrumbs;
    QVector<FileExplorerEntry> entries;
    bool hasMore{false};
    bool truncated{false};
};

class FileExplorerPlatform {
public:
    virtual ~FileExplorerPlatform() = default;

    virtual QVector<FileExplorerRootCandidate> standardRoots() const = 0;
    virtual QString canonicalDirectory(const QString &path, QString *errorCode) const = 0;
    virtual FileExplorerListing listDirectory(
        const QString &canonicalRoot, const QString &rootLabel,
        const QString &relativePath, const QString &query, int offset, int limit,
        const application::CancellationToken &cancellation) const = 0;
    virtual QString resolveRegularFile(const QString &canonicalRoot,
                                       const QString &relativePath,
                                       QString *errorCode) const = 0;
};

class FileExplorerShell {
public:
    virtual ~FileExplorerShell() = default;
    virtual bool supportsNativeActions() const noexcept = 0;
    virtual domain::AppError openFile(const QString &canonicalPath) = 0;
    virtual domain::AppError recycleFile(const QString &canonicalPath) = 0;
};

class NativeFileExplorerPlatform final : public FileExplorerPlatform {
public:
    QVector<FileExplorerRootCandidate> standardRoots() const override;
    QString canonicalDirectory(const QString &path, QString *errorCode) const override;
    FileExplorerListing listDirectory(
        const QString &canonicalRoot, const QString &rootLabel,
        const QString &relativePath, const QString &query, int offset, int limit,
        const application::CancellationToken &cancellation) const override;
    QString resolveRegularFile(const QString &canonicalRoot,
                               const QString &relativePath,
                               QString *errorCode) const override;

private:
    QString resolveContainedPath(const QString &canonicalRoot, const QString &relativePath,
                                 bool requireRegularFile, bool requireDirectory,
                                 QString *errorCode) const;
};

class NativeFileExplorerShell final : public FileExplorerShell {
public:
    bool supportsNativeActions() const noexcept override;
    domain::AppError openFile(const QString &canonicalPath) override;
    domain::AppError recycleFile(const QString &canonicalPath) override;
};

} // namespace something::platform
