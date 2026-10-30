#pragma once

#include "../domain/weather_currency.h"
#include "../storage/storage_worker.h"

namespace something::services {

class WeatherService final {
public:
    static storage::StorageCommand loadPrimaryLocation();
    static storage::StorageCommand savePrimaryLocation(
        double latitude, double longitude, domain::WeatherForecast forecast,
        QString updatedAt);
    static storage::StorageCommand saveRefreshedForecast(
        domain::WeatherForecast forecast, QString updatedAt);
    static storage::StorageCommand loadComparisonCities();
    static storage::StorageCommand addComparisonCity(
        domain::WeatherCity city, domain::WeatherForecast forecast, QString updatedAt);
    static storage::StorageCommand removeComparisonCity(QString identity);
    static storage::StorageCommand reorderComparisonCities(QVector<QString> identities);
    static storage::StorageCommand saveComparisonForecast(
        QString identity, domain::WeatherForecast forecast, QString updatedAt);
};

} // namespace something::services
