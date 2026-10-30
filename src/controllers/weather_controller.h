#pragma once

#include "../application/application_service_graph.h"
#include "../domain/clock.h"
#include "../models/weather_forecast_model.h"
#include "../platform/location_provider.h"

#include <QObject>
#include <QHash>
#include <QString>
#include <QVariantMap>

namespace something::controllers {

class WeatherController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::WeatherForecastModel *primaryForecast READ primaryForecast CONSTANT)
    Q_PROPERTY(something::models::WeatherForecastModel *cityForecast READ cityForecast CONSTANT)
    Q_PROPERTY(QVariantMap primaryCurrent READ primaryCurrent NOTIFY primaryChanged)
    Q_PROPERTY(QVariantMap primaryToday READ primaryToday NOTIFY primaryChanged)
    Q_PROPERTY(QVariantMap primarySolar READ primarySolar NOTIFY primaryChanged)
    Q_PROPERTY(QVariantMap cityCurrent READ cityCurrent NOTIFY cityChanged)
    Q_PROPERTY(QVariantMap citySolar READ citySolar NOTIFY cityChanged)
    Q_PROPERTY(QString primaryLocation READ primaryLocation NOTIFY primaryChanged)
    Q_PROPERTY(QString primaryUpdatedAt READ primaryUpdatedAt NOTIFY primaryChanged)
    Q_PROPERTY(QString primaryStatus READ primaryStatus NOTIFY primaryChanged)
    Q_PROPERTY(QString primaryError READ primaryError NOTIFY primaryChanged)
    Q_PROPERTY(QString cityLocation READ cityLocation NOTIFY cityChanged)
    Q_PROPERTY(QString cityError READ cityError NOTIFY cityChanged)
    Q_PROPERTY(bool primaryLoading READ primaryLoading NOTIFY primaryChanged)
    Q_PROPERTY(bool primaryRefreshing READ primaryRefreshing NOTIFY primaryChanged)
    Q_PROPERTY(bool citySearching READ citySearching NOTIFY cityChanged)
    Q_PROPERTY(bool locationAvailable READ locationAvailable NOTIFY primaryChanged)

public:
    WeatherController(application::ApplicationServiceGraph *graph,
                      const domain::Clock *clock,
                      platform::LocationProvider *locationProvider,
                      QObject *parent = nullptr);

    models::WeatherForecastModel *primaryForecast() noexcept { return &m_primaryForecast; }
    models::WeatherForecastModel *cityForecast() noexcept { return &m_cityForecast; }
    QVariantMap primaryCurrent() const { return m_primaryCurrent; }
    QVariantMap primaryToday() const { return m_primaryToday; }
    QVariantMap primarySolar() const { return m_primarySolar; }
    QVariantMap cityCurrent() const { return m_cityCurrent; }
    QVariantMap citySolar() const { return m_citySolar; }
    QString primaryLocation() const { return m_primaryLocation; }
    QString primaryUpdatedAt() const { return m_primaryUpdatedAt; }
    QString primaryStatus() const { return m_primaryStatus; }
    QString primaryError() const { return m_primaryError; }
    QString cityLocation() const { return m_cityLocation; }
    QString cityError() const { return m_cityError; }
    bool primaryLoading() const noexcept { return m_primaryLoading; }
    bool primaryRefreshing() const noexcept { return m_primaryRefreshing; }
    bool citySearching() const noexcept { return m_citySearching; }
    bool locationAvailable() const noexcept { return m_locationAvailable; }

    Q_INVOKABLE void refreshPrimary();
    Q_INVOKABLE void requestDeviceLocation();
    Q_INVOKABLE void searchCity(QString city);
    Q_INVOKABLE void cancelCitySearch();
    Q_INVOKABLE void updateSolarInfo();

signals:
    void primaryChanged();
    void cityChanged();

private:
    enum class Kind { LoadPrimary, NativeLocation, PrimaryForecast, PrimaryParse,
                      PrimarySave, CityGeocode, CityGeocodeParse,
                      CityForecast, CityForecastParse };
    struct Pending final {
        Kind kind{Kind::LoadPrimary};
        quint64 generation{0};
        double latitude{0.0};
        double longitude{0.0};
        bool saveLocation{false};
        domain::WeatherCity city;
        QString updatedAt;
    };

    void onLifecycleState(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onHttpsFinished(quint64 id, const QByteArray &body, domain::AppError error);
    void onValueJobFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onNativeLocationReady(quint64 id, double latitude, double longitude,
                               domain::AppError error);
    quint64 submitStorage(Kind kind, storage::StorageCommand command,
                          QString freshnessKey, quint64 generation);
    quint64 submitNetwork(Kind kind, providers::ProviderRequest request,
                         QString freshnessKey, quint64 generation,
                         double latitude = 0.0, double longitude = 0.0,
                         bool saveLocation = false, domain::WeatherCity city = {});
    quint64 submitParse(Kind kind, QByteArray body, QString freshnessKey,
                        quint64 generation, double latitude = 0.0,
                        double longitude = 0.0, bool saveLocation = false,
                        domain::WeatherCity city = {});
    void cancelPending(quint64 id);
    void cancelPrimaryOperations();
    void cancelCityOperations();
    void beginPrimaryLoad();
    void beginPrimaryForecast(double latitude, double longitude,
                              bool saveLocation, quint64 generation);
    void beginCityForecast(const domain::WeatherCity &city, quint64 generation);
    void savePrimaryForecast(const domain::WeatherForecast &forecast,
                             double latitude, double longitude,
                             bool saveLocation, quint64 generation);
    void publishPrimary(const domain::WeatherForecast &forecast, QString updatedAt,
                        bool fromCache);
    void publishCity(const domain::WeatherCity &city,
                     const domain::WeatherForecast &forecast);
    void setPrimaryError(domain::AppError error, const QString &fallback);
    void setCityError(domain::AppError error, const QString &fallback);
    void requestNativeLocation(quint64 generation);

    application::ApplicationServiceGraph *m_graph{nullptr};
    const domain::Clock *m_clock{nullptr};
    platform::LocationProvider *m_locationProvider{nullptr};
    models::WeatherForecastModel m_primaryForecast;
    models::WeatherForecastModel m_cityForecast;
    QHash<quint64, Pending> m_pending;
    QHash<quint64, domain::WeatherForecast> m_stagedPrimary;
    QVariantMap m_primaryCurrent;
    QVariantMap m_primaryToday;
    QVariantMap m_primarySolar;
    QVariantMap m_cityCurrent;
    QVariantMap m_citySolar;
    domain::WeatherForecast m_primarySolarForecast;
    domain::WeatherForecast m_citySolarForecast;
    QString m_primaryLocation{QStringLiteral("Your location")};
    QString m_primaryUpdatedAt;
    QString m_primaryStatus;
    QString m_primaryError;
    QString m_cityLocation;
    QString m_cityError;
    quint64 m_primaryGeneration{0};
    quint64 m_cityGeneration{0};
    quint64 m_locationOperation{0};
    double m_primaryLatitude{0.0};
    double m_primaryLongitude{0.0};
    bool m_ready{false};
    bool m_locationAvailable{false};
    bool m_hasLocation{false};
    bool m_primaryLoading{false};
    bool m_primaryRefreshing{false};
    bool m_citySearching{false};
};

} // namespace something::controllers
