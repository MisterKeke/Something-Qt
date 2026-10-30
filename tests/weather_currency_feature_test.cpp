#include "domain/weather_currency.h"
#include "models/currency_rates_model.h"
#include "models/weather_forecast_model.h"
#include "providers/feature_parsers.h"
#include "providers/provider_policy.h"
#include "services/currency_service.h"
#include "services/weather_service.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QFile>
#include <QDateTime>
#include <QLocale>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QTimeZone>
#include <QUuid>
#include <QtTest>

#include <limits>

using namespace something;

namespace {

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(":/provider-fixtures/") + name);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool openDatabase(QSqlDatabase *database, QString *connectionName)
{
    *connectionName = QStringLiteral("weather-currency-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    *database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    database->setDatabaseName(QStringLiteral(":memory:"));
    return database->open() && storage::SchemaMigrator::apply(*database);
}

domain::WeatherForecast sampleForecast()
{
    return providers::parseWeatherForecast(fixture(QStringLiteral("weather-forecast-success.json"))).value;
}

QByteArray weatherForecastJson(const QString &timezone, const QJsonArray &dates,
                               const QJsonValue &sunrise = QJsonValue(QJsonValue::Undefined),
                               const QJsonValue &sunset = QJsonValue(QJsonValue::Undefined),
                               const QJsonValue &daylightDuration = QJsonValue(QJsonValue::Undefined))
{
    QJsonArray codes;
    QJsonArray highs;
    QJsonArray lows;
    QJsonArray precipitation;
    for (qsizetype i = 0; i < dates.size(); ++i) {
        codes.push_back(0);
        highs.push_back(20.0);
        lows.push_back(10.0);
        precipitation.push_back(0.0);
    }

    QJsonObject daily{{QStringLiteral("time"), dates},
                      {QStringLiteral("weather_code"), codes},
                      {QStringLiteral("temperature_2m_max"), highs},
                      {QStringLiteral("temperature_2m_min"), lows},
                      {QStringLiteral("precipitation_probability_max"), precipitation}};
    if (!sunrise.isUndefined()) daily.insert(QStringLiteral("sunrise"), sunrise);
    if (!sunset.isUndefined()) daily.insert(QStringLiteral("sunset"), sunset);
    if (!daylightDuration.isUndefined())
        daily.insert(QStringLiteral("daylight_duration"), daylightDuration);

    return QJsonDocument(QJsonObject{
        {QStringLiteral("current"), QJsonObject{}},
        {QStringLiteral("daily"), daily},
        {QStringLiteral("timezone"), timezone},
    }).toJson(QJsonDocument::Compact);
}

double unixSeconds(const QDate &date, const QTime &time, const QTimeZone &timezone)
{
    return static_cast<double>(QDateTime(date, time, timezone).toUTC().toSecsSinceEpoch());
}

} // namespace

class WeatherCurrencyFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void currencyRulesRequireAsciiCodesAndFinitePositiveAmounts();
    void weatherProviderFixturesKeepDateOnlyValuesAndRejectIncompleteDailyData();
    void weatherSolarEventsRespectLocationTimezoneDstAndUtcDateBoundaries();
    void weatherSolarFieldsRemainOptionalForPolarAndInvalidData();
    void weatherForecastModelFormatsSolarTimesInItsLocationTimezone();
    void weatherSolarCacheRoundTripsAndAcceptsOlderForecasts();
    void cityLookupFixtureDistinguishesNoMatchFromCoordinates();
    void currencyProviderFixturesPreserveMissingAndSortAllRates();
    void providerPoliciesMatchTheReferenceEndpoints();
    void weatherAndCurrencyModelsPublishTypedRows();
    void serviceCommandsPersistOnlyLocationsAndFavoriteDefinitions();
};

