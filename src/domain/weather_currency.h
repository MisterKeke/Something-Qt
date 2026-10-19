#pragma once

#include <QVector>
#include <QString>

#include <optional>

namespace something::domain {

struct WeatherCurrent final {
    std::optional<double> temperatureCelsius;
    std::optional<double> apparentTemperatureCelsius;
    std::optional<int> weatherCode;
    std::optional<double> windKmh;
};

struct WeatherDay final {
    QString date; // Provider-local calendar date; never convert through UTC.
    int weatherCode{0};
    double maximumCelsius{0.0};
    double minimumCelsius{0.0};
    double precipitationProbability{0.0};
};

struct WeatherForecast final {
    WeatherCurrent current;
    QVector<WeatherDay> days;
    QString timezone;
};

struct WeatherCity final {
    QString name;
    QString administrativeArea;
    QString country;
    double latitude{0.0};
    double longitude{0.0};
    bool found{false};
};

struct CurrencyRate final {
    QString baseCode;
    QString quoteCode;
    QString date;
    double rate{0.0};
    bool found{false};
};

struct CurrencyRates final {
    QString baseCode;
    QString date;
    QVector<CurrencyRate> rows; // Sorted by quote code before publication.
};

struct CurrencyPairCode final {
    QString baseCode;
    QString quoteCode;
};

QString normalizeCurrencyCode(QString value);
bool isValidCurrencyCode(const QString &value);
bool isValidCurrencyAmount(double amount) noexcept;
std::optional<CurrencyPairCode> parseCurrencyPair(QString value);
bool isValidWeatherCoordinates(double latitude, double longitude) noexcept;

} // namespace something::domain
