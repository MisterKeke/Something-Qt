#include "feature_parsers.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace something::providers {
namespace {

using something::domain::CurrencyRate;
using something::domain::CurrencyRates;
using something::domain::ErrorCategory;
using something::domain::WeatherCity;
using something::domain::WeatherCurrent;
using something::domain::WeatherDay;
using something::domain::WeatherForecast;
using something::domain::SteamGameDetails;
using something::domain::SteamPriceState;

something::domain::AppError invalidResponse()
{
    return something::domain::AppError(ErrorCategory::InvalidResponse);
}

bool parseObject(const QByteArray &body, QJsonObject *object)
{
    if (!object || body.isEmpty() || body.size() > 2 * 1024 * 1024) return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    *object = document.object();
    return true;
}

bool requiredString(const QJsonObject &object, const QString &key, QString *value)
{
    const QJsonValue field = object.value(key);
    if (!field.isString()) return false;
    *value = field.toString();
    return true;
}

bool optionalString(const QJsonObject &object, const QString &key, QString *value)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined() || field.isNull()) {
        value->clear();
        return true;
    }
    if (!field.isString()) return false;
    *value = field.toString();
    return true;
}

bool requiredNumber(const QJsonObject &object, const QString &key, double *value)
{
    const QJsonValue field = object.value(key);
    if (!field.isDouble() || !std::isfinite(field.toDouble())) return false;
    *value = field.toDouble();
    return true;
}

bool optionalNumber(const QJsonObject &object, const QString &key,
                    std::optional<double> *value)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined() || field.isNull()) {
        value->reset();
        return true;
    }
    if (!field.isDouble() || !std::isfinite(field.toDouble())) return false;
    *value = field.toDouble();
    return true;
}

bool optionalInteger(const QJsonObject &object, const QString &key,
                     std::optional<int> *value)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined() || field.isNull()) {
        value->reset();
        return true;
    }
    if (!field.isDouble() || !std::isfinite(field.toDouble())
        || std::floor(field.toDouble()) != field.toDouble()
        || field.toDouble() < std::numeric_limits<int>::min()
        || field.toDouble() > std::numeric_limits<int>::max()) return false;
    *value = field.toInt();
    return true;
}

bool validDate(const QString &value)
{
    const QDate date = QDate::fromString(value, Qt::ISODate);
    return date.isValid() && date.toString(Qt::ISODate) == value;
}

bool validSolarTimestamp(const QString &value, const QString &expectedDate)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^([0-9]{4}-[0-9]{2}-[0-9]{2})T([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\.[0-9]{1,9})?)?(?:[Zz]|([+-])([0-9]{2}):([0-9]{2}))?$)"));
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch() || match.captured(1) != expectedDate
        || !validDate(match.captured(1))) return false;

    bool hourOk = false;
    bool minuteOk = false;
    const int hour = match.captured(2).toInt(&hourOk);
    const int minute = match.captured(3).toInt(&minuteOk);
    if (!hourOk || !minuteOk || hour > 23 || minute > 59) return false;
    if (!match.captured(4).isEmpty()) {
        bool secondOk = false;
        const int second = match.captured(4).toInt(&secondOk);
        if (!secondOk || second > 59) return false;
    }
    if (!match.captured(5).isEmpty()) {
        bool offsetHourOk = false;
        bool offsetMinuteOk = false;
        const int offsetHour = match.captured(6).toInt(&offsetHourOk);
        const int offsetMinute = match.captured(7).toInt(&offsetMinuteOk);
        if (!offsetHourOk || !offsetMinuteOk || offsetHour > 23 || offsetMinute > 59)
            return false;
    }
    return true;
}

template<typename T, typename Convert>
bool parseArray(const QJsonValue &value, QVector<T> *output, Convert convert)
{
    if (!value.isArray()) return false;
    const QJsonArray array = value.toArray();
    output->reserve(array.size());
    for (const QJsonValue &item : array) {
        T result{};
        if (!convert(item, &result)) return false;
        output->push_back(std::move(result));
    }
    return true;
}

