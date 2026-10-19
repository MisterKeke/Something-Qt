#include "file_explorer_platform.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStandardPaths>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <utility>

namespace something::platform {
namespace {

constexpr int MaximumResults = 2000;
constexpr int MaximumPageSize = 500;

bool isSafeRelativeComponent(const QString &part)
{
    if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral(".."))
        return false;
#if defined(Q_OS_WIN)
    if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) return false;
    QString deviceName = part.section(QLatin1Char('.'), 0, 0).toUpper();
    while (deviceName.endsWith(QLatin1Char(' '))) deviceName.chop(1);
    if (deviceName == QStringLiteral("CON") || deviceName == QStringLiteral("PRN")
        || deviceName == QStringLiteral("AUX") || deviceName == QStringLiteral("NUL")
        || deviceName == QStringLiteral("CONIN$") || deviceName == QStringLiteral("CONOUT$")) return false;
    for (int number = 1; number <= 9; ++number) {
        if (deviceName == QStringLiteral("COM%1").arg(number)
            || deviceName == QStringLiteral("LPT%1").arg(number)) return false;
    }
#endif
    return true;
}

QString normalizedRelativePath(const QString &input, QString *errorCode)
{
    if (input.contains(QChar::Null) || input.contains(QLatin1Char(':'))) {
        if (errorCode) *errorCode = QStringLiteral("invalid-path");
        return {};
    }
    QString path = input;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path)) {
        if (errorCode) *errorCode = QStringLiteral("invalid-path");
        return {};
    }
    const QStringList parts = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &part : parts) {
        if (!part.isEmpty() && !isSafeRelativeComponent(part)) {
            if (errorCode) *errorCode = QStringLiteral("invalid-path");
            return {};
        }
    }
    path = QDir::cleanPath(path);
    if (path == QStringLiteral(".")) path.clear();
    if (errorCode) errorCode->clear();
    return path;
}

bool isReparsePoint(const QString &path)
{
#if defined(Q_OS_WIN)
    QString nativePath = QDir::toNativeSeparators(path);
    if (!nativePath.startsWith(QStringLiteral("\\\\?\\"))) {
        if (nativePath.startsWith(QStringLiteral("\\\\")))
            nativePath = QStringLiteral("\\\\?\\UNC\\") + nativePath.mid(2);
        else
            nativePath = QStringLiteral("\\\\?\\") + nativePath;
    }
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    // Fail closed when the path cannot be inspected, including on a long path
    // when the host OS has not enabled the manifest's long-path policy.
    return attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return QFileInfo(path).isSymLink();
#endif
}

bool isWithinRoot(const QString &root, const QString &candidate)
{
    const QString relative = QDir(root).relativeFilePath(candidate);
    if (relative == QStringLiteral(".")) return true;
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../"))) return false;
#if defined(Q_OS_WIN)
    return !QDir::isAbsolutePath(relative) && !relative.startsWith(QStringLiteral("..\\"));
#else
    return !QDir::isAbsolutePath(relative);
#endif
}

bool entryLess(const FileExplorerEntry &left, const FileExplorerEntry &right)
{
    if (left.directory != right.directory) return left.directory;
    const int insensitive = QString::compare(left.name, right.name, Qt::CaseInsensitive);
    if (insensitive != 0) return insensitive < 0;
    const int ordinal = QString::compare(left.name, right.name, Qt::CaseSensitive);
    if (ordinal != 0) return ordinal < 0;
    return left.relativePath < right.relativePath;
}

QString rootError(const QFileInfo &info)
{
    if (!info.exists()) return QStringLiteral("unavailable-root");
    if (!info.isDir()) return QStringLiteral("not-directory");
    if (!info.isReadable()) return QStringLiteral("permission-denied");
    return QStringLiteral("invalid-root");
}

#if defined(Q_OS_WIN)
class ComApartment final {
public:
    ComApartment()
        : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
    {
    }
    ~ComApartment() { if (SUCCEEDED(m_result)) CoUninitialize(); }
    bool ready() const noexcept { return SUCCEEDED(m_result); }

private:
    HRESULT m_result{E_FAIL};
};

