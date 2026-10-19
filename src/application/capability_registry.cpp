#include "capability_registry.h"

#include <algorithm>

namespace something::application {

CapabilityRegistry::CapabilityRegistry(QObject *parent)
    : QObject(parent)
{
}

void CapabilityRegistry::markAvailable(const QString &name)
{
    if (name.isEmpty()) {
        return;
    }
    Capability capability;
    capability.available = true;
    capability.reason = something::domain::AppError::none();
    m_capabilities.insert(name, capability);
    emit capabilitiesChanged();
}

void CapabilityRegistry::markUnavailable(const QString &name,
                                         something::domain::AppError reason)
{
    if (name.isEmpty()) {
        return;
    }
    Capability capability;
    capability.available = false;
    capability.reason = something::domain::AppError(
        something::domain::ErrorCategory::CapabilityUnavailable);
    if (reason.category() == something::domain::ErrorCategory::Platform) {
        capability.reason = reason;
    }
    m_capabilities.insert(name, capability);
    emit capabilitiesChanged();
}

QStringList CapabilityRegistry::unavailableCapabilities() const
{
    QStringList result;
    for (auto it = m_capabilities.cbegin(); it != m_capabilities.cend(); ++it) {
        if (!it->available) {
            result.push_back(it.key() + QStringLiteral(": ") + it->reason.userMessage());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool CapabilityRegistry::isAvailable(const QString &name) const
{
    const auto it = m_capabilities.constFind(name);
    return it != m_capabilities.cend() && it->available;
}

} // namespace something::application
