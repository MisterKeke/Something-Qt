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
};

} // namespace something::services
