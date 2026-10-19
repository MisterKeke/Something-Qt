#include "weather_currency.h"

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

} // namespace something::domain