template <typename T>
using ComPtr = std::unique_ptr<T, void (*)(T *)>;

template <typename T>
ComPtr<T> adoptCom(T *value)
{
    return ComPtr<T>(value, [](T *pointer) { if (pointer) pointer->Release(); });
}
#endif

} // namespace

QVector<FileExplorerRootCandidate> NativeFileExplorerPlatform::standardRoots() const
{
    QVector<FileExplorerRootCandidate> roots;
#if defined(Q_OS_WIN)
    const QString systemDrive = QStringLiteral("C:/");
    if (QFileInfo(systemDrive).isDir())
        roots.push_back({QStringLiteral("Home"), QStringLiteral("home"), systemDrive});
    const QString dataDrive = QStringLiteral("D:/");
    if (QFileInfo(dataDrive).isDir())
        roots.push_back({QStringLiteral("D:"), QStringLiteral("drive"), dataDrive});
#else
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (!home.isEmpty() && QFileInfo(home).isDir())
        roots.push_back({QStringLiteral("Home"), QStringLiteral("home"), home});
#endif
    const auto addStandard = [&roots](QStandardPaths::StandardLocation location,
                                      const QString &label, const QString &kind) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty() && QFileInfo(path).isDir()) roots.push_back({label, kind, path});
    };
    addStandard(QStandardPaths::DesktopLocation, QStringLiteral("Desktop"), QStringLiteral("desktop"));
    addStandard(QStandardPaths::DocumentsLocation, QStringLiteral("Documents"), QStringLiteral("documents"));
    addStandard(QStandardPaths::DownloadLocation, QStringLiteral("Downloads"), QStringLiteral("downloads"));
    return roots;
}

QString NativeFileExplorerPlatform::canonicalDirectory(const QString &path,
                                                        QString *errorCode) const
{
    if (path.isEmpty() || path.contains(QChar::Null)) {
        if (errorCode) *errorCode = QStringLiteral("invalid-root");
        return {};
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        if (errorCode) *errorCode = rootError(info);
        return {};
    }
    if (!info.isReadable()) {
        if (errorCode) *errorCode = QStringLiteral("permission-denied");
        return {};
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
        if (errorCode) *errorCode = QStringLiteral("unavailable-root");
        return {};
    }
    if (errorCode) errorCode->clear();
    return QDir::cleanPath(canonical);
}

