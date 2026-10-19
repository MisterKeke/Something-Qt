#pragma once

#include <QMetaType>
#include <QString>

namespace something::domain {

enum class ErrorCategory {
    None,
    Validation,
    NotFound,
    Conflict,
    Cancelled,
    Network,
    Timeout,
    Connection,
    Http,
    Provider,
    InvalidResponse,
    ResponseTooLarge,
    Storage,
    Busy,
    Platform,
    CapabilityUnavailable,
    InstanceAlreadyRunning,
    InstanceLock,
    Startup,
    Internal,
};

class AppError final {
public:
    constexpr AppError() noexcept = default;
    explicit constexpr AppError(ErrorCategory category) noexcept
        : m_category(category)
    {
    }

    static constexpr AppError none() noexcept { return AppError(ErrorCategory::None); }
    static constexpr AppError cancelled() noexcept { return AppError(ErrorCategory::Cancelled); }

    constexpr ErrorCategory category() const noexcept { return m_category; }
    constexpr bool isError() const noexcept
    {
        return m_category != ErrorCategory::None && m_category != ErrorCategory::Cancelled;
    }
    constexpr bool isCancelled() const noexcept { return m_category == ErrorCategory::Cancelled; }

    QString userMessage() const;

private:
    ErrorCategory m_category{ErrorCategory::None};
};

} // namespace something::domain

Q_DECLARE_METATYPE(something::domain::AppError)
