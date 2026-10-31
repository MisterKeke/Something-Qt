#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QObject>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <QTimer>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace something::domain { class AppError; }

namespace something::platform {

struct ApplicationUsageObservation final {
    QString applicationId;
    QString executablePath;
    QString executableName;
    QString displayName;
    QString processIdentity;
    QString state;
};

class ApplicationUsagePlatform : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~ApplicationUsagePlatform() override = default;
    virtual bool supported() const noexcept = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void observationsReady(QVector<something::platform::ApplicationUsageObservation> applications,
                          bool succeeded, QString diagnostic);
    void suspendStateChanged(bool suspended);
};

class NativeApplicationUsagePlatform final : public ApplicationUsagePlatform,
                                             public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit NativeApplicationUsagePlatform(QObject *parent = nullptr);
    ~NativeApplicationUsagePlatform() override;
    bool supported() const noexcept override;
    void start() override;
    void stop() override;
    int reconciliationIntervalMs() const noexcept { return 5000; }
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;

private:
    void scheduleScan();
    void scan();
    QVector<ApplicationUsageObservation> snapshot(bool *succeeded, QString *diagnostic) const;
#if defined(Q_OS_WIN)
    static void CALLBACK eventCallback(HWINEVENTHOOK hook, DWORD event, HWND window,
                                       LONG objectId, LONG childId,
                                       DWORD eventThread, DWORD eventTime);
#endif

    QTimer m_reconciliationTimer;
    QTimer m_coalescedEventTimer;
#if defined(Q_OS_WIN)
    QVector<HWINEVENTHOOK> m_eventHooks;
#endif
    bool m_started{false};
    bool m_powerFilterInstalled{false};
};

} // namespace something::platform

Q_DECLARE_METATYPE(something::platform::ApplicationUsageObservation)