template<typename T, typename Convert>
QVector<std::optional<T>> parseOptionalSolarArray(const QJsonValue &value,
                                                  qsizetype expectedCount,
                                                  Convert convert)
{
    QVector<std::optional<T>> output(expectedCount);
    if (!value.isArray()) return output;
    const QJsonArray array = value.toArray();
    const qsizetype count = std::min(array.size(), expectedCount);
    for (qsizetype i = 0; i < count; ++i) {
        const QJsonValue item = array.at(i);
        if (item.isNull() || item.isUndefined()) continue;
        T parsed{};
        if (convert(item, &parsed)) output[i] = std::move(parsed);
    }
    return output;
}

bool parseDailyNumber(const QJsonValue &value, double *out)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
    *out = value.toDouble();
    return true;
}

bool parseDailyCode(const QJsonValue &value, int *out)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())
        || std::floor(value.toDouble()) != value.toDouble()
        || value.toDouble() < std::numeric_limits<int>::min()
        || value.toDouble() > std::numeric_limits<int>::max()) return false;
    *out = value.toInt();
    return true;
}

bool parseRateObject(const QJsonObject &object, CurrencyRate *rate)
{
    QString date;
    QString base;
    QString quote;
    double value = 0.0;
    if (!optionalString(object, QStringLiteral("date"), &date)
        || !requiredString(object, QStringLiteral("base"), &base)
        || !requiredString(object, QStringLiteral("quote"), &quote)
        || !requiredNumber(object, QStringLiteral("rate"), &value)) return false;
    base = something::domain::normalizeCurrencyCode(base);
    quote = something::domain::normalizeCurrencyCode(quote);
    if (base.isEmpty() || quote.isEmpty()) return false;
    if (!date.isEmpty()) {
        const QDate parsedDate = QDate::fromString(date, Qt::ISODate);
        if (!parsedDate.isValid() || parsedDate.toString(Qt::ISODate) != date) return false;
    }
    rate->date = date;
    rate->baseCode = base;
    rate->quoteCode = quote;
    rate->rate = value;
    rate->found = true;
    return true;
}

} // namespace

FeatureParseResult<WeatherForecast> parseWeatherForecast(const QByteArray &body)
{
    FeatureParseResult<WeatherForecast> result;
    QJsonObject root;
    if (!parseObject(body, &root)) {
        result.error = invalidResponse();
        return result;
    }

    const QJsonValue currentValue = root.value(QStringLiteral("current"));
    const QJsonValue dailyValue = root.value(QStringLiteral("daily"));
    if (!currentValue.isObject() || !dailyValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject current = currentValue.toObject();
    const QJsonObject daily = dailyValue.toObject();
    if (!optionalNumber(current, QStringLiteral("temperature_2m"),
                        &result.value.current.temperatureCelsius)
        || !optionalNumber(current, QStringLiteral("apparent_temperature"),
                           &result.value.current.apparentTemperatureCelsius)
        || !optionalInteger(current, QStringLiteral("weather_code"),
                            &result.value.current.weatherCode)
        || !optionalNumber(current, QStringLiteral("wind_speed_10m"),
                           &result.value.current.windKmh)
        || !optionalString(root, QStringLiteral("timezone"), &result.value.timezone)) {
        result.error = invalidResponse();
        return result;
    }

    QVector<QString> dates;
    QVector<int> codes;
    QVector<double> highs;
    QVector<double> lows;
    QVector<double> precipitation;
    const bool parsed = parseArray<QString>(daily.value(QStringLiteral("time")), &dates,
        [](const QJsonValue &v, QString *out) {
            if (!v.isString() || !validDate(v.toString())) return false;
            *out = v.toString(); return true;
        })
        && parseArray<int>(daily.value(QStringLiteral("weather_code")), &codes, parseDailyCode)
        && parseArray<double>(daily.value(QStringLiteral("temperature_2m_max")), &highs, parseDailyNumber)
        && parseArray<double>(daily.value(QStringLiteral("temperature_2m_min")), &lows, parseDailyNumber)
        && parseArray<double>(daily.value(QStringLiteral("precipitation_probability_max")),
                              &precipitation, parseDailyNumber);
    const qsizetype count = dates.size();
    if (!parsed || count == 0 || codes.size() != count || highs.size() != count
        || lows.size() != count || precipitation.size() != count) {
        result.error = invalidResponse();
        return result;
    }

    const auto sunrises = parseOptionalSolarArray<QString>(
        daily.value(QStringLiteral("sunrise")), count,
        [](const QJsonValue &value, QString *out) {
            if (!value.isString()) return false;
            *out = value.toString();
            return true;
        });
    const auto sunsets = parseOptionalSolarArray<QString>(
        daily.value(QStringLiteral("sunset")), count,
        [](const QJsonValue &value, QString *out) {
            if (!value.isString()) return false;
            *out = value.toString();
            return true;
        });
    const auto daylightDurations = parseOptionalSolarArray<double>(
        daily.value(QStringLiteral("daylight_duration")), count,
        [](const QJsonValue &value, double *out) {
            if (!value.isDouble() || !std::isfinite(value.toDouble())
                || value.toDouble() < 0.0 || value.toDouble() > 86400.0) return false;
            *out = value.toDouble();
            return true;
        });
    result.value.days.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        WeatherDay day{dates.at(i), codes.at(i), highs.at(i), lows.at(i),
                       precipitation.at(i)};
        if (sunrises.at(i) && validSolarTimestamp(*sunrises.at(i), dates.at(i)))
            day.sunriseTimestamp = sunrises.at(i);
        if (sunsets.at(i) && validSolarTimestamp(*sunsets.at(i), dates.at(i)))
            day.sunsetTimestamp = sunsets.at(i);
        day.daylightDurationSeconds = daylightDurations.at(i);
        result.value.days.push_back(std::move(day));
    }
    return result;
}

