#include "weather_currency.h"

#include <algorithm>
#include <cmath>

namespace something::domain {

QString normalizeCurrencyCode(QString value)
{
    value = value.trimmed().toUpper();
    if (value.size() != 3) return {};
    for (const QChar ch : value) {
        if (ch < QLatin1Char('A') || ch > QLatin1Char('Z')) return {};
    }
    return value;
}

bool isValidCurrencyCode(const QString &value)
{
    return !normalizeCurrencyCode(value).isEmpty();
}

bool isValidCurrencyAmount(double amount) noexcept
{
    return std::isfinite(amount) && amount > 0.0;
}

std::optional<CurrencyPairCode> parseCurrencyPair(QString value)
{
    value = value.trimmed().toUpper();
    const qsizetype separator = value.indexOf(QLatin1Char(':'));
    if (separator <= 0 || separator != value.lastIndexOf(QLatin1Char(':'))) return std::nullopt;
    const QString base = normalizeCurrencyCode(value.left(separator));
    const QString quote = normalizeCurrencyCode(value.mid(separator + 1));
    if (base.isEmpty() || quote.isEmpty()) return std::nullopt;
    return CurrencyPairCode{base, quote};
}

bool isValidWeatherCoordinates(double latitude, double longitude) noexcept
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
}

QString stableWeatherCityIdentity(const WeatherCity &city)
{
    if (city.geocodingId > 0)
        return QStringLiteral("open-meteo:%1").arg(city.geocodingId);
    if (!isValidWeatherCoordinates(city.latitude, city.longitude)) return {};
    constexpr double coordinateScale = 100000.0;
    return QStringLiteral("coordinates:%1:%2")
        .arg(static_cast<qint64>(std::llround(city.latitude * coordinateScale)))
        .arg(static_cast<qint64>(std::llround(city.longitude * coordinateScale)));
}

bool isSameWeatherLocation(const WeatherCity &left, const WeatherCity &right) noexcept
{
    if (left.geocodingId > 0 && right.geocodingId > 0
        && left.geocodingId == right.geocodingId) return true;
    if (!isValidWeatherCoordinates(left.latitude, left.longitude)
        || !isValidWeatherCoordinates(right.latitude, right.longitude)) return false;

    constexpr double radiansPerDegree = 3.14159265358979323846 / 180.0;
    constexpr double earthRadiusMeters = 6371000.0;
    const double latitude1 = left.latitude * radiansPerDegree;
    const double latitude2 = right.latitude * radiansPerDegree;
    const double latitudeDelta = (right.latitude - left.latitude) * radiansPerDegree;
    const double longitudeDelta = std::remainder(right.longitude - left.longitude, 360.0)
        * radiansPerDegree;
    const double latitudeSin = std::sin(latitudeDelta / 2.0);
    const double longitudeSin = std::sin(longitudeDelta / 2.0);
    const double haversine = latitudeSin * latitudeSin
        + std::cos(latitude1) * std::cos(latitude2) * longitudeSin * longitudeSin;
    const double distance = 2.0 * earthRadiusMeters
        * std::asin(std::sqrt(std::clamp(haversine, 0.0, 1.0)));
    return distance <= 100.0;
}

} // namespace something::domain
