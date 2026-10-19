#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

namespace something::providers {

class ProviderImageProvider final : public QQuickImageProvider {
public:
    ProviderImageProvider();
    QString cacheImage(const QString &contentKey, QImage image);
    QString cachedSource(const QString &contentKey);
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    QMutex m_mutex;
    QCache<QString, QImage> m_images;
};

} // namespace something::providers
