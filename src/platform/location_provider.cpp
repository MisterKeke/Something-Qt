#include "location_provider.h"

#include "../domain/weather_currency.h"

#include <QThread>

#ifdef SOMETHING_QT_HAS_POSITIONING
#include <QGeoCoordinate>
#include <QGeoPositionInfo>
#include <QGeoPositionInfoSource>
#endif

namespace something::platform {

NativeLocationProvider::NativeLocationProvider(QObject *parent)
    : LocationProvider(parent)
{
    Q_ASSERT(QThread::currentThread() == thread());
#ifdef SOMETHING_QT_HAS_POSITIONING
    m_source = QGeoPositionInfoSource::createDefaultSource(this);
    if (m_source) {
        connect(m_source, &QGeoPositionInfoSource::positionUpdated, this,
                [this](const QGeoPositionInfo &position) {
            if (!m_activeRequestId) return;
            const QGeoCoordinate coordinate = position.coordinate();
            const quint64 requestId = m_activeRequestId;
            m_activeRequestId = 0;
            if (!coordinate.isValid()
                || !domain::isValidWeatherCoordinates(coordinate.latitude(), coordinate.longitude())) {
                emit positionReady(requestId, 0.0, 0.0,
                                   domain::AppError(domain::ErrorCategory::Platform));
                return;
            }
            emit positionReady(requestId, coordinate.latitude(), coordinate.longitude(),
                               domain::AppError::none());
        });
        connect(m_source, &QGeoPositionInfoSource::errorOccurred, this,
                [this](QGeoPositionInfoSource::Error sourceError) {
            if (!m_activeRequestId) return;
            switch (sourceError) {
            case QGeoPositionInfoSource::UpdateTimeoutError:
                finishWithError(domain::AppError(domain::ErrorCategory::Timeout));
                break;
            case QGeoPositionInfoSource::AccessError:
                finishWithError(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
                break;
            case QGeoPositionInfoSource::NoError:
                break;
            case QGeoPositionInfoSource::ClosedError:
            case QGeoPositionInfoSource::UnknownSourceError:
                finishWithError(domain::AppError(domain::ErrorCategory::Platform));
                break;
            }
        });
    }
#endif
}

NativeLocationProvider::~NativeLocationProvider() = default;

bool NativeLocationProvider::available() const noexcept
{
#ifdef SOMETHING_QT_HAS_POSITIONING
    return !m_source.isNull();
#else
    return false;
#endif
}

void NativeLocationProvider::request(quint64 requestId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!requestId) return;
    if (!available()) {
        emit positionReady(requestId, 0.0, 0.0,
                           domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
        return;
    }
    if (m_activeRequestId) {
        const quint64 superseded = m_activeRequestId;
        m_activeRequestId = 0;
        emit positionReady(superseded, 0.0, 0.0, domain::AppError::cancelled());
    }
    m_activeRequestId = requestId;
#ifdef SOMETHING_QT_HAS_POSITIONING
    // Mirrors the reference's ten-second initial location attempt. Qt's
    // positioning source reports timeout and access failures asynchronously.
    m_source->requestUpdate(10'000);
#endif
}

void NativeLocationProvider::cancel(quint64 requestId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!requestId || requestId != m_activeRequestId) return;
    m_activeRequestId = 0;
    emit positionReady(requestId, 0.0, 0.0, domain::AppError::cancelled());
}

void NativeLocationProvider::finishWithError(domain::AppError error)
{
    const quint64 requestId = m_activeRequestId;
    if (!requestId) return;
    m_activeRequestId = 0;
    emit positionReady(requestId, 0.0, 0.0, error);
}

} // namespace something::platform