void WeatherCurrencyFeatureTest::currencyRulesRequireAsciiCodesAndFinitePositiveAmounts()
{
    QCOMPARE(domain::normalizeCurrencyCode(QStringLiteral(" usd ")), QStringLiteral("USD"));
    QVERIFY(domain::normalizeCurrencyCode(QStringLiteral("U$D")).isEmpty());
    QVERIFY(domain::normalizeCurrencyCode(QString::fromUtf8("ßUR")).isEmpty());
    QVERIFY(domain::isValidCurrencyAmount(0.01));
    QVERIFY(!domain::isValidCurrencyAmount(0.0));
    QVERIFY(!domain::isValidCurrencyAmount(-1.0));
    QVERIFY(!domain::isValidCurrencyAmount(std::numeric_limits<double>::infinity()));
    const auto pair = domain::parseCurrencyPair(QStringLiteral(" usd : eur "));
    QVERIFY(pair.has_value());
    QCOMPARE(pair->baseCode, QStringLiteral("USD"));
    QCOMPARE(pair->quoteCode, QStringLiteral("EUR"));
    const auto identical = domain::parseCurrencyPair(QStringLiteral("USD:USD"));
    QVERIFY(identical.has_value()); // Pair parsing is shape-only; favorite creation enforces distinct codes.
}

void WeatherCurrencyFeatureTest::weatherProviderFixturesKeepDateOnlyValuesAndRejectIncompleteDailyData()
{
    const auto parsed = providers::parseWeatherForecast(
        fixture(QStringLiteral("weather-forecast-success.json")));
    QVERIFY(parsed.succeeded());
    QCOMPARE(parsed.value.days.size(), 6);
    QCOMPARE(parsed.value.days.first().date, QStringLiteral("2026-10-19"));
    QCOMPARE(parsed.value.timezone, QStringLiteral("Europe/Istanbul"));
    QCOMPARE(*parsed.value.current.temperatureCelsius, 21.4);

    const auto missingCurrent = providers::parseWeatherForecast(
        fixture(QStringLiteral("weather-current-missing.json")));
    QVERIFY(missingCurrent.succeeded());
    QVERIFY(!missingCurrent.value.current.temperatureCelsius.has_value());
    QVERIFY(!missingCurrent.value.current.windKmh.has_value());
    QVERIFY(providers::parseWeatherForecast(
        fixture(QStringLiteral("weather-daily-malformed.json"))).error.isError());
    QVERIFY(providers::parseWeatherForecast(QByteArrayLiteral("{" )).error.isError());
}

