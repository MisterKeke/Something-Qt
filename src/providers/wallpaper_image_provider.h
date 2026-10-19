#pragma once

#include <QCache>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

namespace something::providers {

class WallpaperImageProvider final : public QQuickImageProvider {
public:
    explicit WallpaperImageProvider(QString profileDirectory);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
    void invalidate(const QString &id);

private:
    QString m_directory;
    QCache<QString, QImage> m_cache;
    QMutex m_cacheMutex;
};

} // namespace something::providers
