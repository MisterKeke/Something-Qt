#include "desktop_applications_platform.h"

#include <QFileInfo>
#include <QBuffer>
#include <QDir>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <cstring>
#include <limits>

namespace something::platform {
namespace {

#if defined(Q_OS_WIN)

struct WindowSnapshot final {
    HWND handle{nullptr};
    DWORD processId{0};
    QString title;
    QString processName;
    QString executablePath;
    QString className;
    LONG_PTR extendedStyle{0};
    bool visible{false};
    bool cloaked{false};
    bool minimized{false};
    bool active{false};
    bool taskbarRepresentative{false};
};

struct EnumerationContext final {
    QVector<WindowSnapshot> snapshots;
    application::CancellationToken cancellation;
    DWORD currentProcessId{0};
    HWND foreground{nullptr};
    bool cancelled{false};
    bool failed{false};
};

QString windowIdentity(DWORD processId, HWND handle)
{
    return QStringLiteral("%1:%2")
        .arg(processId)
        .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(handle)));
}

bool parseWindowIdentity(const QString &id, DWORD *processId, HWND *handle)
{
    if (!processId || !handle || id.isEmpty() || id.trimmed() != id) return false;
    const QStringList parts = id.split(QLatin1Char(':'));
    if (parts.size() != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return false;
    for (const QString &part : parts) {
        for (const QChar ch : part) if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    }
    bool processOk = false;
    bool handleOk = false;
    const qulonglong processValue = parts[0].toULongLong(&processOk, 10);
    const qulonglong handleValue = parts[1].toULongLong(&handleOk, 10);
    if (!processOk || !handleOk || processValue == 0 || handleValue == 0
        || processValue > MAXDWORD || handleValue > std::numeric_limits<quintptr>::max()) return false;
    *processId = static_cast<DWORD>(processValue);
    *handle = reinterpret_cast<HWND>(static_cast<quintptr>(handleValue));
    return true;
}

bool isShellClass(QString className)
{
    className = className.trimmed().toLower();
    return className == QStringLiteral("shell_traywnd")
        || className == QStringLiteral("shell_secondarytraywnd")
        || className == QStringLiteral("progman")
        || className == QStringLiteral("workerw");
}

bool isTaskbarRepresentative(HWND handle)
{
    HWND root = GetAncestor(handle, GA_ROOTOWNER);
    if (!root) root = handle;
    HWND current = root;
    QSet<quintptr> visited;
    while (current) {
        const quintptr identity = reinterpret_cast<quintptr>(current);
        if (visited.contains(identity)) return true;
        visited.insert(identity);
        const HWND popup = GetLastActivePopup(current);
        if (!popup || popup == current) return current == handle;
        if (IsWindowVisible(popup)) return popup == handle;
        current = popup;
    }
    return root == handle;
}

QString processImagePath(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    QString result;
    for (DWORD capacity : {DWORD(260), DWORD(32768)}) {
        QVector<wchar_t> buffer(static_cast<qsizetype>(capacity));
        DWORD length = capacity;
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length) && length > 0
            && length <= capacity) {
            result = QString::fromWCharArray(buffer.constData(), static_cast<qsizetype>(length));
            break;
        }
    }
    CloseHandle(process);
    return result;
}

WindowSnapshot readWindow(HWND handle, HWND foreground)
{
    WindowSnapshot item;
    item.handle = handle;
    if (!IsWindow(handle)) return item;
    item.visible = IsWindowVisible(handle) != FALSE;
    item.extendedStyle = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    item.processId = 0;
    GetWindowThreadProcessId(handle, &item.processId);
    item.active = handle == foreground;
    item.minimized = IsIconic(handle) != FALSE;
    item.taskbarRepresentative = isTaskbarRepresentative(handle);

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        item.cloaked = cloaked != 0;

    wchar_t classBuffer[256]{};
    const int classLength = GetClassNameW(handle, classBuffer, 256);
    if (classLength > 0) item.className = QString::fromWCharArray(classBuffer, classLength);

    const int titleLength = std::clamp(GetWindowTextLengthW(handle), 0, 4096);
    if (titleLength > 0) {
        QVector<wchar_t> title(static_cast<qsizetype>(titleLength + 1));
        const int copied = GetWindowTextW(handle, title.data(), titleLength + 1);
        if (copied > 0) item.title = QString::fromWCharArray(title.constData(), copied).trimmed();
    }
    return item;
}

