#include "feature_parsers.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>
#include <QTime>
#include <QTimeZone>

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
using something::domain::SteamReleaseDateInfo;
using something::domain::SteamReleaseDatePrecision;
using something::domain::SteamReleaseDetails;
using something::domain::SteamReleaseStatus;

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

struct ParsedSteamReleaseDate final {
    SteamReleaseDateInfo info;
    bool comingSoon{false};
};

QDate dateFromEnglishText(const QString &text, const QStringList &formats)
{
    const QLocale english(QLocale::English, QLocale::UnitedStates);
    for (const QString &format : formats) {
        const QDate date = english.toDate(text, format);
        if (date.isValid()) return date;
    }
    return {};
}

std::optional<ParsedSteamReleaseDate> parseSteamReleaseDateSegment(const QString &text)
{
    ParsedSteamReleaseDate parsed;
    parsed.info.announcedText = text;
    const QString normalized = text.trimmed();
    if (normalized.isEmpty()) return std::nullopt;

    static const QRegularExpression comingSoonPattern(
        QStringLiteral("^(?:coming\\s+soon|to\\s+be\\s+announced|tba)$"),
        QRegularExpression::CaseInsensitiveOption);
    if (comingSoonPattern.match(normalized).hasMatch()) {
        parsed.comingSoon = true;
        return parsed;
    }

    const QDate isoDate = QDate::fromString(normalized, Qt::ISODate);
    if (isoDate.isValid() && isoDate.toString(Qt::ISODate) == normalized) {
        parsed.info.precision = SteamReleaseDatePrecision::ExactDay;
        parsed.info.exactDate = isoDate;
        return parsed;
    }

    static const QStringList exactDateFormats{
        QStringLiteral("MMM d, yyyy"), QStringLiteral("MMMM d, yyyy"),
        QStringLiteral("d MMM, yyyy"), QStringLiteral("d MMMM, yyyy"),
        QStringLiteral("MMM d yyyy"), QStringLiteral("MMMM d yyyy"),
        QStringLiteral("d MMM yyyy"), QStringLiteral("d MMMM yyyy")};
    const QDate exactDate = dateFromEnglishText(normalized, exactDateFormats);
    if (exactDate.isValid()) {
        parsed.info.precision = SteamReleaseDatePrecision::ExactDay;
        parsed.info.exactDate = exactDate;
        return parsed;
    }

    static const QStringList monthFormats{
        QStringLiteral("MMMM yyyy"), QStringLiteral("MMM yyyy")};
    const QDate monthDate = dateFromEnglishText(normalized, monthFormats);
    if (monthDate.isValid()) {
        parsed.info.precision = SteamReleaseDatePrecision::Month;
        parsed.info.windowStart = QDate(monthDate.year(), monthDate.month(), 1);
        parsed.info.windowEnd = QDate(monthDate.year(), monthDate.month(),
                                      monthDate.daysInMonth());
        return parsed;
    }

    static const QRegularExpression quarterPattern(
        QStringLiteral("^(?:Q([1-4])\\s+(\\d{4})|(\\d{4})\\s+Q([1-4]))$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto quarterMatch = quarterPattern.match(normalized);
    if (quarterMatch.hasMatch()) {
        const int quarter = quarterMatch.captured(1).isEmpty()
            ? quarterMatch.captured(4).toInt() : quarterMatch.captured(1).toInt();
        const int year = quarterMatch.captured(2).isEmpty()
            ? quarterMatch.captured(3).toInt() : quarterMatch.captured(2).toInt();
        const QDate firstDay(year, (quarter - 1) * 3 + 1, 1);
        if (!firstDay.isValid()) return std::nullopt;
        parsed.info.precision = SteamReleaseDatePrecision::Quarter;
        parsed.info.windowStart = firstDay;
        const int lastMonth = (quarter - 1) * 3 + 3;
        parsed.info.windowEnd = QDate(year, lastMonth,
                                      QDate(year, lastMonth, 1).daysInMonth());
        return parsed;
    }

    static const QRegularExpression yearPattern(QStringLiteral("^(\\d{4})$"));
    const auto yearMatch = yearPattern.match(normalized);
    if (yearMatch.hasMatch()) {
        const int year = yearMatch.captured(1).toInt();
        const QDate firstDay(year, 1, 1);
        if (!firstDay.isValid()) return std::nullopt;
        parsed.info.precision = SteamReleaseDatePrecision::Year;
        parsed.info.windowStart = firstDay;
        parsed.info.windowEnd = QDate(year, 12, 31);
        return parsed;
    }

    static const QRegularExpression seasonPattern(
        QStringLiteral("^(?:spring|summer|fall|autumn|winter)\\s+\\d{4}$"),
        QRegularExpression::CaseInsensitiveOption);
    if (seasonPattern.match(normalized).hasMatch()) {
        parsed.info.precision = SteamReleaseDatePrecision::Season;
        return parsed;
    }

    return std::nullopt;
}

ParsedSteamReleaseDate parseSteamReleaseDateText(const QString &text)
{
    if (const auto segment = parseSteamReleaseDateSegment(text)) return *segment;

    // Keep a provider-supplied but unfamiliar window as text. It is explicitly
    // approximate and has no generated calendar boundaries.
    static const QRegularExpression rangeSeparator(
        QStringLiteral("\\s+(?:to|[-–—])\\s+"),
        QRegularExpression::CaseInsensitiveOption);
    const QStringList parts = text.split(rangeSeparator, Qt::SkipEmptyParts);
    if (parts.size() == 2) {
        const auto first = parseSteamReleaseDateSegment(parts.at(0).trimmed());
        const auto last = parseSteamReleaseDateSegment(parts.at(1).trimmed());
        const auto start = first
            ? (first->info.exactDate ? first->info.exactDate : first->info.windowStart)
            : std::optional<QDate>{};
        const auto end = last
            ? (last->info.exactDate ? last->info.exactDate : last->info.windowEnd)
            : std::optional<QDate>{};
        if (start && end && *start <= *end) {
            ParsedSteamReleaseDate parsed;
            parsed.info.announcedText = text;
            parsed.info.precision = SteamReleaseDatePrecision::Range;
            parsed.info.windowStart = start;
            parsed.info.windowEnd = end;
            return parsed;
        }
    }

    ParsedSteamReleaseDate parsed;
    parsed.info.announcedText = text;
    parsed.info.precision = SteamReleaseDatePrecision::Range;
    return parsed;
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

bool optionalInteger64(const QJsonObject &object, const QString &key, qint64 *value)
{
    if (!value) return false;
    const QJsonValue field = object.value(key);
    if (field.isUndefined() || field.isNull()) {
        *value = 0;
        return true;
    }
    constexpr double maximumExactJsonInteger = 9007199254740991.0;
    if (!field.isDouble() || !std::isfinite(field.toDouble())
        || std::floor(field.toDouble()) != field.toDouble()
        || field.toDouble() < 0.0
        || field.toDouble() > maximumExactJsonInteger) {
        return false;
    }
    *value = static_cast<qint64>(field.toDouble());
    return true;
}

bool validDate(const QString &value)
{
    const QDate date = QDate::fromString(value, Qt::ISODate);
    return date.isValid() && date.toString(Qt::ISODate) == value;
}

bool parseUnixTimestamp(const QJsonValue &value, QDateTime *output)
{
    if (!output || !value.isDouble() || !std::isfinite(value.toDouble())) return false;
    const double seconds = value.toDouble();
    constexpr double minimumSeconds = -62135596800.0;
    constexpr double maximumSeconds = 253402300799.0;
    if (seconds < minimumSeconds || seconds > maximumSeconds) return false;
    const qint64 milliseconds = static_cast<qint64>(std::llround(seconds * 1000.0));
    *output = QDateTime::fromMSecsSinceEpoch(milliseconds, Qt::UTC);
    return output->isValid();
}

bool parseProviderDate(const QJsonValue &value, const QTimeZone &timeZone, QString *output)
{
    if (!output) return false;
    if (value.isString()) {
        if (!validDate(value.toString())) return false;
        *output = value.toString();
        return true;
    }

    QDateTime timestamp;
    if (!timeZone.isValid() || !parseUnixTimestamp(value, &timestamp)) return false;
    *output = timestamp.toTimeZone(timeZone).date().toString(Qt::ISODate);
    return !output->isEmpty();
}

bool parseSolarTimestamp(const QJsonValue &value, const QTimeZone &timeZone,
                         QDateTime *output)
{
    if (!output) return false;
    if (value.isDouble()) return parseUnixTimestamp(value, output);
    if (!value.isString() || !timeZone.isValid()) return false;

    // Older cached Open-Meteo responses contain local ISO wall times. Their
    // fixed request-time offset can be stale across DST, so use the wall clock
    // components with the saved IANA timezone instead of applying that offset.
    static const QRegularExpression pattern(QStringLiteral(
        R"(^([0-9]{4}-[0-9]{2}-[0-9]{2})T([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\.[0-9]{1,9})?)?(?:[Zz]|([+-])([0-9]{2}):([0-9]{2}))?$)"));
    const QRegularExpressionMatch match = pattern.match(value.toString());
    if (!match.hasMatch() || !validDate(match.captured(1))) return false;

    bool hourOk = false;
    bool minuteOk = false;
    const int hour = match.captured(2).toInt(&hourOk);
    const int minute = match.captured(3).toInt(&minuteOk);
    if (!hourOk || !minuteOk || hour > 23 || minute > 59) return false;
    int second = 0;
    if (!match.captured(4).isEmpty()) {
        bool secondOk = false;
        second = match.captured(4).toInt(&secondOk);
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

    const QDate date = QDate::fromString(match.captured(1), Qt::ISODate);
    const QTime time(hour, minute, second);
    const QDateTime localTime(date, time, timeZone);
    if (!localTime.isValid()) return false;
    *output = localTime.toUTC();
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
    const QTimeZone locationTimeZone(result.value.timezone.toUtf8());

    QVector<QString> dates;
    QVector<int> codes;
    QVector<double> highs;
    QVector<double> lows;
    QVector<double> precipitation;
    const bool parsed = parseArray<QString>(daily.value(QStringLiteral("time")), &dates,
        [&locationTimeZone](const QJsonValue &v, QString *out) {
            return parseProviderDate(v, locationTimeZone, out);
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

    const auto sunrises = parseOptionalSolarArray<QDateTime>(
        daily.value(QStringLiteral("sunrise")), count,
        [&locationTimeZone](const QJsonValue &value, QDateTime *out) {
            return parseSolarTimestamp(value, locationTimeZone, out);
        });
    const auto sunsets = parseOptionalSolarArray<QDateTime>(
        daily.value(QStringLiteral("sunset")), count,
        [&locationTimeZone](const QJsonValue &value, QDateTime *out) {
            return parseSolarTimestamp(value, locationTimeZone, out);
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
        const QDate localDate = QDate::fromString(dates.at(i), Qt::ISODate);
        if (sunrises.at(i) && locationTimeZone.isValid()
            && sunrises.at(i)->toTimeZone(locationTimeZone).date() == localDate)
            day.sunriseUtc = sunrises.at(i)->toUTC();
        if (sunsets.at(i) && locationTimeZone.isValid()
            && sunsets.at(i)->toTimeZone(locationTimeZone).date() == localDate)
            day.sunsetUtc = sunsets.at(i)->toUTC();
        day.daylightDurationSeconds = daylightDurations.at(i);
        result.value.days.push_back(std::move(day));
    }
    return result;
}

FeatureParseResult<WeatherCity> parseWeatherCitySearch(const QByteArray &body)
{
    FeatureParseResult<WeatherCity> result;
    const auto candidates = parseWeatherCitySearchResults(body);
    if (!candidates.succeeded()) {
        result.error = candidates.error;
        return result;
    }
    if (!candidates.value.isEmpty()) result.value = candidates.value.first();
    return result;
}

FeatureParseResult<QVector<WeatherCity>> parseWeatherCitySearchResults(
    const QByteArray &body)
{
    FeatureParseResult<QVector<WeatherCity>> result;
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
    if (results.size() > 10) {
        result.error = invalidResponse();
        return result;
    }
    result.value.reserve(results.size());
    for (const QJsonValue &value : results) {
        if (!value.isObject()) {
            result.error = invalidResponse();
            result.value.clear();
            return result;
        }
        const QJsonObject object = value.toObject();
        WeatherCity city;
        QString secondaryAdministrativeArea;
        if (!optionalInteger64(object, QStringLiteral("id"), &city.geocodingId)
            || !requiredString(object, QStringLiteral("name"), &city.name)
            || !requiredNumber(object, QStringLiteral("latitude"), &city.latitude)
            || !requiredNumber(object, QStringLiteral("longitude"), &city.longitude)
            || !optionalString(object, QStringLiteral("admin1"), &city.administrativeArea)
            || !optionalString(object, QStringLiteral("admin2"), &secondaryAdministrativeArea)
            || !optionalString(object, QStringLiteral("country"), &city.country)
            || !optionalString(object, QStringLiteral("timezone"), &city.timezone)
            || city.name.trimmed().isEmpty()
            || !something::domain::isValidWeatherCoordinates(city.latitude, city.longitude)) {
            result.error = invalidResponse();
            result.value.clear();
            return result;
        }
        city.name = city.name.trimmed();
        city.administrativeArea = city.administrativeArea.trimmed();
        secondaryAdministrativeArea = secondaryAdministrativeArea.trimmed();
        if (city.administrativeArea.isEmpty())
            city.administrativeArea = std::move(secondaryAdministrativeArea);
        city.country = city.country.trimmed();
        city.timezone = city.timezone.trimmed();
        city.found = true;
        const bool duplicate = std::any_of(result.value.cbegin(), result.value.cend(),
            [&city](const WeatherCity &existing) {
                return something::domain::isSameWeatherLocation(existing, city);
            });
        if (!duplicate) result.value.push_back(std::move(city));
    }
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
        if (day.sunriseUtc && day.sunriseUtc->isValid()) {
            sunrise.push_back(static_cast<double>(day.sunriseUtc->toMSecsSinceEpoch()) / 1000.0);
            hasSunrise = true;
        } else {
            sunrise.push_back(QJsonValue(QJsonValue::Null));
        }
        if (day.sunsetUtc && day.sunsetUtc->isValid()) {
            sunset.push_back(static_cast<double>(day.sunsetUtc->toMSecsSinceEpoch()) / 1000.0);
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

FeatureParseResult<something::domain::SteamGameCatalogDetails> parseSteamGameCatalogDetails(
    const QByteArray &body, quint32 expectedAppId)
{
    FeatureParseResult<something::domain::SteamGameCatalogDetails> result;
    if (expectedAppId == 0 || body.isEmpty() || body.size() > 2 * 1024 * 1024) {
        result.error = invalidResponse();
        return result;
    }

    QJsonObject root;
    if (!parseObject(body, &root)) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonValue envelopeValue = root.value(QString::number(expectedAppId));
    if (!envelopeValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject envelope = envelopeValue.toObject();
    const QJsonValue success = envelope.value(QStringLiteral("success"));
    if (!success.isBool()) {
        result.error = invalidResponse();
        return result;
    }
    if (!success.toBool()) {
        result.error = something::domain::AppError(ErrorCategory::NotFound);
        return result;
    }

    const QJsonValue dataValue = envelope.value(QStringLiteral("data"));
    if (!dataValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject data = dataValue.toObject();
    QString type;
    QString name;
    double returnedAppId = 0.0;
    if (!requiredString(data, QStringLiteral("type"), &type)
        || !requiredString(data, QStringLiteral("name"), &name)
        || !requiredNumber(data, QStringLiteral("steam_appid"), &returnedAppId)
        || std::floor(returnedAppId) != returnedAppId || returnedAppId <= 0.0
        || returnedAppId > std::numeric_limits<quint32>::max()) {
        result.error = invalidResponse();
        return result;
    }
    if (type.trimmed().compare(QStringLiteral("game"), Qt::CaseInsensitive) != 0) {
        result.error = something::domain::AppError(ErrorCategory::CapabilityUnavailable);
        return result;
    }
    name = name.trimmed().normalized(QString::NormalizationForm_C);
    if (static_cast<quint32>(returnedAppId) != expectedAppId
        || name.isEmpty() || name.toUcs4().size() > 300) {
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

    something::domain::SteamGameCatalogDetails details;
    details.appId = expectedAppId;
    details.canonicalStoreUrl = QStringLiteral("https://store.steampowered.com/app/%1/")
                                    .arg(expectedAppId);
    details.name = std::move(name);
    details.artworkUrl = something::domain::normalizeSteamArtworkUrl(headerImage);
    if (details.artworkUrl.isEmpty())
        details.artworkUrl = something::domain::normalizeSteamArtworkUrl(capsuleImage);
    if (!something::domain::isValidSteamGameCatalogDetails(details)) {
        result.error = invalidResponse();
        return result;
    }
    result.value = std::move(details);
    return result;
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
        details.priceState = isFreeValue.isBool()
            ? SteamPriceState::Unavailable : SteamPriceState::Unsupported;
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

FeatureParseResult<SteamReleaseDetails> parseSteamReleaseDetails(const QByteArray &body,
                                                                 quint32 expectedAppId)
{
    FeatureParseResult<SteamReleaseDetails> result;
    const auto catalog = parseSteamGameCatalogDetails(body, expectedAppId);
    if (!catalog.succeeded()) {
        result.error = catalog.error;
        return result;
    }
    if (expectedAppId == 0 || body.isEmpty() || body.size() > 2 * 1024 * 1024) {
        result.error = invalidResponse();
        return result;
    }

    QJsonObject root;
    if (!parseObject(body, &root)) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonValue envelopeValue = root.value(QString::number(expectedAppId));
    if (!envelopeValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject envelope = envelopeValue.toObject();
    const QJsonValue success = envelope.value(QStringLiteral("success"));
    if (!success.isBool()) {
        result.error = invalidResponse();
        return result;
    }
    if (!success.toBool()) {
        result.error = something::domain::AppError(ErrorCategory::NotFound);
        return result;
    }

    const QJsonValue dataValue = envelope.value(QStringLiteral("data"));
    if (!dataValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject data = dataValue.toObject();

    SteamReleaseDetails details;
    details.appId = expectedAppId;
    details.title = catalog.value.name;
    details.artworkUrl = catalog.value.artworkUrl;

    const QJsonValue releaseDateValue = data.value(QStringLiteral("release_date"));
    if (releaseDateValue.isUndefined() || releaseDateValue.isNull()) {
        details.status = SteamReleaseStatus::InformationUnavailable;
        result.value = std::move(details);
        return result;
    }
    if (!releaseDateValue.isObject()) {
        result.error = invalidResponse();
        return result;
    }

    const QJsonObject releaseDate = releaseDateValue.toObject();
    std::optional<bool> comingSoon;
    const QJsonValue comingSoonValue = releaseDate.value(QStringLiteral("coming_soon"));
    if (!comingSoonValue.isUndefined() && !comingSoonValue.isNull()) {
        if (!comingSoonValue.isBool()) {
            result.error = invalidResponse();
            return result;
        }
        comingSoon = comingSoonValue.toBool();
    }

    QString announcedText;
    if (!optionalString(releaseDate, QStringLiteral("date"), &announcedText)
        || announcedText.size() > 200) {
        result.error = invalidResponse();
        return result;
    }
    announcedText = announcedText.trimmed();
    if (!announcedText.isEmpty()) {
        const ParsedSteamReleaseDate parsed = parseSteamReleaseDateText(announcedText);
        details.releaseDate = parsed.info;
        if (parsed.comingSoon) {
            details.status = SteamReleaseStatus::ComingSoon;
        } else if (comingSoon && !*comingSoon) {
            details.status = SteamReleaseStatus::Released;
        } else if (details.releaseDate.hasExactDate()) {
            details.status = SteamReleaseStatus::ExactDateAnnounced;
        } else {
            details.status = SteamReleaseStatus::ApproximateDateAnnounced;
        }
    } else if (comingSoon) {
        details.status = *comingSoon ? SteamReleaseStatus::ComingSoon
                                     : SteamReleaseStatus::Released;
    } else {
        details.status = SteamReleaseStatus::InformationUnavailable;
    }

    result.value = std::move(details);
    return result;
}

} // namespace something::providers
