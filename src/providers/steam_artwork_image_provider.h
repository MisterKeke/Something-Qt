#pragma once

#include <QCache>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

namespace something::providers {

class SteamArtworkImageProvider final : public QQuickImageProvider {
public:
    explicit SteamArtworkImageProvider(QString profileDirectory);
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
    void invalidate(const QString &filename);

private:
    QString m_directory;
    QCache<QString, QImage> m_cache;
    QMutex m_mutex;
};

} // namespace something::providers
