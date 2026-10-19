#include "app_error.h"

namespace something::domain {

QString AppError::userMessage() const
{
    switch (m_category) {
    case ErrorCategory::None:
    case ErrorCategory::Cancelled:
        return {};
    case ErrorCategory::Validation:
        return QStringLiteral("Check the entered information.");
    case ErrorCategory::NotFound:
        return QStringLiteral("The requested item could not be found.");
    case ErrorCategory::Conflict:
        return QStringLiteral("This item changed. Reload it and try again.");
    case ErrorCategory::Network:
        return QStringLiteral("The network request could not be completed.");
    case ErrorCategory::Timeout:
        return QStringLiteral("The service did not respond in time.");
    case ErrorCategory::Connection:
        return QStringLiteral("Could not connect to the service.");
    case ErrorCategory::Http:
        return QStringLiteral("The service returned an error response.");
    case ErrorCategory::Provider:
        return QStringLiteral("The service could not complete the request.");
    case ErrorCategory::InvalidResponse:
        return QStringLiteral("The service returned data that could not be used.");
    case ErrorCategory::ResponseTooLarge:
        return QStringLiteral("The service response exceeded the allowed size.");
    case ErrorCategory::Storage:
        return QStringLiteral("Local data could not be opened or saved.");
    case ErrorCategory::Busy:
        return QStringLiteral("Background work is busy. Try again shortly.");
    case ErrorCategory::Platform:
        return QStringLiteral("This action is unavailable on this device.");
    case ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("This capability is currently unavailable.");
    case ErrorCategory::InstanceAlreadyRunning:
        return QStringLiteral("Something is already running for this profile.");
    case ErrorCategory::InstanceLock:
        return QStringLiteral("The profile could not be locked for exclusive use.");
    case ErrorCategory::Startup:
        return QStringLiteral("Something could not start. Check the application setup.");
    case ErrorCategory::Internal:
        return QStringLiteral("Something went wrong. Try again.");
    }

    return QStringLiteral("Something went wrong. Try again.");
}

} // namespace something::domain