bool shouldReadProcess(const WindowSnapshot &item, DWORD currentProcessId)
{
    if (!item.handle || !item.visible || item.cloaked || item.processId == 0
        || item.processId == currentProcessId || isShellClass(item.className)) return false;
    const bool appWindow = (item.extendedStyle & WS_EX_APPWINDOW) != 0;
    const bool toolWindow = (item.extendedStyle & WS_EX_TOOLWINDOW) != 0;
    if (toolWindow && !appWindow) return false;
    return appWindow || item.taskbarRepresentative;
}

BOOL CALLBACK enumerateWindow(HWND handle, LPARAM parameter)
{
    auto *context = reinterpret_cast<EnumerationContext *>(parameter);
    if (!context) return FALSE;
    if (context->cancellation.isCancellationRequested()) {
        context->cancelled = true;
        return FALSE;
    }
    WindowSnapshot item = readWindow(handle, context->foreground);
    if (shouldReadProcess(item, context->currentProcessId)) {
        item.executablePath = processImagePath(item.processId);
        item.processName = QFileInfo(item.executablePath).fileName();
        if (item.title.isEmpty() && item.processName.isEmpty()) return TRUE;
        context->snapshots.push_back(std::move(item));
    }
    return TRUE;
}

QImage imageFromIcon(HICON icon)
{
    if (!icon) return {};
    constexpr int iconSize = 32;
    HDC screen = GetDC(nullptr);
    if (!screen) return {};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = iconSize;
    info.bmiHeader.biHeight = -iconSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    if (!bitmap || !memory || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return {};
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    std::memset(pixels, 0, iconSize * iconSize * 4);
    const BOOL drawn = DrawIconEx(memory, 0, 0, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    QImage image;
    if (drawn) {
        const QImage view(static_cast<const uchar *>(pixels), iconSize, iconSize,
                          iconSize * 4, QImage::Format_ARGB32);
        image = view.copy();
    }
    if (previous) SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return image;
}

QImage executableIcon(const QString &path)
{
    if (path.isEmpty()) return {};
    SHFILEINFOW info{};
    const auto *nativePath = reinterpret_cast<const wchar_t *>(path.utf16());
    if (!SHGetFileInfoW(nativePath, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)
        || !info.hIcon) return {};
    QImage image = imageFromIcon(info.hIcon);
    DestroyIcon(info.hIcon);
    return image;
}

QImage windowIcon(const WindowSnapshot &snapshot)
{
    if (!snapshot.handle || !IsWindow(snapshot.handle)) return {};
    HICON icon = nullptr;
    DWORD_PTR result = 0;
    for (WPARAM kind : {WPARAM(ICON_BIG), WPARAM(ICON_SMALL2), WPARAM(ICON_SMALL)}) {
        if (SendMessageTimeoutW(snapshot.handle, WM_GETICON, kind, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &result) && result) {
            icon = reinterpret_cast<HICON>(result);
            break;
        }
    }
    if (!icon) {
        const ULONG_PTR classIcon = GetClassLongPtrW(snapshot.handle, GCLP_HICON);
        if (classIcon) icon = reinterpret_cast<HICON>(classIcon);
    }
    if (!icon) {
        const ULONG_PTR smallIcon = GetClassLongPtrW(snapshot.handle, GCLP_HICONSM);
        if (smallIcon) icon = reinterpret_cast<HICON>(smallIcon);
    }
    if (icon) {
        const QImage image = imageFromIcon(icon);
        if (!image.isNull()) return image;
    }
    return executableIcon(snapshot.executablePath);
}

QString iconDataUrl(const WindowSnapshot &snapshot)
{
    const QImage image = windowIcon(snapshot);
    if (image.isNull()) return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,%1")
        .arg(QString::fromLatin1(bytes.toBase64()));
}

QVector<WindowSnapshot> enumerateSnapshots(const application::CancellationToken &cancellation,
                                           bool *cancelled, bool *failed)
{
    EnumerationContext context;
    context.cancellation = cancellation;
    context.currentProcessId = GetCurrentProcessId();
    context.foreground = GetForegroundWindow();
    SetLastError(ERROR_SUCCESS);
    const BOOL result = EnumWindows(enumerateWindow, reinterpret_cast<LPARAM>(&context));
    if (cancelled) *cancelled = context.cancelled || cancellation.isCancellationRequested();
    if (!result && !context.cancelled && !cancellation.isCancellationRequested()
        && GetLastError() != ERROR_SUCCESS) context.failed = true;
    if (failed) *failed = context.failed;
    return context.snapshots;
}

#endif // Q_OS_WIN

} // namespace

bool NativeDesktopApplicationsPlatform::supported() const noexcept
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

domain::AppError NativeDesktopApplicationsPlatform::resolveExecutable(
    const QString &selectedPath, QString *canonicalPath) const
{
    if (!supported()) return domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
    if (!canonicalPath || selectedPath.trimmed().isEmpty())
        return domain::AppError(domain::ErrorCategory::Validation);
    const QFileInfo info(selectedPath.trimmed());
    if (!info.isAbsolute() || info.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0
        || !info.exists() || !info.isFile() || !info.isReadable())
        return domain::AppError(domain::ErrorCategory::Validation);
    const QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty()) return domain::AppError(domain::ErrorCategory::Validation);
    *canonicalPath = QDir::cleanPath(resolved);
    return domain::AppError::none();
}

