#pragma once

#include "../domain/app_error.h"

#include <QObject>
#include <QHash>
#include <QStringList>

namespace something::application {

class CapabilityRegistry final : public QObject {
    Q_OBJECT

public:
    explicit CapabilityRegistry(QObject *parent = nullptr);

    void markAvailable(const QString &name);
    void markUnavailable(const QString &name, something::domain::AppError reason);
    QStringList unavailableCapabilities() const;
    bool isAvailable(const QString &name) const;

signals:
    void capabilitiesChanged();

private:
    struct Capability final {
        bool available{false};
        something::domain::AppError reason{something::domain::ErrorCategory::CapabilityUnavailable};
    };

    QHash<QString, Capability> m_capabilities;
};

} // namespace something::application
