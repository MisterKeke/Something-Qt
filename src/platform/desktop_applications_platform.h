#pragma once

#include "../application/cancellation_token.h"
#include "../domain/app_error.h"

#include <QImage>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

namespace something::platform {

class DesktopApplicationsPlatform {
public:
    virtual ~DesktopApplicationsPlatform() = default;
    virtual bool supported() const noexcept = 0;
    virtual domain::AppError resolveExecutable(const QString &selectedPath,
                                              QString *canonicalPath) const = 0;
    virtual QImage extractExecutableIcon(const QString &canonicalPath,
                                         const application::CancellationToken &cancellation) const = 0;
    virtual domain::AppError launchExecutable(const QString &canonicalPath,
                                              const application::CancellationToken &cancellation) const = 0;
};

struct RunningWindow final {
    QString id;
    QString title;
    QString processName;
    QString iconDataUrl;
    bool minimized{false};
    bool active{false};
};

enum class WindowActionResult { Activated, Unsupported, InvalidId, Unavailable,
                                ActivationRefused, Cancelled, Failed };

class RunningWindowsPlatform {
public:
    virtual ~RunningWindowsPlatform() = default;
    virtual bool supported() const noexcept = 0;
    virtual QVector<RunningWindow> list(const application::CancellationToken &cancellation,
                                        domain::AppError *error) = 0;
    virtual WindowActionResult activate(const QString &windowId,
                                        const application::CancellationToken &cancellation) = 0;
};

class NativeDesktopApplicationsPlatform final : public DesktopApplicationsPlatform {
public:
    bool supported() const noexcept override;
    domain::AppError resolveExecutable(const QString &selectedPath,
                                      QString *canonicalPath) const override;
    QImage extractExecutableIcon(const QString &canonicalPath,
                                 const application::CancellationToken &cancellation) const override;
    domain::AppError launchExecutable(const QString &canonicalPath,
                                      const application::CancellationToken &cancellation) const override;

private:
    mutable QMutex m_iconMutex;
    mutable QHash<QString, QImage> m_iconCache;
    mutable QHash<QString, QString> m_iconSourceKeys;
};

class NativeRunningWindowsPlatform final : public RunningWindowsPlatform {
public:
    bool supported() const noexcept override;
    QVector<RunningWindow> list(const application::CancellationToken &cancellation,
                                domain::AppError *error) override;
    WindowActionResult activate(const QString &windowId,
                                const application::CancellationToken &cancellation) override;

private:
    QMutex m_iconMutex;
    QHash<QString, QString> m_iconCache;
};

} // namespace something::platform