QImage NativeDesktopApplicationsPlatform::extractExecutableIcon(
    const QString &canonicalPath, const application::CancellationToken &cancellation) const
{
    if (cancellation.isCancellationRequested()) return {};
#if defined(Q_OS_WIN)
    const QFileInfo file(canonicalPath);
    if (!file.exists() || !file.isFile()) return {};
    const QString pathKey = file.canonicalFilePath().toCaseFolded();
    const QString sourceKey = QStringLiteral("%1:%2:%3")
        .arg(pathKey).arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&m_iconMutex);
        const auto previous = m_iconSourceKeys.constFind(pathKey);
        if (previous != m_iconSourceKeys.cend() && *previous == sourceKey) {
            const auto cached = m_iconCache.constFind(pathKey);
            if (cached != m_iconCache.cend()) return *cached; // Null is a cached extraction failure too.
        }
    }
    const QImage extracted = executableIcon(canonicalPath);
    if (!cancellation.isCancellationRequested()) {
        QMutexLocker lock(&m_iconMutex);
        m_iconCache.insert(pathKey, extracted);
        m_iconSourceKeys.insert(pathKey, sourceKey);
        while (m_iconCache.size() > 128) {
            const QString key = m_iconCache.constBegin().key();
            m_iconCache.remove(key);
            m_iconSourceKeys.remove(key);
        }
    }
    return extracted;
#else
    Q_UNUSED(canonicalPath)
    return {};
#endif
}

domain::AppError NativeDesktopApplicationsPlatform::launchExecutable(
    const QString &canonicalPath, const application::CancellationToken &cancellation) const
{
    if (!supported()) return domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
    if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
    QString resolved;
    const domain::AppError validation = resolveExecutable(canonicalPath, &resolved);
    if (validation.isError()) return validation;
    if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
    // QProcess starts the validated executable directly with an empty argument
    // vector. The child process is detached and remains independent at shutdown.
    return QProcess::startDetached(resolved, {}, QFileInfo(resolved).absolutePath())
        ? domain::AppError::none()
        : domain::AppError(domain::ErrorCategory::Platform);
}

bool NativeRunningWindowsPlatform::supported() const noexcept
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