FeatureParseResult<WeatherCity> parseWeatherCitySearch(const QByteArray &body)
{
    FeatureParseResult<WeatherCity> result;
    QJsonObject root;
    if (!parseObject(body, &root)) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonValue resultsValue = root.value(QStringLiteral("results"));
    if (resultsValue.isUndefined()) return result; // Reference treats a missing result list as no match.
    if (!resultsValue.isArray()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonArray results = resultsValue.toArray();
    if (results.isEmpty()) return result;
    if (!results.first().isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject first = results.first().toObject();
    WeatherCity city;
    if (!requiredString(first, QStringLiteral("name"), &city.name)
        || !requiredNumber(first, QStringLiteral("latitude"), &city.latitude)
        || !requiredNumber(first, QStringLiteral("longitude"), &city.longitude)
        || !optionalString(first, QStringLiteral("admin1"), &city.administrativeArea)
        || !optionalString(first, QStringLiteral("country"), &city.country)
        || !something::domain::isValidWeatherCoordinates(city.latitude, city.longitude)) {
        result.error = invalidResponse();
        return result;
    }
    result.value = std::move(city);
    result.value.found = true;
    return result;
}

FeatureParseResult<CurrencyRate> parseCurrencyRate(const QByteArray &body,
                                                   const QString &expectedBase,
                                                   const QString &expectedQuote)
{
    FeatureParseResult<CurrencyRate> result;
    const QString base = something::domain::normalizeCurrencyCode(expectedBase);
    const QString quote = something::domain::normalizeCurrencyCode(expectedQuote);
    if (base.isEmpty() || quote.isEmpty()) {
        result.error = invalidResponse();
        return result;
    }
    QJsonObject root;
    if (!parseObject(body, &root)) {
        result.error = invalidResponse();
        return result;
    }
    if (!parseRateObject(root, &result.value)
        || result.value.baseCode != base || result.value.quoteCode != quote) {
        result.error = invalidResponse();
    }
    return result;
}

FeatureParseResult<CurrencyRates> parseCurrencyRates(const QByteArray &body,
                                                     const QString &expectedBase)
{
    FeatureParseResult<CurrencyRates> result;
    const QString base = something::domain::normalizeCurrencyCode(expectedBase);
    if (base.isEmpty()) {
        result.error = invalidResponse();
        return result;
    }
    if (body.isEmpty() || body.size() > 2 * 1024 * 1024) {
        result.error = invalidResponse();
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        result.error = invalidResponse();
        return result;
    }
    result.value.baseCode = base;
    const QJsonArray array = document.array();
    QHash<QString, int> codeIndex;
    for (const QJsonValue &item : array) {
        if (!item.isObject()) {
            result.error = invalidResponse();
            return result;
        }
        CurrencyRate rate;
        if (!parseRateObject(item.toObject(), &rate)
            || rate.baseCode != base || rate.quoteCode.isEmpty()) {
            result.error = invalidResponse();
            return result;
        }
        rate.quoteCode = something::domain::normalizeCurrencyCode(rate.quoteCode);
        if (rate.quoteCode.isEmpty()) {
            result.error = invalidResponse();
            return result;
        }
        rate.baseCode = base;
        if (result.value.date.isEmpty()) result.value.date = rate.date;
        const auto previous = codeIndex.constFind(rate.quoteCode);
        if (previous == codeIndex.cend()) {
            codeIndex.insert(rate.quoteCode, result.value.rows.size());
            result.value.rows.push_back(std::move(rate));
        } else {
            result.value.rows[*previous] = std::move(rate);
        }
    }
    std::sort(result.value.rows.begin(), result.value.rows.end(),
              [](const CurrencyRate &a, const CurrencyRate &b) {
                  return a.quoteCode < b.quoteCode;
              });
    return result;
}

QByteArray serializeWeatherForecast(const WeatherForecast &forecast)
{
    QJsonObject current;
    if (forecast.current.temperatureCelsius)
        current.insert(QStringLiteral("temperature_2m"), *forecast.current.temperatureCelsius);
    if (forecast.current.apparentTemperatureCelsius)
        current.insert(QStringLiteral("apparent_temperature"), *forecast.current.apparentTemperatureCelsius);
    if (forecast.current.weatherCode)
        current.insert(QStringLiteral("weather_code"), *forecast.current.weatherCode);
    if (forecast.current.windKmh)
        current.insert(QStringLiteral("wind_speed_10m"), *forecast.current.windKmh);
    QJsonArray time, code, maximum, minimum, precipitation;
    QJsonArray sunrise, sunset, daylightDuration;
    bool hasSunrise = false;
    bool hasSunset = false;
    bool hasDaylightDuration = false;
    for (const WeatherDay &day : forecast.days) {
        time.push_back(day.date);
        code.push_back(day.weatherCode);
        maximum.push_back(day.maximumCelsius);
        minimum.push_back(day.minimumCelsius);
        precipitation.push_back(day.precipitationProbability);
        if (day.sunriseTimestamp) {
            sunrise.push_back(*day.sunriseTimestamp);
            hasSunrise = true;
        } else {
            sunrise.push_back(QJsonValue(QJsonValue::Null));
        }
        if (day.sunsetTimestamp) {
            sunset.push_back(*day.sunsetTimestamp);
            hasSunset = true;
        } else {
            sunset.push_back(QJsonValue(QJsonValue::Null));
        }
        if (day.daylightDurationSeconds) {
            daylightDuration.push_back(*day.daylightDurationSeconds);
            hasDaylightDuration = true;
        } else {
            daylightDuration.push_back(QJsonValue(QJsonValue::Null));
        }
    }
    QJsonObject daily{
        {QStringLiteral("time"), time},
        {QStringLiteral("weather_code"), code},
        {QStringLiteral("temperature_2m_max"), maximum},
        {QStringLiteral("temperature_2m_min"), minimum},
        {QStringLiteral("precipitation_probability_max"), precipitation},
    };
    if (hasSunrise) daily.insert(QStringLiteral("sunrise"), sunrise);
    if (hasSunset) daily.insert(QStringLiteral("sunset"), sunset);
    if (hasDaylightDuration) daily.insert(QStringLiteral("daylight_duration"), daylightDuration);
    const QJsonObject root{{QStringLiteral("current"), current},
                           {QStringLiteral("daily"), daily},
                           {QStringLiteral("timezone"), forecast.timezone}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

FeatureParseResult<SteamGameDetails> parseSteamGameDetails(const QByteArray &body,
                                                           quint32 expectedAppId)
{
    FeatureParseResult<SteamGameDetails> result;
    if (expectedAppId == 0 || body.isEmpty() || body.size() > 2 * 1024 * 1024) {
        result.error = invalidResponse();
        return result;
    }
    QJsonObject root;
    if (!parseObject(body, &root)) {
        result.error = invalidResponse();
        return result;
    }
    const QString appIdKey = QString::number(expectedAppId);
    const QJsonValue envelopeValue = root.value(appIdKey);
    if (!envelopeValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject envelope = envelopeValue.toObject();
    const QJsonValue success = envelope.value(QStringLiteral("success"));
    const QJsonValue dataValue = envelope.value(QStringLiteral("data"));
    if (!success.isBool() || !success.toBool() || !dataValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject data = dataValue.toObject();
    QString type;
    QString name;
    if (!requiredString(data, QStringLiteral("type"), &type)
        || type.trimmed().compare(QStringLiteral("game"), Qt::CaseInsensitive) != 0
        || !requiredString(data, QStringLiteral("name"), &name)) {
        result.error = invalidResponse();
        return result;
    }
    name = name.trimmed();
    if (name.isEmpty() || name.toUcs4().size() > 300) {
        result.error = invalidResponse();
        return result;
    }
    quint64 returnedId = 0;
    double returnedIdNumber = 0.0;
    if (!requiredNumber(data, QStringLiteral("steam_appid"), &returnedIdNumber)
        || std::floor(returnedIdNumber) != returnedIdNumber || returnedIdNumber <= 0.0
        || returnedIdNumber > std::numeric_limits<quint32>::max()) {
        result.error = invalidResponse();
        return result;
    }
    returnedId = static_cast<quint64>(returnedIdNumber);
    if (returnedId != expectedAppId) {
        result.error = invalidResponse();
        return result;
    }

    const QJsonValue isFreeValue = data.value(QStringLiteral("is_free"));
    if (!isFreeValue.isUndefined() && !isFreeValue.isBool()) {
        result.error = invalidResponse();
        return result;
    }
    QString headerImage;
    QString capsuleImage;
    if (!optionalString(data, QStringLiteral("header_image"), &headerImage)
        || !optionalString(data, QStringLiteral("capsule_image"), &capsuleImage)) {
        result.error = invalidResponse();
        return result;
    }

    SteamGameDetails details;
    details.appId = expectedAppId;
    details.name = name;
    details.artworkUrl = something::domain::normalizeSteamArtworkUrl(headerImage);
    if (details.artworkUrl.isEmpty())
        details.artworkUrl = something::domain::normalizeSteamArtworkUrl(capsuleImage);

    if (isFreeValue.toBool()) {
        details.priceState = SteamPriceState::Free;
        details.regularPriceMinor = 0;
        details.currentPriceMinor = 0;
        result.value = std::move(details);
        return result;
    }
    const QJsonValue priceValue = data.value(QStringLiteral("price_overview"));
    if (priceValue.isUndefined() || priceValue.isNull()) {
        details.priceState = SteamPriceState::Unavailable;
        result.value = std::move(details);
        return result;
    }
    if (!priceValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject price = priceValue.toObject();
    QString currency;
    double initialValue = 0.0;
    double finalValue = 0.0;
    std::optional<double> discountField;
    if (!requiredString(price, QStringLiteral("currency"), &currency)
        || !requiredNumber(price, QStringLiteral("initial"), &initialValue)
        || !requiredNumber(price, QStringLiteral("final"), &finalValue)
        || !optionalNumber(price, QStringLiteral("discount_percent"), &discountField)
        || std::floor(initialValue) != initialValue || std::floor(finalValue) != finalValue
        || (discountField && std::floor(*discountField) != *discountField)
        || initialValue < 0.0 || finalValue < 0.0
        || initialValue >= std::ldexp(1.0, 63)
        || finalValue >= std::ldexp(1.0, 63)
        || discountField.value_or(0.0) < 0.0 || discountField.value_or(0.0) > 100.0) {
        result.error = invalidResponse();
        return result;
    }
    currency = currency.trimmed().toUpper();
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    if (!currencyPattern.match(currency).hasMatch()) {
        result.error = invalidResponse();
        return result;
    }
    details.priceState = SteamPriceState::Priced;
    details.currency = currency;
    details.regularPriceMinor = static_cast<qint64>(initialValue);
    details.currentPriceMinor = static_cast<qint64>(finalValue);
    details.discountPercent = static_cast<int>(discountField.value_or(0.0));
    result.value = std::move(details);
    return result;
}

} // namespace something::providers