QString NativeFileExplorerPlatform::resolveContainedPath(const QString &canonicalRoot,
                                                          const QString &relativePath,
                                                          bool requireRegularFile,
                                                          bool requireDirectory,
                                                          QString *errorCode) const
{
    QString relativeError;
    const QString relative = normalizedRelativePath(relativePath, &relativeError);
    if (!relativeError.isEmpty()) {
        if (errorCode) *errorCode = relativeError;
        return {};
    }
    const QFileInfo rootInfo(canonicalRoot);
    const QString root = QDir::cleanPath(rootInfo.canonicalFilePath());
    if (root.isEmpty() || !rootInfo.isDir()) {
        if (errorCode) *errorCode = QStringLiteral("unavailable-root");
        return {};
    }
    const QString registeredRoot = QDir::cleanPath(canonicalRoot);
#if defined(Q_OS_WIN)
    if (root.compare(registeredRoot, Qt::CaseInsensitive) != 0) {
#else
    if (root != registeredRoot) {
#endif
        if (errorCode) *errorCode = QStringLiteral("unavailable-root");
        return {};
    }
    QString candidate = relative.isEmpty() ? root : QDir(root).filePath(relative);
    candidate = QDir::cleanPath(candidate);
    const QFileInfo lexicalInfo(candidate);
    if (!lexicalInfo.exists()) {
        if (errorCode) *errorCode = QStringLiteral("not-found");
        return {};
    }

    // Reject every reparse point below an explicitly selected root. This also
    // excludes Windows junctions and mount points, even when their destination
    // currently resolves inside the root.
    if (!relative.isEmpty()) {
        QString current = root;
        const QStringList parts = relative.split(QLatin1Char('/'));
        for (const QString &part : parts) {
            current = QDir(current).filePath(part);
            if (QFileInfo(current).isSymLink() || isReparsePoint(current)) {
                if (errorCode) *errorCode = QStringLiteral("unsafe-link");
                return {};
            }
        }
    }

    const QString canonical = lexicalInfo.canonicalFilePath();
    if (canonical.isEmpty() || !isWithinRoot(root, canonical)) {
        if (errorCode) *errorCode = QStringLiteral("outside-root");
        return {};
    }
    const QFileInfo info(canonical);
    if (requireDirectory && !info.isDir()) {
        if (errorCode) *errorCode = QStringLiteral("not-directory");
        return {};
    }
    if (requireRegularFile) {
        if (info.isDir()) {
            if (errorCode) *errorCode = QStringLiteral("not-file");
            return {};
        }
        if (!info.isFile()) {
            if (errorCode) *errorCode = QStringLiteral("not-file");
            return {};
        }
        if (!info.isReadable()) {
            if (errorCode) *errorCode = QStringLiteral("permission-denied");
            return {};
        }
    }
    if (errorCode) errorCode->clear();
    return canonical;
}

FileExplorerListing NativeFileExplorerPlatform::listDirectory(
    const QString &canonicalRoot, const QString &rootLabel,
    const QString &relativePath, const QString &query, int offset, int limit,
    const application::CancellationToken &cancellation) const
{
    FileExplorerListing listing;
    if (offset < 0 || offset >= MaximumResults || limit <= 0 || limit > MaximumPageSize) {
        listing.errorCode = QStringLiteral("invalid-page");
        return listing;
    }
    if (query.toUcs4().size() > 255) {
        listing.errorCode = QStringLiteral("invalid-query");
        return listing;
    }
    if (cancellation.isCancellationRequested()) {
        listing.errorCode = QStringLiteral("cancelled");
        return listing;
    }
    QString errorCode;
    const QString directory = resolveContainedPath(canonicalRoot, relativePath, false, true,
                                                    &errorCode);
    if (directory.isEmpty()) {
        listing.errorCode = errorCode;
        return listing;
    }
    if (!QFileInfo(directory).isReadable()) {
        listing.errorCode = QStringLiteral("permission-denied");
        return listing;
    }

    listing.relativePath = QDir::cleanPath(relativePath);
    if (listing.relativePath == QStringLiteral(".")) listing.relativePath.clear();
    if (!listing.relativePath.isEmpty()) {
        QString parent = listing.relativePath;
        const qsizetype separator = parent.lastIndexOf(QLatin1Char('/'));
        listing.parentRelativePath = separator < 0 ? QString() : parent.left(separator);
    }
    listing.breadcrumbs.push_back({rootLabel, QString()});
    QString accumulated;
    const QStringList breadcrumbParts = listing.relativePath.isEmpty()
        ? QStringList{} : listing.relativePath.split(QLatin1Char('/'));
    for (const QString &part : breadcrumbParts) {
        accumulated = accumulated.isEmpty() ? part : accumulated + QLatin1Char('/') + part;
        listing.breadcrumbs.push_back({part, accumulated});
    }

    const QString normalizedQuery = query.trimmed();
    std::set<FileExplorerEntry, decltype(&entryLess)> firstEntries(&entryLess);
    quint64 matchedCount = 0;
    QDirIterator iterator(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                          | QDir::System, QDirIterator::NoIteratorFlags);
    while (iterator.hasNext()) {
        if (cancellation.isCancellationRequested()) {
            listing.errorCode = QStringLiteral("cancelled");
            return listing;
        }
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        const QString name = info.fileName();
        if (!normalizedQuery.isEmpty() && !name.contains(normalizedQuery, Qt::CaseInsensitive))
            continue;

        const bool directoryEntry = info.isDir();
        const bool pathSafe = isSafeRelativeComponent(name);
        const bool unsafe = !pathSafe || info.isSymLink() || isReparsePoint(info.filePath());
        QString relative = listing.relativePath.isEmpty() ? name
            : listing.relativePath + QLatin1Char('/') + name;
        FileExplorerEntry entry;
        entry.name = name;
        entry.relativePath = relative;
        entry.directory = directoryEntry;
        entry.canNavigate = directoryEntry && !unsafe;
        entry.type = unsafe ? QStringLiteral("link")
                     : directoryEntry ? QStringLiteral("folder")
                     : info.suffix().isEmpty() ? QStringLiteral("file")
                                               : info.suffix().toUpper();
        entry.size = directoryEntry ? 0 : static_cast<quint64>(std::max<qint64>(0, info.size()));
        entry.modified = info.lastModified().toString(Qt::ISODate);
        entry.canOpen = !unsafe && !directoryEntry && info.isFile();
        entry.canRecycle = entry.canOpen;
        ++matchedCount;
        firstEntries.insert(entry);
        if (firstEntries.size() > MaximumResults + 1)
            firstEntries.erase(std::prev(firstEntries.end()));
    }
    if (cancellation.isCancellationRequested()) {
        listing.errorCode = QStringLiteral("cancelled");
        return listing;
    }

    listing.truncated = matchedCount > MaximumResults;
    listing.hasMore = offset + limit < std::min<quint64>(matchedCount, MaximumResults);
    listing.entries.reserve(std::min<qsizetype>(limit,
        std::max<qsizetype>(0, static_cast<qsizetype>(firstEntries.size()) - offset)));
    int index = 0;
    for (const FileExplorerEntry &entry : firstEntries) {
        if (index >= offset && listing.entries.size() < limit && index < MaximumResults)
            listing.entries.push_back(entry);
        ++index;
    }
    return listing;
}

QString NativeFileExplorerPlatform::resolveRegularFile(const QString &canonicalRoot,
                                                       const QString &relativePath,
                                                       QString *errorCode) const
{
    return resolveContainedPath(canonicalRoot, relativePath, true, false, errorCode);
}

bool NativeFileExplorerShell::supportsNativeActions() const noexcept
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

domain::AppError NativeFileExplorerShell::openFile(const QString &canonicalPath)
{
#if defined(Q_OS_WIN)
    const HINSTANCE result = ShellExecuteW(nullptr, L"open",
        reinterpret_cast<LPCWSTR>(canonicalPath.utf16()), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) return {};
    return domain::AppError(domain::ErrorCategory::Platform);
#else
    Q_UNUSED(canonicalPath)
    return domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
#endif
}

domain::AppError NativeFileExplorerShell::recycleFile(const QString &canonicalPath)
{
#if defined(Q_OS_WIN)
    ComApartment apartment;
    if (!apartment.ready()) return domain::AppError(domain::ErrorCategory::Platform);
    IShellItem *rawItem = nullptr;
    HRESULT result = SHCreateItemFromParsingName(
        reinterpret_cast<LPCWSTR>(canonicalPath.utf16()), nullptr, IID_PPV_ARGS(&rawItem));
    if (FAILED(result)) return domain::AppError(domain::ErrorCategory::NotFound);
    auto item = adoptCom(rawItem);

    IFileOperation *rawOperation = nullptr;
    result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&rawOperation));
    if (FAILED(result)) return domain::AppError(domain::ErrorCategory::Platform);
    auto operation = adoptCom(rawOperation);
    // Require recycling: ALLOWUNDO alone permits permanent deletion when the
    // destination cannot use the Recycle Bin. Do not offer that fallback.
    result = operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT
        | FOF_NOERRORUI | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD | FOFX_EARLYFAILURE);
    if (FAILED(result)) return domain::AppError(domain::ErrorCategory::Platform);
    result = operation->DeleteItem(item.get(), nullptr);
    if (FAILED(result)) return domain::AppError(domain::ErrorCategory::Platform);
    result = operation->PerformOperations();
    if (FAILED(result)) return domain::AppError(domain::ErrorCategory::Platform);
    BOOL aborted = FALSE;
    result = operation->GetAnyOperationsAborted(&aborted);
    if (FAILED(result) || aborted) return domain::AppError(domain::ErrorCategory::Platform);
    return {};
#else
    Q_UNUSED(canonicalPath)
    return domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
#endif
}

} // namespace something::platform
