#include "weather_service.h"

#include "../providers/feature_parsers.h"

#include <QDate>
#include <QVariantList>
#include <QVariantMap>

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

storage::StorageCommand WeatherService::loadComparisonCities()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto loaded = repositories.weatherComparison.load();
        if (!loaded.succeeded()) return failed(loaded.error);
        QVariantList cities;
        cities.reserve(loaded.value.size());
        for (const storage::WeatherComparisonLocation &entry : loaded.value) {
            QVariantMap city{
                {QStringLiteral("identity"), domain::stableWeatherCityIdentity(entry.city)},
                {QStringLiteral("geocodingId"), entry.city.geocodingId},
                {QStringLiteral("name"), entry.city.name},
                {QStringLiteral("administrativeArea"), entry.city.administrativeArea},
                {QStringLiteral("country"), entry.city.country},
                {QStringLiteral("timezone"), entry.city.timezone},
                {QStringLiteral("latitude"), entry.city.latitude},
                {QStringLiteral("longitude"), entry.city.longitude},
                {QStringLiteral("displayOrder"), entry.displayOrder},
                {QStringLiteral("forecastJson"), entry.forecastJson},
                {QStringLiteral("updatedAt"), entry.forecastUpdatedAt},
            };
            cities.push_back(std::move(city));
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("cities"), cities);
        return reply;
    };
}

storage::StorageCommand WeatherService::addComparisonCity(
    domain::WeatherCity city, domain::WeatherForecast forecast, QString updatedAt)
{
    return [city = std::move(city), forecast = std::move(forecast),
            updatedAt = std::move(updatedAt)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (updatedAt.isEmpty() || !validForecast(forecast)
            || domain::stableWeatherCityIdentity(city).isEmpty()) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }
        const auto saved = repositories.weatherComparison.add(
            city, QString::fromUtf8(providers::serializeWeatherForecast(forecast)), updatedAt);
        if (!saved.succeeded()) return failed(saved.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("saved"), true);
        return reply;
    };
}

storage::StorageCommand WeatherService::replaceComparisonCity(
    QString identity, domain::WeatherCity city,
    domain::WeatherForecast forecast, QString updatedAt)
{
    return [identity = std::move(identity), city = std::move(city),
            forecast = std::move(forecast), updatedAt = std::move(updatedAt)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (identity.isEmpty() || updatedAt.isEmpty() || !validForecast(forecast)
            || domain::stableWeatherCityIdentity(city).isEmpty()) {
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        }
        const auto replaced = repositories.weatherComparison.replace(
            identity, city, QString::fromUtf8(providers::serializeWeatherForecast(forecast)),
            updatedAt);
        if (!replaced.succeeded()) return failed(replaced.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("saved"), true);
        return reply;
    };
}

storage::StorageCommand WeatherService::removeComparisonCity(QString identity)
{
    return [identity = std::move(identity)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto removed = repositories.weatherComparison.remove(identity);
        if (!removed.succeeded()) return failed(removed.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("saved"), true);
        return reply;
    };
}

storage::StorageCommand WeatherService::reorderComparisonCities(QVector<QString> identities)
{
    return [identities = std::move(identities)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto reordered = repositories.weatherComparison.reorder(identities);
        if (!reordered.succeeded()) return failed(reordered.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("saved"), true);
        return reply;
    };
}

storage::StorageCommand WeatherService::saveComparisonForecast(
    QString identity, domain::WeatherForecast forecast, QString updatedAt)
{
    return [identity = std::move(identity), forecast = std::move(forecast),
            updatedAt = std::move(updatedAt)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (identity.isEmpty() || updatedAt.isEmpty() || !validForecast(forecast))
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto saved = repositories.weatherComparison.saveForecast(
            identity, QString::fromUtf8(providers::serializeWeatherForecast(forecast)), updatedAt);
        if (!saved.succeeded()) return failed(saved.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("saved"), true);
        return reply;
    };
}

} // namespace something::services