QVector<RunningWindow> NativeRunningWindowsPlatform::list(
    const application::CancellationToken &cancellation, domain::AppError *error)
{
    if (error) *error = domain::AppError::none();
    if (!supported()) {
        if (error) *error = domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
        return {};
    }
#if defined(Q_OS_WIN)
    bool cancelled = false;
    bool failed = false;
    const QVector<WindowSnapshot> snapshots = enumerateSnapshots(cancellation, &cancelled, &failed);
    if (cancelled) {
        if (error) *error = domain::AppError::cancelled();
        return {};
    }
    if (failed) {
        if (error) *error = domain::AppError(domain::ErrorCategory::Platform);
        return {};
    }
    QVector<RunningWindow> windows;
    QSet<QString> seen;
    QSet<QString> presentIcons;
    for (qsizetype index = 0; index < snapshots.size(); ++index) {
        if (index % 16 == 0 && cancellation.isCancellationRequested()) {
            if (error) *error = domain::AppError::cancelled();
            return {};
        }
        const WindowSnapshot &snapshot = snapshots.at(index);
        if (!IsWindow(snapshot.handle) || !snapshot.visible || snapshot.cloaked) continue;
        const bool appWindow = (snapshot.extendedStyle & WS_EX_APPWINDOW) != 0;
        const bool toolWindow = (snapshot.extendedStyle & WS_EX_TOOLWINDOW) != 0;
        if ((toolWindow && !appWindow) || (!appWindow && !snapshot.taskbarRepresentative)) continue;
        const QString id = windowIdentity(snapshot.processId, snapshot.handle);
        if (seen.contains(id)) continue;
        seen.insert(id);
        presentIcons.insert(id);
        QString icon;
        bool iconCached = false;
        {
            QMutexLocker lock(&m_iconMutex);
            iconCached = m_iconCache.contains(id);
            if (iconCached) icon = m_iconCache.value(id);
        }
        if (!iconCached) {
            icon = iconDataUrl(snapshot);
            QMutexLocker lock(&m_iconMutex);
            m_iconCache.insert(id, icon);
        }
        windows.push_back({id, snapshot.title, snapshot.processName, icon,
                           snapshot.minimized, snapshot.active});
    }
    {
        QMutexLocker lock(&m_iconMutex);
        for (auto it = m_iconCache.begin(); it != m_iconCache.end();) {
            if (!presentIcons.contains(it.key())) it = m_iconCache.erase(it);
            else ++it;
        }
        while (m_iconCache.size() > 256) m_iconCache.erase(m_iconCache.begin());
    }
    std::sort(windows.begin(), windows.end(), [](const RunningWindow &left,
                                                  const RunningWindow &right) {
        const int title = QString::compare(left.title, right.title, Qt::CaseInsensitive);
        if (title != 0) return title < 0;
        const int process = QString::compare(left.processName, right.processName, Qt::CaseInsensitive);
        if (process != 0) return process < 0;
        return left.id < right.id;
    });
    return windows;
#else
    Q_UNUSED(cancellation)
    return {};
#endif
}

WindowActionResult NativeRunningWindowsPlatform::activate(
    const QString &windowId, const application::CancellationToken &cancellation)
{
    if (!supported()) return WindowActionResult::Unsupported;
    if (cancellation.isCancellationRequested()) return WindowActionResult::Cancelled;
#if defined(Q_OS_WIN)
    DWORD expectedProcessId = 0;
    HWND handle = nullptr;
    if (!parseWindowIdentity(windowId, &expectedProcessId, &handle))
        return WindowActionResult::InvalidId;
    if (!IsWindow(handle)) return WindowActionResult::Unavailable;
    bool cancelled = false;
    bool failed = false;
    const QVector<WindowSnapshot> snapshots = enumerateSnapshots(cancellation, &cancelled, &failed);
    if (cancelled) return WindowActionResult::Cancelled;
    if (failed) return WindowActionResult::Failed;
    const auto it = std::find_if(snapshots.cbegin(), snapshots.cend(),
        [handle, expectedProcessId](const WindowSnapshot &item) {
            return item.handle == handle && item.processId == expectedProcessId;
        });
    if (it == snapshots.cend() || !IsWindow(handle)) return WindowActionResult::Unavailable;
    const bool appWindow = (it->extendedStyle & WS_EX_APPWINDOW) != 0;
    const bool toolWindow = (it->extendedStyle & WS_EX_TOOLWINDOW) != 0;
    if (!it->visible || it->cloaked || (toolWindow && !appWindow)
        || (!appWindow && !it->taskbarRepresentative)) return WindowActionResult::Unavailable;
    if (cancellation.isCancellationRequested()) return WindowActionResult::Cancelled;
    if (it->minimized) ShowWindowAsync(handle, SW_RESTORE);
    BringWindowToTop(handle);
    return SetForegroundWindow(handle) ? WindowActionResult::Activated
                                       : WindowActionResult::ActivationRefused;
#else
    Q_UNUSED(windowId)
    return WindowActionResult::Unsupported;
#endif
}

} // namespace something::platform
