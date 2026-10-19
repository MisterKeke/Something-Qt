#pragma once

#include "../domain/app_error.h"

#include <QObject>
#include <QPointer>

#ifdef SOMETHING_QT_HAS_POSITIONING
class QGeoPositionInfoSource;
#endif

namespace something::platform {

class LocationProvider : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit LocationProvider(QObject *parent = nullptr) : QObject(parent) {}
    ~LocationProvider() override = default;

    virtual bool available() const noexcept = 0;
    virtual void request(quint64 requestId) = 0;
    virtual void cancel(quint64 requestId) = 0;

signals:
    void positionReady(quint64 requestId, double latitude, double longitude,
                       something::domain::AppError error);
};

class NativeLocationProvider final : public LocationProvider {
    Q_OBJECT

public:
    explicit NativeLocationProvider(QObject *parent = nullptr);
    ~NativeLocationProvider() override;
    bool available() const noexcept override;
    void request(quint64 requestId) override;
    void cancel(quint64 requestId) override;

private:
    void finishWithError(something::domain::AppError error);

#ifdef SOMETHING_QT_HAS_POSITIONING
    QPointer<QGeoPositionInfoSource> m_source;
#endif
    quint64 m_activeRequestId{0};
};

} // namespace something::platform
