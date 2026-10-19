#include "weather_service.h"

#include "../providers/feature_parsers.h"

#include <QDate>

#include <cmath>
#include <utility>

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

bool validForecast(const domain::WeatherForecast &forecast)
{
    if (forecast.days.isEmpty()) return false;
    for (const domain::WeatherDay &day : forecast.days) {
        if (!QDate::fromString(day.date, Qt::ISODate).isValid()
            || !std::isfinite(day.maximumCelsius) || !std::isfinite(day.minimumCelsius)
            || !std::isfinite(day.precipitationProbability)) return false;
    }
    const QByteArray encoded = providers::serializeWeatherForecast(forecast);
    return providers::parseWeatherForecast(encoded).succeeded();
}

} // namespace

storage::StorageCommand WeatherService::loadPrimaryLocation()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto loaded = repositories.weather.load();
        if (!loaded.succeeded()) return failed(loaded.error);
        storage::StorageReply reply;
        if (!loaded.value) {
            reply.values.insert(QStringLiteral("found"), false);
            return reply;
        }
        const storage::WeatherLocation &location = *loaded.value;
        reply.values.insert(QStringLiteral("found"), true);
        reply.values.insert(QStringLiteral("latitude"), location.latitude);
        reply.values.insert(QStringLiteral("longitude"), location.longitude);
        reply.values.insert(QStringLiteral("forecastJson"), location.forecastJson);
        reply.values.insert(QStringLiteral("updatedAt"), location.forecastUpdatedAt);
        return reply;
    };
}

storage::StorageCommand WeatherService::savePrimaryLocation(
    double latitude, double longitude, domain::WeatherForecast forecast, QString updatedAt)
{
    return [latitude, longitude, forecast = std::move(forecast),
            updatedAt = std::move(updatedAt)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (!domain::isValidWeatherCoordinates(latitude, longitude)
            || updatedAt.isEmpty() || !validForecast(forecast)) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }
        const auto saved = repositories.weather.saveLocationAndForecast(
            latitude, longitude,
            QString::fromUtf8(providers::serializeWeatherForecast(forecast)), updatedAt);
        if (!saved.succeeded()) return failed(saved.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("saved"), true);
        return reply;
    };
}

storage::StorageCommand WeatherService::saveRefreshedForecast(
    domain::WeatherForecast forecast, QString updatedAt)
{
    return [forecast = std::move(forecast), updatedAt = std::move(updatedAt)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (updatedAt.isEmpty() || !validForecast(forecast)) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }
        const auto saved = repositories.weather.saveForecast(
            QString::fromUtf8(providers::serializeWeatherForecast(forecast)), updatedAt);
        if (!saved.succeeded()) return failed(saved.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("saved"), true);
        return reply;
    };
}

} // namespace something::services