void WeatherCurrencyFeatureTest::weatherSolarEventsRespectLocationTimezoneDstAndUtcDateBoundaries()
{
    const QTimeZone berlin(QByteArrayLiteral("Europe/Berlin"));
    QVERIFY(berlin.isValid());
    const QDate beforeDst(2026, 3, 28);
    const QDate dstStart(2026, 3, 29);
    const auto berlinForecast = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("Europe/Berlin"),
        QJsonArray{beforeDst.toString(Qt::ISODate), dstStart.toString(Qt::ISODate)},
        QJsonArray{unixSeconds(beforeDst, QTime(6, 30), berlin),
                   unixSeconds(dstStart, QTime(6, 30), berlin)},
        QJsonArray{unixSeconds(beforeDst, QTime(19, 30), berlin),
                   unixSeconds(dstStart, QTime(19, 30), berlin)},
        QJsonArray{46800, 46800}));
    QVERIFY(berlinForecast.succeeded());
    QCOMPARE(berlinForecast.value.timezone, QStringLiteral("Europe/Berlin"));
    QCOMPARE(berlinForecast.value.days.size(), 2);
    for (qsizetype i = 0; i < berlinForecast.value.days.size(); ++i) {
        const auto &day = berlinForecast.value.days.at(i);
        const QDate date = i == 0 ? beforeDst : dstStart;
        QVERIFY(day.sunriseUtc.has_value());
        QVERIFY(day.sunsetUtc.has_value());
        QCOMPARE(day.sunriseUtc->toTimeZone(berlin).date(), date);
        QCOMPARE(day.sunriseUtc->toTimeZone(berlin).time(), QTime(6, 30));
        QCOMPARE(day.sunsetUtc->toTimeZone(berlin).date(), date);
        QCOMPARE(day.sunsetUtc->toTimeZone(berlin).time(), QTime(19, 30));
    }
    QCOMPARE(berlinForecast.value.days.at(0).sunriseUtc->secsTo(
                 *berlinForecast.value.days.at(1).sunriseUtc), qint64(23 * 60 * 60));

    // Older cache records used local wall-time strings and could retain an old offset.
    const auto olderStringCache = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("Europe/Berlin"), QJsonArray{dstStart.toString(Qt::ISODate)},
        QJsonArray{QStringLiteral("2026-03-29T06:30:00+01:00")},
        QJsonArray{QStringLiteral("2026-03-29T19:30:00+01:00")}));
    QVERIFY(olderStringCache.succeeded());
    QCOMPARE(olderStringCache.value.days.first().sunriseUtc->toTimeZone(berlin).time(),
             QTime(6, 30));
    QCOMPARE(olderStringCache.value.days.first().sunriseUtc->toTimeZone(berlin).offsetFromUtc(),
             2 * 60 * 60);

    // Kiritimati is UTC+14: this provider-day starts on the prior UTC date.
    const QTimeZone kiritimati(QByteArrayLiteral("Pacific/Kiritimati"));
    QVERIFY(kiritimati.isValid());
    const QDate kiritimatiDate(2026, 1, 2);
    const double providerMidnightUtc = unixSeconds(
        QDate(2026, 1, 1), QTime(10, 0), QTimeZone::utc());
    const auto boundaryForecast = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("Pacific/Kiritimati"), QJsonArray{providerMidnightUtc},
        QJsonArray{unixSeconds(kiritimatiDate, QTime(6, 5), kiritimati)},
        QJsonArray{unixSeconds(kiritimatiDate, QTime(18, 10), kiritimati)},
        QJsonArray{43500}));
    QVERIFY(boundaryForecast.succeeded());
    QCOMPARE(boundaryForecast.value.days.first().date, kiritimatiDate.toString(Qt::ISODate));
    QCOMPARE(boundaryForecast.value.days.first().sunriseUtc->toTimeZone(kiritimati).date(),
             kiritimatiDate);
    QCOMPARE(boundaryForecast.value.days.first().sunsetUtc->toTimeZone(kiritimati).date(),
             kiritimatiDate);
    QCOMPARE(boundaryForecast.value.days.first().sunriseUtc->toTimeZone(kiritimati).time(),
             QTime(6, 5));
}

void WeatherCurrencyFeatureTest::weatherSolarFieldsRemainOptionalForPolarAndInvalidData()
{
    const QJsonArray dates{QStringLiteral("2026-06-20"), QStringLiteral("2026-12-20")};
    const auto polar = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("Europe/Oslo"), dates,
        QJsonArray{QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null)},
        QJsonArray{QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null)},
        QJsonArray{86400, 0}));
    QVERIFY(polar.succeeded());
    QVERIFY(!polar.value.days.at(0).sunriseUtc.has_value());
    QVERIFY(!polar.value.days.at(0).sunsetUtc.has_value());
    QCOMPARE(*polar.value.days.at(0).daylightDurationSeconds, 86400.0);
    QVERIFY(!polar.value.days.at(1).sunriseUtc.has_value());
    QVERIFY(!polar.value.days.at(1).sunsetUtc.has_value());
    QCOMPARE(*polar.value.days.at(1).daylightDurationSeconds, 0.0);

    const QTimeZone utc = QTimeZone::utc();
    const auto malformedSolar = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("UTC"), dates,
        QJsonArray{QStringLiteral("invalid timestamp"),
                   unixSeconds(QDate(2026, 6, 21), QTime(6, 0), utc)},
        QJsonArray{unixSeconds(QDate(2026, 6, 21), QTime(18, 0), utc)},
        QJsonArray{-1, 86401}));
    QVERIFY(malformedSolar.succeeded());
    for (const auto &day : malformedSolar.value.days) {
        QVERIFY(!day.sunriseUtc.has_value());
        QVERIFY(!day.sunsetUtc.has_value());
        QVERIFY(!day.daylightDurationSeconds.has_value());
    }

    // Solar array lengths and values are optional; required daily forecast arrays remain strict.
    const auto missingSolar = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("UTC"), dates));
    QVERIFY(missingSolar.succeeded());
    QVERIFY(!missingSolar.value.days.first().sunriseUtc.has_value());
    QVERIFY(!missingSolar.value.days.first().sunsetUtc.has_value());
    QVERIFY(!missingSolar.value.days.first().daylightDurationSeconds.has_value());
    QVERIFY(providers::parseWeatherForecast(fixture(
        QStringLiteral("weather-daily-malformed.json"))).error.isError());
}

