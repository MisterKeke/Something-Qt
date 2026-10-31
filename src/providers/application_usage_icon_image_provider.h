#pragma once

#include <QCache>
#include <QMutex>
#include <QQuickImageProvider>

namespace something::platform { class DesktopApplicationsPlatform; }

namespace something::providers {

class ApplicationUsageIconImageProvider final : public QQuickImageProvider {
public:
    explicit ApplicationUsageIconImageProvider(
        something::platform::DesktopApplicationsPlatform *platform);
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    QCache<QString, QImage> m_cache;
    QMutex m_mutex;
    something::platform::DesktopApplicationsPlatform *m_platform{nullptr};
};

} // namespace something::providers
