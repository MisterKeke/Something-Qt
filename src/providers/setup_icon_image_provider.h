#pragma once

#include <QCache>
#include <QMutex>
#include <QQuickImageProvider>

namespace something::providers {

class SetupIconImageProvider final : public QQuickImageProvider {
public:
    explicit SetupIconImageProvider(QString profileDirectory);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    void invalidate(const QString &filename);
private:
    QString m_directory;
    QCache<QString, QImage> m_cache;
    QMutex m_mutex;
};

} // namespace something::providers