void WeatherCurrencyFeatureTest::weatherForecastModelFormatsSolarTimesInItsLocationTimezone()
{
    const QTimeZone berlin(QByteArrayLiteral("Europe/Berlin"));
    const QTimeZone tokyo(QByteArrayLiteral("Asia/Tokyo"));
    QVERIFY(berlin.isValid());
    QVERIFY(tokyo.isValid());
    const QDate date(2026, 10, 25);
    const QDate followingDate = date.addDays(1);
    const QJsonArray dates{date.toString(Qt::ISODate), followingDate.toString(Qt::ISODate)};
    const auto parsed = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("Europe/Berlin"), dates,
        QJsonArray{unixSeconds(date, QTime(6, 42), berlin),
                   unixSeconds(followingDate, QTime(7, 1), berlin)},
        QJsonArray{unixSeconds(date, QTime(17, 11), berlin),
                   unixSeconds(followingDate, QTime(16, 58), berlin)},
        QJsonArray{37740, 35820}));
    QVERIFY(parsed.succeeded());
    const auto searchedCityForecast = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("Asia/Tokyo"), dates,
        QJsonArray{unixSeconds(date, QTime(6, 42), tokyo),
                   unixSeconds(followingDate, QTime(7, 1), tokyo)},
        QJsonArray{unixSeconds(date, QTime(17, 11), tokyo),
                   unixSeconds(followingDate, QTime(16, 58), tokyo)},
        QJsonArray{37740, 35820}));
    QVERIFY(searchedCityForecast.succeeded());

    models::WeatherForecastModel primary;
    models::WeatherForecastModel searchedCity;
    primary.replace(parsed.value.days, parsed.value.timezone);
    searchedCity.replace(searchedCityForecast.value.days, searchedCityForecast.value.timezone);
    const QModelIndex row = primary.index(0);
    QCOMPARE(primary.data(row, models::WeatherForecastModel::SunriseRole).toString(),
             QLocale().toString(parsed.value.days.first().sunriseUtc->toTimeZone(berlin).time(),
                                QLocale::ShortFormat));
    QCOMPARE(primary.data(row, models::WeatherForecastModel::SunsetRole).toString(),
             QLocale().toString(parsed.value.days.first().sunsetUtc->toTimeZone(berlin).time(),
                                QLocale::ShortFormat));
    QCOMPARE(searchedCity.data(searchedCity.index(0),
                               models::WeatherForecastModel::SunriseRole).toString(),
             QLocale().toString(searchedCityForecast.value.days.first().sunriseUtc
                                    ->toTimeZone(tokyo).time(), QLocale::ShortFormat));
    QVERIFY(primary.data(row, models::WeatherForecastModel::HasSunriseRole).toBool());
    QVERIFY(primary.data(row, models::WeatherForecastModel::HasSunsetRole).toBool());
    QVERIFY(primary.data(row, models::WeatherForecastModel::HasDaylightDurationRole).toBool());
    const QModelIndex followingRow = primary.index(1);
    QCOMPARE(primary.data(followingRow, models::WeatherForecastModel::DateRole).toString(),
             followingDate.toString(Qt::ISODate));
    QCOMPARE(primary.data(followingRow, models::WeatherForecastModel::SunriseRole).toString(),
             QLocale().toString(parsed.value.days.at(1).sunriseUtc
                                    ->toTimeZone(berlin).time(), QLocale::ShortFormat));

    const auto noSolar = providers::parseWeatherForecast(weatherForecastJson(
        QStringLiteral("Europe/Berlin"), QJsonArray{date.toString(Qt::ISODate)}));
    primary.replace(noSolar.value.days, noSolar.value.timezone);
    QVERIFY(primary.data(primary.index(0), models::WeatherForecastModel::SunriseRole).toString().isEmpty());
    QVERIFY(!primary.data(primary.index(0), models::WeatherForecastModel::HasSunriseRole).toBool());
}

