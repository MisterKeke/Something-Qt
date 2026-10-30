#pragma once

#include "../domain/app_error.h"
#include "../domain/steam_game.h"
#include "../domain/steam_news.h"
#include "../domain/steam_release.h"
#include "../domain/weather_currency.h"

#include <QByteArray>

namespace something::providers {

template<typename T>
struct FeatureParseResult final {
    T value{};
    something::domain::AppError error{};
    bool succeeded() const noexcept { return !error.isError() && !error.isCancelled(); }
};

FeatureParseResult<something::domain::WeatherForecast> parseWeatherForecast(
    const QByteArray &body);
FeatureParseResult<something::domain::WeatherCity> parseWeatherCitySearch(
    const QByteArray &body);
FeatureParseResult<QVector<something::domain::WeatherCity>> parseWeatherCitySearchResults(
    const QByteArray &body);
FeatureParseResult<something::domain::CurrencyRate> parseCurrencyRate(
    const QByteArray &body, const QString &expectedBase, const QString &expectedQuote);
FeatureParseResult<something::domain::CurrencyRates> parseCurrencyRates(
    const QByteArray &body, const QString &expectedBase);
FeatureParseResult<something::domain::SteamGameDetails> parseSteamGameDetails(
    const QByteArray &body, quint32 expectedAppId);
FeatureParseResult<something::domain::SteamReleaseDetails> parseSteamReleaseDetails(
    const QByteArray &body, quint32 expectedAppId);
FeatureParseResult<QVector<something::domain::SteamNewsArticle>> parseSteamNewsArticles(
    const QByteArray &body, quint32 expectedAppId);

QByteArray serializeWeatherForecast(const something::domain::WeatherForecast &forecast);

} // namespace something::providers
