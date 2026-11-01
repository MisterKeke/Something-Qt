#include "application_usage_platform.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>

#include <utility>

namespace something::platform {
namespace {

Q_LOGGING_CATEGORY(lcUsageObservation, "something.usage.observation", QtInfoMsg)

#if defined(Q_OS_WIN)
QMutex g_observerMutex;
QPointer<NativeApplicationUsagePlatform> g_observer;
#endif

} // namespace

NativeApplicationUsagePlatform::NativeApplicationUsagePlatform(QObject *parent)
    : ApplicationUsagePlatform(parent)
{
    m_reconciliationTimer.setInterval(reconciliationIntervalMs());
    m_reconciliationTimer.setTimerType(Qt::CoarseTimer);
    m_coalescedEventTimer.setInterval(100);
    m_coalescedEventTimer.setSingleShot(true);
    connect(&m_reconciliationTimer, &QTimer::timeout,
            this, &NativeApplicationUsagePlatform::scan);
    connect(&m_coalescedEventTimer, &QTimer::timeout,
            this, &NativeApplicationUsagePlatform::scan);
}

NativeApplicationUsagePlatform::~NativeApplicationUsagePlatform()
{
    stop();
}

bool NativeApplicationUsagePlatform::supported() const noexcept
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

void NativeApplicationUsagePlatform::start()
{
    if (m_started || !supported()) return;
    m_started = true;
    if (auto *application = QCoreApplication::instance()) {
        application->installNativeEventFilter(this);
        m_powerFilterInstalled = true;
    }
#if defined(Q_OS_WIN)
    {
        QMutexLocker lock(&g_observerMutex);
        g_observer = this;
    }
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const DWORD events[] = {
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_MINIMIZESTART,
        EVENT_SYSTEM_MINIMIZEEND,
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_HIDE,
        EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_LOCATIONCHANGE
    };
    for (DWORD event : events) {
        const HWINEVENTHOOK hook = SetWinEventHook(
            event, event, nullptr, &NativeApplicationUsagePlatform::eventCallback,
            0, 0, flags);
        if (hook) m_eventHooks.push_back(hook);
    }
#endif
    m_reconciliationTimer.start();
    scheduleScan();
}

void NativeApplicationUsagePlatform::stop()
{
    if (!m_started) return;
    m_started = false;
    m_scanDispatchPending.store(false, std::memory_order_release);
    m_reconciliationTimer.stop();
    m_coalescedEventTimer.stop();
    if (m_powerFilterInstalled) {
        if (auto *application = QCoreApplication::instance())
            application->removeNativeEventFilter(this);
        m_powerFilterInstalled = false;
    }
#if defined(Q_OS_WIN)
    for (const HWINEVENTHOOK hook : std::as_const(m_eventHooks)) {
        if (hook) UnhookWinEvent(hook);
    }
    m_eventHooks.clear();
    QMutexLocker lock(&g_observerMutex);
    if (g_observer == this) g_observer.clear();
#endif
}

bool NativeApplicationUsagePlatform::nativeEventFilter(const QByteArray &eventType,
                                                        void *message,
                                                        qintptr *result)
{
    Q_UNUSED(eventType);
    Q_UNUSED(result);
#if defined(Q_OS_WIN)
    const auto *nativeMessage = static_cast<const MSG *>(message);
    if (!nativeMessage || nativeMessage->message != WM_POWERBROADCAST) return false;
    switch (nativeMessage->wParam) {
    case PBT_APMSUSPEND:
        emit suspendStateChanged(true);
        break;
    case PBT_APMRESUMECRITICAL:
    case PBT_APMRESUMEAUTOMATIC:
    case PBT_APMRESUMESUSPEND:
        emit suspendStateChanged(false);
        scheduleScan();
        break;
    default:
        break;
    }
#else
    Q_UNUSED(message);
#endif
    return false;
}

void NativeApplicationUsagePlatform::scheduleScan()
{
    if (m_started && !m_coalescedEventTimer.isActive())
        m_coalescedEventTimer.start();
}

void NativeApplicationUsagePlatform::scan()
{
    if (!m_started) return;
    m_scanDispatchPending.store(false, std::memory_order_release);
    QElapsedTimer elapsed;
    elapsed.start();
    bool succeeded = false;
    QString diagnostic;
    const auto applications = snapshot(&succeeded, &diagnostic);
    ++m_completedScanCount;
    if (m_completedScanCount % 12 == 0) {
        qCInfo(lcUsageObservation).nospace()
            << "scan=" << m_completedScanCount
            << " last_elapsed_ms=" << elapsed.elapsed()
            << " native_callbacks=" << m_nativeCallbackCount.exchange(0)
            << " queued_dispatches=" << m_queuedDispatchCount.exchange(0)
            << " coalesced_callbacks=" << m_coalescedCallbackCount.exchange(0)
            << " observations=" << applications.size()
            << " succeeded=" << succeeded;
    }
    emit observationsReady(applications, succeeded, diagnostic);
}

#if defined(Q_OS_WIN)
void CALLBACK NativeApplicationUsagePlatform::eventCallback(
    HWINEVENTHOOK hook, DWORD event, HWND window, LONG objectId, LONG childId,
    DWORD eventThread, DWORD eventTime)
{
    Q_UNUSED(hook);
    Q_UNUSED(window);
    Q_UNUSED(eventThread);
    Q_UNUSED(eventTime);
    if (event != EVENT_SYSTEM_FOREGROUND
        && (objectId != OBJID_WINDOW || childId != CHILDID_SELF)) return;
    QPointer<NativeApplicationUsagePlatform> observer;
    {
        QMutexLocker lock(&g_observerMutex);
        observer = g_observer;
    }
    if (!observer) return;
    observer->m_nativeCallbackCount.fetch_add(1, std::memory_order_relaxed);
    if (observer->m_scanDispatchPending.exchange(true, std::memory_order_acq_rel)) {
        observer->m_coalescedCallbackCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool queued = QMetaObject::invokeMethod(observer.data(), [observer] {
        if (observer) observer->scheduleScan();
    }, Qt::QueuedConnection);
    if (queued) {
        observer->m_queuedDispatchCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        observer->m_scanDispatchPending.store(false, std::memory_order_release);
    }
}
#endif

} // namespace something::platform