void WeatherCurrencyFeatureTest::weatherSolarCacheRoundTripsAndAcceptsOlderForecasts()
{
    const auto older = providers::parseWeatherForecast(
        fixture(QStringLiteral("weather-forecast-success.json")));
    QVERIFY(older.succeeded());
    for (const auto &day : older.value.days) {
        QVERIFY(!day.sunriseUtc.has_value());
        QVERIFY(!day.sunsetUtc.has_value());
        QVERIFY(!day.daylightDurationSeconds.has_value());
    }
    const auto olderRoundTrip = providers::parseWeatherForecast(
        providers::serializeWeatherForecast(older.value));
    QVERIFY(olderRoundTrip.succeeded());
    QVERIFY(!olderRoundTrip.value.days.first().sunriseUtc.has_value());

    const QTimeZone timezone(QByteArrayLiteral("Europe/Istanbul"));
    domain::WeatherForecast withSolar = older.value;
    withSolar.days.first().sunriseUtc = QDateTime(
        QDate(2026, 10, 19), QTime(7, 12), timezone).toUTC();
    withSolar.days.first().sunsetUtc = QDateTime(
        QDate(2026, 10, 19), QTime(18, 24), timezone).toUTC();
    withSolar.days.first().daylightDurationSeconds = 40320.0;
    const auto decoded = providers::parseWeatherForecast(
        providers::serializeWeatherForecast(withSolar));
    QVERIFY(decoded.succeeded());
    QCOMPARE(decoded.value.timezone, withSolar.timezone);
    QCOMPARE(decoded.value.days.first().sunriseUtc->toMSecsSinceEpoch(),
             withSolar.days.first().sunriseUtc->toMSecsSinceEpoch());
    QCOMPARE(decoded.value.days.first().sunsetUtc->toMSecsSinceEpoch(),
             withSolar.days.first().sunsetUtc->toMSecsSinceEpoch());
    QCOMPARE(*decoded.value.days.first().daylightDurationSeconds, 40320.0);
    QVERIFY(!decoded.value.days.at(1).sunriseUtc.has_value());

    QSqlDatabase database;
    QString connection;
    QVERIFY(openDatabase(&database, &connection));
    {
        storage::StorageRepositories repositories(database);
        const auto noCancel = [] { return false; };
        const QString updatedAt = QStringLiteral("2026-10-19T12:00:00.000Z");
        const auto saveOlder = services::WeatherService::savePrimaryLocation(
            41.0082, 28.9784, older.value, updatedAt);
        QVERIFY(saveOlder(repositories, noCancel).error.category()
                == domain::ErrorCategory::None);
        const storage::StorageReply loadedOlder = services::WeatherService::loadPrimaryLocation()(
            repositories, noCancel);
        const auto olderCache = providers::parseWeatherForecast(
            loadedOlder.values.value(QStringLiteral("forecastJson")).toString().toUtf8());
        QVERIFY(olderCache.succeeded());
        QVERIFY(!olderCache.value.days.first().sunriseUtc.has_value());

        const auto save = services::WeatherService::savePrimaryLocation(
            41.0082, 28.9784, withSolar, updatedAt);
        QVERIFY(save(repositories, noCancel).error.category() == domain::ErrorCategory::None);
        const storage::StorageReply loaded = services::WeatherService::loadPrimaryLocation()(
            repositories, noCancel);
        QVERIFY(loaded.values.value(QStringLiteral("found")).toBool());
        const auto cached = providers::parseWeatherForecast(
            loaded.values.value(QStringLiteral("forecastJson")).toString().toUtf8());
        QVERIFY(cached.succeeded());
        QCOMPARE(cached.value.days.first().sunriseUtc->toMSecsSinceEpoch(),
                 withSolar.days.first().sunriseUtc->toMSecsSinceEpoch());
        QCOMPARE(*cached.value.days.first().daylightDurationSeconds, 40320.0);
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

void WeatherCurrencyFeatureTest::cityLookupFixtureDistinguishesNoMatchFromCoordinates()
{
    const auto city = providers::parseWeatherCitySearch(
        fixture(QStringLiteral("weather-city-success.json")));
    QVERIFY(city.succeeded());
    QVERIFY(city.value.found);
    QCOMPARE(city.value.name, QStringLiteral("Istanbul"));
    QVERIFY(domain::isValidWeatherCoordinates(city.value.latitude, city.value.longitude));

    const auto empty = providers::parseWeatherCitySearch(
        fixture(QStringLiteral("weather-city-empty.json")));
    QVERIFY(empty.succeeded());
    QVERIFY(!empty.value.found);
    QVERIFY(!domain::isValidWeatherCoordinates(91.0, 0.0));
    QVERIFY(!domain::isValidWeatherCoordinates(0.0, std::numeric_limits<double>::quiet_NaN()));
}

void WeatherCurrencyFeatureTest::currencyProviderFixturesPreserveMissingAndSortAllRates()
{
    const auto pair = providers::parseCurrencyRate(
        fixture(QStringLiteral("currency-rate-success.json")),
        QStringLiteral(" usd "), QStringLiteral("eur"));
    QVERIFY(pair.succeeded());
    QVERIFY(pair.value.found);
    QCOMPARE(pair.value.rate, 0.8623);
    QCOMPARE(pair.value.date, QStringLiteral("2026-10-16"));
    QVERIFY(providers::parseCurrencyRate(
        fixture(QStringLiteral("currency-rate-mismatch.json")),
        QStringLiteral("USD"), QStringLiteral("EUR")).error.isError());

    const auto all = providers::parseCurrencyRates(
        fixture(QStringLiteral("currency-rates-success.json")), QStringLiteral(" usd "));
    QVERIFY(all.succeeded());
    QCOMPARE(all.value.rows.size(), 2);
    QCOMPARE(all.value.rows.at(0).quoteCode, QStringLiteral("EUR"));
    QCOMPARE(all.value.rows.at(1).quoteCode, QStringLiteral("TRY"));
    const auto empty = providers::parseCurrencyRates(
        fixture(QStringLiteral("currency-rates-empty.json")), QStringLiteral("USD"));
    QVERIFY(empty.succeeded());
    QVERIFY(empty.value.rows.isEmpty());
    QVERIFY(providers::parseCurrencyRates(
        fixture(QStringLiteral("currency-rates-malformed.json")), QStringLiteral("USD")).error.isError());
    QVERIFY(providers::parseCurrencyRates(
        fixture(QStringLiteral("currency-rates-mismatched-base.json")),
        QStringLiteral("USD")).error.isError());
}

void WeatherCurrencyFeatureTest::providerPoliciesMatchTheReferenceEndpoints()
{
    QVERIFY(providers::isAllowedProviderUrl(providers::ProviderId::Frankfurter,
        QUrl(QStringLiteral("https://api.frankfurter.dev/v2/rate/USD/EUR"))));
    QVERIFY(providers::isAllowedProviderUrl(providers::ProviderId::Frankfurter,
        QUrl(QStringLiteral("https://api.frankfurter.dev/v2/rates?base=USD"))));
    QVERIFY(!providers::isAllowedProviderUrl(providers::ProviderId::Frankfurter,
        QUrl(QStringLiteral("https://api.frankfurter.dev/v1/rates?base=USD"))));
    QVERIFY(providers::isAllowedProviderUrl(providers::ProviderId::OpenMeteoGeocoding,
        QUrl(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search?name=Istanbul"))));
}

void WeatherCurrencyFeatureTest::weatherAndCurrencyModelsPublishTypedRows()
{
    models::WeatherForecastModel weather;
    QSignalSpy weatherCountSpy(&weather, &models::WeatherForecastModel::countChanged);
    weather.replace(sampleForecast().days);
    QCOMPARE(weather.rowCount(), 6);
    QCOMPARE(weather.data(weather.index(0), models::WeatherForecastModel::ConditionRole).toString(),
             QStringLiteral("Partly cloudy"));
    QVERIFY(!weatherCountSpy.isEmpty());
    QCOMPARE(models::WeatherForecastModel::iconForCode(95), QString::fromUtf8("⛈️"));

    models::CurrencyRatesModel all;
    all.replace({QVariantMap{{QStringLiteral("code"), QStringLiteral("TRY")},
                             {QStringLiteral("rate"), 42.1}},
                 QVariantMap{{QStringLiteral("code"), QStringLiteral("EUR")},
                             {QStringLiteral("rate"), 0.8623}}});
    QCOMPARE(all.rowCount(), 2);
    QCOMPARE(all.data(all.index(0), models::CurrencyRatesModel::CodeRole).toString(),
             QStringLiteral("EUR"));
    all.setFilter(QStringLiteral("tr"));
    QCOMPARE(all.rowCount(), 1);
    QCOMPARE(all.totalCount(), 2);
    QCOMPARE(all.data(all.index(0), models::CurrencyRatesModel::CodeRole).toString(),
             QStringLiteral("TRY"));

    models::CurrencyFavoritesModel favorites;
    favorites.replace({QVariantMap{{QStringLiteral("baseCode"), QStringLiteral("USD")},
                                   {QStringLiteral("quoteCode"), QStringLiteral("EUR")},
                                   {QStringLiteral("code"), QStringLiteral("USD:EUR")}}});
    favorites.markAll(QStringLiteral("loading"));
    favorites.setRate(QStringLiteral("USD"), QStringLiteral("EUR"), false, 0.0, {},
                      QStringLiteral("missing"));
    QVERIFY(!favorites.data(favorites.index(0), models::CurrencyFavoritesModel::RateRole).isValid());
    QCOMPARE(favorites.data(favorites.index(0), models::CurrencyFavoritesModel::RateStatusRole).toString(),
             QStringLiteral("missing"));
}

void WeatherCurrencyFeatureTest::serviceCommandsPersistOnlyLocationsAndFavoriteDefinitions()
{
    QSqlDatabase database;
    QString connection;
    QVERIFY(openDatabase(&database, &connection));
    {
        storage::StorageRepositories repositories(database);
        const auto noCancel = [] { return false; };
        const auto add = services::CurrencyService::addFavorite(QStringLiteral(" usd "),
                                                                 QStringLiteral("eur"));
        const storage::StorageReply added = add(repositories, noCancel);
        QVERIFY(added.error.category() == domain::ErrorCategory::None);
        QVERIFY(added.values.value(QStringLiteral("added")).toBool());
        const storage::StorageReply duplicate = add(repositories, noCancel);
        QVERIFY(duplicate.values.value(QStringLiteral("exists")).toBool());
        const storage::StorageReply identical = services::CurrencyService::addFavorite(
            QStringLiteral("USD"), QStringLiteral("USD"))(repositories, noCancel);
        QCOMPARE(identical.error.category(), domain::ErrorCategory::Validation);
        const storage::StorageReply list = services::CurrencyService::listFavorites()(repositories, noCancel);
        const QVariantList rows = list.values.value(QStringLiteral("favorites")).toList();
        QCOMPARE(rows.size(), 1);
        QVERIFY(!rows.first().toMap().contains(QStringLiteral("rate")));

        const domain::WeatherForecast forecast = sampleForecast();
        const auto save = services::WeatherService::savePrimaryLocation(
            41.0082, 28.9784, forecast, QStringLiteral("2026-10-19T12:00:00.000Z"));
        QVERIFY(save(repositories, noCancel).error.category() == domain::ErrorCategory::None);
        const storage::StorageReply loaded = services::WeatherService::loadPrimaryLocation()(
            repositories, noCancel);
        QVERIFY(loaded.values.value(QStringLiteral("found")).toBool());
        QCOMPARE(loaded.values.value(QStringLiteral("latitude")).toDouble(), 41.0082);
        QVERIFY(providers::parseWeatherForecast(
            loaded.values.value(QStringLiteral("forecastJson")).toString().toUtf8()).succeeded());
        const auto invalid = services::WeatherService::savePrimaryLocation(
            91.0, 0.0, forecast, QStringLiteral("2026-10-19T12:00:00.000Z"));
        QCOMPARE(invalid(repositories, noCancel).error.category(), domain::ErrorCategory::Validation);
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

QTEST_GUILESS_MAIN(WeatherCurrencyFeatureTest)
#include "weather_currency_feature_test.moc"
