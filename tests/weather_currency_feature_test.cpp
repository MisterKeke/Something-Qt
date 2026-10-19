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
#include <QSqlDatabase>
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

} // namespace

class WeatherCurrencyFeatureTest final : public QObject {
    Q_OBJECT

private slots:
    void currencyRulesRequireAsciiCodesAndFinitePositiveAmounts();
    void weatherProviderFixturesKeepDateOnlyValuesAndRejectIncompleteDailyData();
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
