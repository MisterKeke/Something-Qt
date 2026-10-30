#include "weather_controller.h"

#include "../providers/feature_parsers.h"
#include "../services/weather_service.h"

#include <QDateTime>
#include <QLocale>
#include <QThread>
#include <QUrlQuery>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace something::controllers {
namespace {

using namespace something;

QString safeMessage(domain::AppError error, const QString &fallback)
{
    const QString message = error.userMessage();
    return message.isEmpty() ? fallback : message;
}

QString updatedAtText(const QString &value)
{
    QDateTime date = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(value, Qt::ISODate);
    if (!date.isValid()) return {};
    return QLocale().toString(date.toLocalTime(), QLocale::ShortFormat);
}

QString solarTimeText(const std::optional<QDateTime> &timestamp,
                      const QTimeZone &timeZone)
{
    if (!timestamp || !timestamp->isValid() || !timeZone.isValid()) return {};
    return QLocale().toString(timestamp->toUTC().toTimeZone(timeZone).time(),
                              QLocale::ShortFormat);
}

QString daylightDurationText(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return {};
    const qint64 minutes = static_cast<qint64>(std::llround(seconds / 60.0));
    const qint64 hoursPart = minutes / 60;
    const qint64 minutesPart = minutes % 60;
    if (hoursPart > 0 && minutesPart > 0)
        return QStringLiteral("%1h %2m").arg(hoursPart).arg(minutesPart);
    if (hoursPart > 0) return QStringLiteral("%1h").arg(hoursPart);
    return QStringLiteral("%1m").arg(minutesPart);
}

QString availableForecastDate(const domain::WeatherForecast &forecast,
                              const QString &requestedDate)
{
    for (const domain::WeatherDay &day : forecast.days) {
        if (day.date == requestedDate) return requestedDate;
    }
    return forecast.days.isEmpty() ? QString{} : forecast.days.first().date;
}

QVariantMap solarInfoForForecast(const domain::WeatherForecast &forecast,
                                 const QString &selectedDate,
                                 const QDateTime &nowUtc)
{
    if (forecast.days.isEmpty()) return {};
    const QTimeZone timeZone(forecast.timezone.toUtf8());
    const auto selectedDay = std::find_if(forecast.days.cbegin(), forecast.days.cend(),
        [&selectedDate](const domain::WeatherDay &day) { return day.date == selectedDate; });
    if (selectedDay == forecast.days.cend()) return {};
    const domain::WeatherDay &displayedDay = *selectedDay;
    const bool hasSunrise = displayedDay.sunriseUtc && displayedDay.sunriseUtc->isValid()
        && timeZone.isValid();
    const bool hasSunset = displayedDay.sunsetUtc && displayedDay.sunsetUtc->isValid()
        && timeZone.isValid();
    const bool hasDaylightDuration = displayedDay.daylightDurationSeconds
        && std::isfinite(*displayedDay.daylightDurationSeconds)
        && *displayedDay.daylightDurationSeconds >= 0.0;

    QVariantMap result{
        {QStringLiteral("date"), displayedDay.date},
        {QStringLiteral("sunrise"), hasSunrise
             ? solarTimeText(displayedDay.sunriseUtc, timeZone) : QString{}},
        {QStringLiteral("sunset"), hasSunset
             ? solarTimeText(displayedDay.sunsetUtc, timeZone) : QString{}},
        {QStringLiteral("hasSunrise"), hasSunrise},
        {QStringLiteral("hasSunset"), hasSunset},
        {QStringLiteral("daylightDuration"), hasDaylightDuration
             ? daylightDurationText(*displayedDay.daylightDurationSeconds) : QString{}},
        {QStringLiteral("hasDaylightDuration"), hasDaylightDuration},
        {QStringLiteral("hasSolarState"), false},
        {QStringLiteral("solarState"), QString{}},
        {QStringLiteral("hasNextEvent"), false},
        {QStringLiteral("nextEvent"), QString{}},
        {QStringLiteral("secondsUntilNextEvent"), 0},
    };

    if (!timeZone.isValid()) return result;
    const QDateTime now = nowUtc.toUTC();
    const QString localToday = now.toTimeZone(timeZone).date().toString(Qt::ISODate);
    if (displayedDay.date != localToday) return result;

    const bool hasOrderedPair = hasSunrise && hasSunset
        && *displayedDay.sunriseUtc < *displayedDay.sunsetUtc;
    QString state;
    if (hasOrderedPair) {
        if (now < *displayedDay.sunriseUtc) state = QStringLiteral("beforeSunrise");
        else if (now < *displayedDay.sunsetUtc) state = QStringLiteral("daylight");
        else state = QStringLiteral("afterSunset");
    } else if (hasDaylightDuration
               && *displayedDay.daylightDurationSeconds >= 86400.0 - 60.0) {
        state = QStringLiteral("polarDay");
    } else if (hasDaylightDuration && *displayedDay.daylightDurationSeconds <= 60.0) {
        state = QStringLiteral("polarNight");
    }
    if (!state.isEmpty()) {
        result.insert(QStringLiteral("hasSolarState"), true);
        result.insert(QStringLiteral("solarState"), state);
    }

    QDateTime nextEvent;
    QString nextEventName;
    const auto considerEvent = [&now, &nextEvent, &nextEventName](
                                   const std::optional<QDateTime> &event,
                                   const QString &name) {
        if (!event || !event->isValid()) return;
        const QDateTime utcEvent = event->toUTC();
        if (utcEvent <= now || (nextEvent.isValid() && utcEvent >= nextEvent)) return;
        nextEvent = utcEvent;
        nextEventName = name;
    };
    for (const domain::WeatherDay &day : forecast.days) {
        considerEvent(day.sunriseUtc, QStringLiteral("sunrise"));
        considerEvent(day.sunsetUtc, QStringLiteral("sunset"));
    }
    if (nextEvent.isValid()) {
        result.insert(QStringLiteral("hasNextEvent"), true);
        result.insert(QStringLiteral("nextEvent"), nextEventName);
        result.insert(QStringLiteral("secondsUntilNextEvent"), now.secsTo(nextEvent));
    }
    return result;
}

QVariantMap currentMap(const domain::WeatherCurrent &current)
{
    QVariantMap result;
    if (current.temperatureCelsius) {
        result.insert(QStringLiteral("temperature"), *current.temperatureCelsius);
        result.insert(QStringLiteral("hasTemperature"), true);
    }
    if (current.apparentTemperatureCelsius) {
        result.insert(QStringLiteral("feelsLike"), *current.apparentTemperatureCelsius);
        result.insert(QStringLiteral("hasFeelsLike"), true);
    }
    if (current.windKmh) {
        result.insert(QStringLiteral("wind"), *current.windKmh);
        result.insert(QStringLiteral("hasWind"), true);
    }
    if (current.weatherCode) {
        result.insert(QStringLiteral("weatherCode"), *current.weatherCode);
        result.insert(QStringLiteral("condition"), models::WeatherForecastModel::conditionForCode(*current.weatherCode));
        result.insert(QStringLiteral("icon"), models::WeatherForecastModel::iconForCode(*current.weatherCode));
        result.insert(QStringLiteral("hasWeather"), true);
    }
    return result;
}

QVariantMap weatherCityMap(const domain::WeatherCity &city)
{
    return {{QStringLiteral("identity"), domain::stableWeatherCityIdentity(city)},
            {QStringLiteral("geocodingId"), city.geocodingId},
            {QStringLiteral("name"), city.name},
            {QStringLiteral("administrativeArea"), city.administrativeArea},
            {QStringLiteral("country"), city.country},
            {QStringLiteral("timezone"), city.timezone},
            {QStringLiteral("latitude"), city.latitude},
            {QStringLiteral("longitude"), city.longitude}};
}

domain::WeatherCity weatherCityFromMap(const QVariantMap &values)
{
    domain::WeatherCity city;
    city.geocodingId = values.value(QStringLiteral("geocodingId")).toLongLong();
    city.name = values.value(QStringLiteral("name")).toString();
    city.administrativeArea = values.value(QStringLiteral("administrativeArea")).toString();
    city.country = values.value(QStringLiteral("country")).toString();
    city.timezone = values.value(QStringLiteral("timezone")).toString();
    city.latitude = values.value(QStringLiteral("latitude")).toDouble();
    city.longitude = values.value(QStringLiteral("longitude")).toDouble();
    city.found = true;
    return city;
}

QUrl forecastUrl(double latitude, double longitude)
{
    QUrl url(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("latitude"), QString::number(latitude, 'f', 6));
    query.addQueryItem(QStringLiteral("longitude"), QString::number(longitude, 'f', 6));
    query.addQueryItem(QStringLiteral("current"), QStringLiteral("temperature_2m,apparent_temperature,weather_code,wind_speed_10m"));
    query.addQueryItem(QStringLiteral("daily"), QStringLiteral("weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,daylight_duration"));
    query.addQueryItem(QStringLiteral("timezone"), QStringLiteral("auto"));
    query.addQueryItem(QStringLiteral("timeformat"), QStringLiteral("unixtime"));
    query.addQueryItem(QStringLiteral("temperature_unit"), QStringLiteral("celsius"));
    query.addQueryItem(QStringLiteral("wind_speed_unit"), QStringLiteral("kmh"));
    query.addQueryItem(QStringLiteral("precipitation_unit"), QStringLiteral("mm"));
    query.addQueryItem(QStringLiteral("forecast_days"), QStringLiteral("6"));
    url.setQuery(query);
    return url;
}

QUrl geocodingUrl(const QString &city)
{
    QUrl url(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("name"), city);
    query.addQueryItem(QStringLiteral("count"), QStringLiteral("10"));
    query.addQueryItem(QStringLiteral("language"), QStringLiteral("en"));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(query);
    return url;
}

QString cityLabel(const domain::WeatherCity &city)
{
    QStringList parts;
    for (const QString &part : {city.name, city.administrativeArea, city.country}) {
        if (!part.trimmed().isEmpty() && !parts.contains(part)) parts.push_back(part);
    }
    return parts.join(QStringLiteral(", "));
}

QString forecastLocation(const domain::WeatherForecast &forecast)
{
    return forecast.timezone.isEmpty()
        ? QStringLiteral("Your location")
        : QStringLiteral("Your location (%1)").arg(forecast.timezone);
}

QString comparisonFreshnessKey(const QString &identity)
{
    return QStringLiteral("weather-comparison:%1").arg(identity);
}

QString comparisonNumber(std::optional<double> value, const QString &suffix)
{
    if (!value || !std::isfinite(*value)) return QStringLiteral("—");
    return QLocale().toString(*value, 'f', 0) + suffix;
}

QVariantMap weatherDayMap(const domain::WeatherDay &day)
{
    QVariantMap row{
        {QStringLiteral("date"), day.date},
        {QStringLiteral("weatherCode"), day.weatherCode},
        {QStringLiteral("maximum"), day.maximumCelsius},
        {QStringLiteral("minimum"), day.minimumCelsius},
        {QStringLiteral("precipitation"), day.precipitationProbability},
    };
    if (day.sunriseUtc)
        row.insert(QStringLiteral("sunriseUtc"), *day.sunriseUtc);
    if (day.sunsetUtc)
        row.insert(QStringLiteral("sunsetUtc"), *day.sunsetUtc);
    if (day.daylightDurationSeconds)
        row.insert(QStringLiteral("daylightDurationSeconds"), *day.daylightDurationSeconds);
    return row;
}

domain::WeatherDay weatherDayFromMap(const QVariantMap &row)
{
    domain::WeatherDay day{row.value(QStringLiteral("date")).toString(),
                           row.value(QStringLiteral("weatherCode")).toInt(),
                           row.value(QStringLiteral("maximum")).toDouble(),
                           row.value(QStringLiteral("minimum")).toDouble(),
                           row.value(QStringLiteral("precipitation")).toDouble()};
    const QDateTime sunrise = row.value(QStringLiteral("sunriseUtc")).toDateTime();
    if (sunrise.isValid()) day.sunriseUtc = sunrise.toUTC();
    const QDateTime sunset = row.value(QStringLiteral("sunsetUtc")).toDateTime();
    if (sunset.isValid()) day.sunsetUtc = sunset.toUTC();
    if (row.contains(QStringLiteral("daylightDurationSeconds")))
        day.daylightDurationSeconds = row.value(QStringLiteral("daylightDurationSeconds")).toDouble();
    return day;
}

domain::WeatherForecast weatherForecastFromValues(const QVariantMap &values)
{
    domain::WeatherForecast forecast;
    const QVariantMap current = values.value(QStringLiteral("current")).toMap();
    if (current.contains(QStringLiteral("temperature")))
        forecast.current.temperatureCelsius = current.value(QStringLiteral("temperature")).toDouble();
    if (current.contains(QStringLiteral("feelsLike")))
        forecast.current.apparentTemperatureCelsius = current.value(QStringLiteral("feelsLike")).toDouble();
    if (current.contains(QStringLiteral("weatherCode")))
        forecast.current.weatherCode = current.value(QStringLiteral("weatherCode")).toInt();
    if (current.contains(QStringLiteral("wind")))
        forecast.current.windKmh = current.value(QStringLiteral("wind")).toDouble();
    forecast.timezone = values.value(QStringLiteral("timezone")).toString();
    for (const QVariant &value : values.value(QStringLiteral("days")).toList())
        forecast.days.push_back(weatherDayFromMap(value.toMap()));
    return forecast;
}

} // namespace

WeatherController::WeatherController(application::ApplicationServiceGraph *graph,
                                     const domain::Clock *clock,
                                     platform::LocationProvider *locationProvider,
                                     QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_clock(clock)
    , m_locationProvider(locationProvider)
    , m_primaryForecast(this)
    , m_cityForecast(this)
    , m_comparisonCities(this)
    , m_locationAvailable(locationProvider && locationProvider->available())
{
    Q_ASSERT(m_graph && m_clock && m_locationProvider);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &WeatherController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::httpsFinished,
            this, &WeatherController::onHttpsFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &WeatherController::onValueJobFinished);
    connect(m_locationProvider, &platform::LocationProvider::positionReady,
            this, &WeatherController::onNativeLocationReady);
    connect(&m_comparisonCities, &models::WeatherComparisonModel::contentsChanged,
            this, &WeatherController::comparisonChanged);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, [this](application::ApplicationLifecycle::State state) { onLifecycleState(state); });
    connect(&m_graph->operations(), &application::OperationCoordinator::cancellationRequested,
            this, [this](quint64 id) {
        if (id == m_locationOperation) m_locationProvider->cancel(id);
    });

    if (m_locationAvailable)
        m_graph->reportCapabilityAvailable(QStringLiteral("native-location"));
    else
        m_graph->reportCapabilityUnavailable(QStringLiteral("native-location"),
            domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
    onLifecycleState(m_graph->lifecycle().state());
}

void WeatherController::onLifecycleState(application::ApplicationLifecycle::State state)
{
    if (state == application::ApplicationLifecycle::State::Running) {
        if (!m_ready) {
            m_ready = true;
            beginPrimaryLoad();
            beginComparisonLoad();
        }
        return;
    }
    if (state == application::ApplicationLifecycle::State::ShuttingDown
        || state == application::ApplicationLifecycle::State::Failed
        || state == application::ApplicationLifecycle::State::Stopped) {
        m_ready = false;
        cancelPrimaryOperations();
        cancelCityOperations();
        cancelComparisonOperations();
        m_primaryLoading = false;
        m_primaryRefreshing = false;
        m_citySearching = false;
        m_cityCandidates.clear();
        emit primaryChanged();
        emit cityChanged();
        emit comparisonChanged();
    }
}

quint64 WeatherController::submitStorage(Kind kind, storage::StorageCommand command,
                                         QString freshnessKey, quint64 generation)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("weather storage operation"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    m_pending.insert(id, Pending{kind, generation});
    if (!m_graph->submitStorage(operation, std::move(command))) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 WeatherController::submitNetwork(Kind kind, providers::ProviderRequest request,
                                         QString freshnessKey, quint64 generation,
                                         double latitude, double longitude,
                                         bool saveLocation, domain::WeatherCity city)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("weather provider request"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    m_pending.insert(id, Pending{kind, generation, latitude, longitude, saveLocation, std::move(city)});
    if (!m_graph->requestHttps(operation, std::move(request))) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 WeatherController::submitParse(Kind kind, QByteArray body, QString freshnessKey,
                                       quint64 generation, double latitude, double longitude,
                                       bool saveLocation, domain::WeatherCity city)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("parse weather provider response"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    m_pending.insert(id, Pending{kind, generation, latitude, longitude, saveLocation, std::move(city)});
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [kind, body = std::move(body)](const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            if (cancellation.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            if (kind == Kind::CityGeocodeParse) {
                const auto parsed = providers::parseWeatherCitySearchResults(body);
                if (!parsed.succeeded()) result.error = parsed.error;
                else {
                    QVariantList candidates;
                    candidates.reserve(parsed.value.size());
                    for (const domain::WeatherCity &city : parsed.value)
                        candidates.push_back(weatherCityMap(city));
                    result.values.insert(QStringLiteral("candidates"), candidates);
                }
                return result;
            }
            const auto parsed = providers::parseWeatherForecast(body);
            if (!parsed.succeeded()) {
                result.error = parsed.error;
                return result;
            }
            const domain::WeatherForecast &forecast = parsed.value;
            QVariantList days;
            days.reserve(forecast.days.size());
            for (const domain::WeatherDay &day : forecast.days) {
                days.push_back(weatherDayMap(day));
            }
            QVariantMap current;
            if (forecast.current.temperatureCelsius)
                current.insert(QStringLiteral("temperature"), *forecast.current.temperatureCelsius);
            if (forecast.current.apparentTemperatureCelsius)
                current.insert(QStringLiteral("feelsLike"), *forecast.current.apparentTemperatureCelsius);
            if (forecast.current.weatherCode)
                current.insert(QStringLiteral("weatherCode"), *forecast.current.weatherCode);
            if (forecast.current.windKmh)
                current.insert(QStringLiteral("wind"), *forecast.current.windKmh);
            result.values.insert(QStringLiteral("current"), current);
            result.values.insert(QStringLiteral("days"), days);
            result.values.insert(QStringLiteral("timezone"), forecast.timezone);
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

void WeatherController::cancelPending(quint64 id)
{
    if (!id) return;
    if (id == m_locationOperation) {
        m_locationProvider->cancel(id);
        m_locationOperation = 0;
    }
    m_stagedPrimary.remove(id);
    m_pending.remove(id);
    m_graph->cancelOperation({id});
}

void WeatherController::cancelPrimaryOperations()
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it != m_pending.cend() && it->kind != Kind::CityGeocode
            && it->kind != Kind::CityGeocodeParse && it->kind != Kind::CityForecast
            && it->kind != Kind::CityForecastParse
            && it->kind != Kind::ComparisonForecast
            && it->kind != Kind::ComparisonForecastParse) cancelPending(id);
    }
}

void WeatherController::cancelCityOperations()
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it != m_pending.cend() && (it->kind == Kind::CityGeocode
            || it->kind == Kind::CityGeocodeParse || it->kind == Kind::CityForecast
            || it->kind == Kind::CityForecastParse)) cancelPending(id);
    }
}

void WeatherController::cancelComparisonCityOperations(const QString &identity)
{
    if (identity.isEmpty()) return;
    m_comparisonGenerations.insert(identity, m_comparisonGenerations.value(identity) + 1);
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it == m_pending.cend()) continue;
        const bool comparisonOperation = it->kind == Kind::ComparisonForecast
            || it->kind == Kind::ComparisonForecastParse;
        if (comparisonOperation
            && domain::stableWeatherCityIdentity(it->city) == identity) cancelPending(id);
    }
}

void WeatherController::cancelComparisonOperations()
{
    QStringList identities;
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it == m_pending.cend()) continue;
        const bool forecastOperation = it->kind == Kind::ComparisonForecast
            || it->kind == Kind::ComparisonForecastParse;
        const bool storageOperation = it->kind == Kind::LoadComparison
            || it->kind == Kind::AddComparisonCity
            || it->kind == Kind::RemoveComparisonCity
            || it->kind == Kind::ReplaceComparisonCity
            || it->kind == Kind::ReorderComparisonCities
            || it->kind == Kind::SaveComparisonForecast;
        if (!forecastOperation && !storageOperation) continue;
        if (forecastOperation) {
            const QString identity = domain::stableWeatherCityIdentity(it->city);
            if (!identity.isEmpty() && !identities.contains(identity))
                identities.push_back(identity);
        }
        cancelPending(id);
    }
    for (const QString &identity : identities)
        m_comparisonGenerations.insert(identity, m_comparisonGenerations.value(identity) + 1);
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        const QString identity = m_comparisonCities.data(
            m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString();
        m_comparisonCities.setLoading(identity, false);
    }
    m_comparisonMutationPending = false;
}

void WeatherController::beginPrimaryLoad()
{
    if (!m_ready || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running)
        return;
    cancelPrimaryOperations();
    const quint64 generation = ++m_primaryGeneration;
    m_primaryError.clear();
    m_primaryLoading = !m_hasLocation;
    m_primaryRefreshing = m_hasLocation || m_primaryForecast.count() > 0;
    if (!m_primaryForecast.count()) m_primaryStatus = QStringLiteral("Loading saved location…");
    emit primaryChanged();
    const quint64 id = submitStorage(Kind::LoadPrimary,
        services::WeatherService::loadPrimaryLocation(),
        QStringLiteral("weather-primary"), generation);
    if (!id) {
        m_primaryLoading = false;
        m_primaryRefreshing = false;
        m_primaryError = QStringLiteral("Saved weather location could not be loaded.");
        emit primaryChanged();
    }
}

void WeatherController::beginComparisonLoad()
{
    if (!m_ready || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running)
        return;
    m_comparisonLoaded = false;
    m_comparisonStorageError.clear();
    emit comparisonChanged();
    const quint64 id = submitStorage(Kind::LoadComparison,
        services::WeatherService::loadComparisonCities(),
        QStringLiteral("weather-comparison-list"), 0);
    if (!id) {
        m_comparisonLoaded = true;
        m_comparisonStorageError = QStringLiteral("Saved comparison cities could not be loaded.");
        emit comparisonChanged();
        updateComparisonLocalTimes();
        refreshAllComparisonCities();
    }
}

void WeatherController::refreshPrimary()
{
    beginPrimaryLoad();
}

void WeatherController::requestDeviceLocation()
{
    if (!m_ready || !m_locationAvailable) {
        m_primaryError = QStringLiteral("Device location is unavailable. You can still search for a city.");
        emit primaryChanged();
        return;
    }
    cancelPrimaryOperations();
    requestNativeLocation(++m_primaryGeneration);
}

void WeatherController::requestNativeLocation(quint64 generation)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("request native location"),
                                                    QStringLiteral("weather-primary"));
    if (!operation.isValid()) return;
    const quint64 id = operation.id.value;
    m_pending.insert(id, Pending{Kind::NativeLocation, generation});
    m_locationOperation = id;
    m_primaryLoading = m_primaryForecast.count() == 0;
    m_primaryRefreshing = m_primaryForecast.count() > 0;
    m_primaryError.clear();
    m_primaryStatus = QStringLiteral("Finding device location…");
    emit primaryChanged();
    m_locationProvider->request(id);
}

void WeatherController::searchCity(QString city)
{
    city = city.trimmed();
    if (!m_ready) return;
    cancelCityOperations();
    const quint64 generation = ++m_cityGeneration;
    m_cityCandidates.clear();
    m_searchedCity = {};
    m_hasSearchedCity = false;
    m_cityUpdatedAt.clear();
    m_cityUpdatedAtUtc.clear();
    m_cityForecast.clear();
    m_citySolarForecast = {};
    m_citySelectedDate.clear();
    m_cityCurrent.clear();
    m_citySolar.clear();
    m_cityLocation.clear();
    m_cityError.clear();
    m_citySearching = false;
    const int codePoints = city.toUcs4().size();
    if (codePoints < 1 || codePoints > 100) {
        m_cityError = QStringLiteral("Enter a city name between 1 and 100 characters.");
        emit cityChanged();
        emit comparisonChanged();
        return;
    }
    m_citySearching = true;
    emit cityChanged();
    emit comparisonChanged();
    providers::ProviderRequest request;
    request.provider = providers::ProviderId::OpenMeteoGeocoding;
    request.url = geocodingUrl(city);
    const quint64 id = submitNetwork(Kind::CityGeocode, std::move(request),
        QStringLiteral("weather-city"), generation);
    if (!id) setCityError(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                          QStringLiteral("City search is unavailable right now."));
}

void WeatherController::selectCityCandidate(int candidateIndex)
{
    if (!m_ready || candidateIndex < 0 || candidateIndex >= m_cityCandidates.size()) return;
    const domain::WeatherCity city = m_cityCandidates.at(candidateIndex);
    const quint64 generation = m_cityGeneration;
    m_cityCandidates.clear();
    m_searchedCity = city;
    m_hasSearchedCity = true;
    m_cityLocation = cityLabel(city);
    m_cityUpdatedAt.clear();
    m_cityUpdatedAtUtc.clear();
    m_cityError.clear();
    m_citySearching = true;
    emit cityChanged();
    emit comparisonChanged();
    beginCityForecast(city, generation);
}

bool WeatherController::canAddSearchedCity() const noexcept
{
    return m_comparisonLoaded && !m_comparisonMutationPending
        && m_hasSearchedCity && !m_citySolarForecast.days.isEmpty()
        && m_comparisonCities.count() < models::WeatherComparisonModel::maximumCities()
        && !m_comparisonCities.containsCity(m_searchedCity);
}

bool WeatherController::canReplaceSearchedCity() const noexcept
{
    return m_comparisonLoaded && !m_comparisonMutationPending && m_hasSearchedCity
        && !m_citySolarForecast.days.isEmpty() && m_comparisonCities.count() > 0
        && !m_comparisonCities.containsCity(m_searchedCity);
}

bool WeatherController::searchedCityAlreadyAdded() const noexcept
{
    return m_hasSearchedCity && m_comparisonCities.containsCity(m_searchedCity);
}

QVariantList WeatherController::cityCandidates() const
{
    QVariantList result;
    result.reserve(m_cityCandidates.size());
    for (const domain::WeatherCity &city : m_cityCandidates)
        result.push_back(weatherCityMap(city));
    return result;
}

QVariantList WeatherController::comparisonTableHeaders() const
{
    QVariantList headers;
    headers.reserve(m_comparisonCities.count());
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        const QString identity = m_comparisonCities.data(
            m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString();
        const auto entry = m_comparisonCities.cityByIdentity(identity);
        if (!entry) continue;
        headers.push_back(QVariantMap{
            {QStringLiteral("name"), cityLabel(entry->city)},
            {QStringLiteral("date"), m_comparisonSelectedDate},
            {QStringLiteral("dateAvailable"), !m_comparisonSelectedDate.isEmpty()
                && entry->selectedForecastDate == m_comparisonSelectedDate},
            {QStringLiteral("hasForecast"), !entry->forecast.days.isEmpty()},
            {QStringLiteral("loading"), entry->loading},
        });
    }
    return headers;
}

QVariantList WeatherController::comparisonTableRows() const
{
    QVector<domain::WeatherComparisonCity> cities;
    cities.reserve(m_comparisonCities.count());
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        const QString identity = m_comparisonCities.data(
            m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString();
        const auto entry = m_comparisonCities.cityByIdentity(identity);
        if (entry) cities.push_back(*entry);
    }

    const std::array<QString, 7> metricKeys{
        QStringLiteral("temperature"), QStringLiteral("feelsLike"),
        QStringLiteral("maximum"), QStringLiteral("minimum"),
        QStringLiteral("wind"), QStringLiteral("precipitation"),
        QStringLiteral("condition")};
    QVariantList rows;
    rows.reserve(metricKeys.size());
    for (const QString &key : metricKeys) {
        QVariantList values;
        values.reserve(cities.size());
        for (const domain::WeatherComparisonCity &entry : cities) {
            const auto selectedDay = std::find_if(entry.forecast.days.cbegin(),
                entry.forecast.days.cend(), [&entry](const domain::WeatherDay &day) {
                    return day.date == entry.selectedForecastDate;
                });
            const bool hasSelectedDay = selectedDay != entry.forecast.days.cend();
            QString text = QStringLiteral("—");
            if (key == QStringLiteral("temperature")) {
                text = comparisonNumber(entry.forecast.current.temperatureCelsius,
                                        QStringLiteral("°C"));
            } else if (key == QStringLiteral("feelsLike")) {
                text = comparisonNumber(entry.forecast.current.apparentTemperatureCelsius,
                                        QStringLiteral("°C"));
            } else if (key == QStringLiteral("maximum") && hasSelectedDay) {
                text = comparisonNumber(selectedDay->maximumCelsius, QStringLiteral("°C"));
            } else if (key == QStringLiteral("minimum") && hasSelectedDay) {
                text = comparisonNumber(selectedDay->minimumCelsius, QStringLiteral("°C"));
            } else if (key == QStringLiteral("wind")) {
                text = comparisonNumber(entry.forecast.current.windKmh,
                                        QStringLiteral(" km/h"));
            } else if (key == QStringLiteral("precipitation") && hasSelectedDay) {
                text = comparisonNumber(selectedDay->precipitationProbability,
                                        QStringLiteral("%"));
            } else if (key == QStringLiteral("condition")
                       && entry.forecast.current.weatherCode) {
                text = models::WeatherForecastModel::conditionForCode(
                    *entry.forecast.current.weatherCode);
            }
            values.push_back(QVariantMap{
                {QStringLiteral("text"), text},
                {QStringLiteral("city"), cityLabel(entry.city)},
            });
        }
        rows.push_back(QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("values"), values},
        });
    }
    return rows;
}

QVariantList WeatherController::comparisonDateOptions() const
{
    QVector<QString> dates;
    int forecastedCityCount = 0;
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        const QString identity = m_comparisonCities.data(
            m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString();
        const auto entry = m_comparisonCities.cityByIdentity(identity);
        if (!entry || entry->forecast.days.isEmpty()) continue;
        ++forecastedCityCount;
        for (const domain::WeatherDay &day : entry->forecast.days) {
            if (!day.date.isEmpty() && !dates.contains(day.date)) dates.push_back(day.date);
        }
    }
    std::sort(dates.begin(), dates.end());

    QVariantList options;
    options.reserve(dates.size());
    for (const QString &date : dates) {
        int availableCount = 0;
        for (int row = 0; row < m_comparisonCities.count(); ++row) {
            const QString identity = m_comparisonCities.data(
                m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString();
            const auto entry = m_comparisonCities.cityByIdentity(identity);
            if (!entry) continue;
            const bool available = std::any_of(entry->forecast.days.cbegin(),
                entry->forecast.days.cend(), [&date](const domain::WeatherDay &day) {
                    return day.date == date;
                });
            if (available) ++availableCount;
        }
        options.push_back(QVariantMap{
            {QStringLiteral("date"), date},
            {QStringLiteral("availableCount"), availableCount},
            {QStringLiteral("cityCount"), m_comparisonCities.count()},
            {QStringLiteral("forecastedCityCount"), forecastedCityCount},
            {QStringLiteral("common"), forecastedCityCount > 0
                && availableCount == forecastedCityCount},
        });
    }
    return options;
}

bool WeatherController::addSearchedCityToComparison()
{
    if (!canAddSearchedCity()) return false;
    const quint64 id = submitStorage(Kind::AddComparisonCity,
        services::WeatherService::addComparisonCity(
            m_searchedCity, m_citySolarForecast, m_cityUpdatedAtUtc),
        QStringLiteral("weather-comparison-list"), 0);
    if (!id) {
        m_comparisonStorageError = QStringLiteral("The city could not be saved to comparison.");
        emit comparisonChanged();
        return false;
    }
    Pending &pending = m_pending[id];
    pending.city = m_searchedCity;
    pending.forecast = m_citySolarForecast;
    pending.updatedAt = m_cityUpdatedAtUtc;
    m_comparisonMutationPending = true;
    m_comparisonStorageError.clear();
    emit comparisonChanged();
    return true;
}

void WeatherController::replaceComparisonCity(QString identity)
{
    if (!canReplaceSearchedCity()
        || m_comparisonCities.indexOfIdentity(identity) < 0) return;
    cancelComparisonCityOperations(identity);
    m_comparisonCities.setLoading(identity, false);
    const quint64 id = submitStorage(Kind::ReplaceComparisonCity,
        services::WeatherService::replaceComparisonCity(
            identity, m_searchedCity, m_citySolarForecast, m_cityUpdatedAtUtc),
        QStringLiteral("weather-comparison-list"), 0);
    if (!id) {
        m_comparisonStorageError = QStringLiteral("The selected city could not be replaced.");
        emit comparisonChanged();
        return;
    }
    Pending &pending = m_pending[id];
    pending.identity = std::move(identity);
    pending.city = m_searchedCity;
    pending.forecast = m_citySolarForecast;
    pending.updatedAt = m_cityUpdatedAtUtc;
    m_comparisonMutationPending = true;
    m_comparisonStorageError.clear();
    emit comparisonChanged();
}

void WeatherController::removeComparisonCity(QString identity)
{
    if (!m_comparisonLoaded || m_comparisonMutationPending
        || m_comparisonCities.indexOfIdentity(identity) < 0) return;
    cancelComparisonCityOperations(identity);
    m_comparisonCities.setLoading(identity, false);
    const quint64 id = submitStorage(Kind::RemoveComparisonCity,
        services::WeatherService::removeComparisonCity(identity),
        QStringLiteral("weather-comparison-list"), 0);
    if (!id) {
        m_comparisonStorageError = QStringLiteral("The city could not be removed from saved comparison.");
        emit comparisonChanged();
        return;
    }
    m_pending[id].identity = std::move(identity);
    m_comparisonMutationPending = true;
    m_comparisonStorageError.clear();
    emit comparisonChanged();
}

void WeatherController::moveComparisonCity(QString identity, int offset)
{
    if (!m_comparisonLoaded || m_comparisonMutationPending
        || (offset != -1 && offset != 1)) return;
    const int sourceRow = m_comparisonCities.indexOfIdentity(identity);
    if (sourceRow < 0) return;
    const int destinationRow = sourceRow + offset;
    if (destinationRow < 0 || destinationRow >= m_comparisonCities.count()) return;
    QVector<QString> identities;
    identities.reserve(m_comparisonCities.count());
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        identities.push_back(m_comparisonCities.data(
            m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString());
    }
    identities.move(sourceRow, destinationRow);
    const quint64 id = submitStorage(Kind::ReorderComparisonCities,
        services::WeatherService::reorderComparisonCities(identities),
        QStringLiteral("weather-comparison-list"), 0);
    if (!id) {
        m_comparisonStorageError = QStringLiteral("The comparison order could not be saved.");
        emit comparisonChanged();
        return;
    }
    m_pending[id].identity = std::move(identity);
    m_pending[id].destinationRow = destinationRow;
    m_comparisonMutationPending = true;
    m_comparisonStorageError.clear();
    emit comparisonChanged();
}

void WeatherController::refreshComparisonCity(QString identity)
{
    if (!m_ready
        || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running) return;
    const auto entry = m_comparisonCities.cityByIdentity(identity);
    if (!entry || entry->loading) return;
    const quint64 generation = m_comparisonGenerations.value(identity) + 1;
    m_comparisonGenerations.insert(identity, generation);
    m_comparisonCities.setLoading(identity, true);
    beginComparisonForecast(entry->city, generation);
}

void WeatherController::refreshAllComparisonCities()
{
    if (!m_ready) return;
    QStringList identities;
    identities.reserve(m_comparisonCities.count());
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        const QString identity = m_comparisonCities.data(
            m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString();
        if (!identity.isEmpty()) identities.push_back(identity);
    }
    for (const QString &identity : identities) refreshComparisonCity(identity);
}

void WeatherController::cancelComparisonCityRefresh(QString identity)
{
    if (m_comparisonCities.indexOfIdentity(identity) < 0) return;
    cancelComparisonCityOperations(identity);
    m_comparisonCities.setLoading(identity, false);
}

void WeatherController::selectComparisonForecastDate(QString date)
{
    const auto options = comparisonDateOptions();
    const bool available = std::any_of(options.cbegin(), options.cend(),
        [&date](const QVariant &value) {
            const QVariantMap option = value.toMap();
            return option.value(QStringLiteral("date")).toString() == date
                && option.value(QStringLiteral("availableCount")).toInt() > 0;
        });
    if (!available) return;
    m_comparisonDateUserSelected = true;
    m_comparisonSelectedDate = std::move(date);
    reconcileComparisonDate();
}

void WeatherController::cancelCitySearch()
{
    cancelCityOperations();
    ++m_cityGeneration;
    m_cityCandidates.clear();
    m_citySearching = false;
    emit cityChanged();
    emit comparisonChanged();
}

void WeatherController::beginPrimaryForecast(double latitude, double longitude,
                                             bool saveLocation, quint64 generation)
{
    if (!domain::isValidWeatherCoordinates(latitude, longitude)) {
        setPrimaryError(domain::AppError(domain::ErrorCategory::Validation),
                        QStringLiteral("The device returned invalid coordinates."));
        return;
    }
    providers::ProviderRequest request;
    request.provider = providers::ProviderId::OpenMeteoForecast;
    request.url = forecastUrl(latitude, longitude);
    const quint64 id = submitNetwork(Kind::PrimaryForecast, std::move(request),
        QStringLiteral("weather-primary"), generation, latitude, longitude, saveLocation);
    if (!id) setPrimaryError(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                             QStringLiteral("Weather is unavailable right now."));
}

void WeatherController::beginCityForecast(const domain::WeatherCity &city, quint64 generation)
{
    providers::ProviderRequest request;
    request.provider = providers::ProviderId::OpenMeteoForecast;
    request.url = forecastUrl(city.latitude, city.longitude);
    if (!submitNetwork(Kind::CityForecast, std::move(request),
        QStringLiteral("weather-city"), generation, city.latitude, city.longitude, false, city)) {
        setCityError(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                     QStringLiteral("Weather for this city is unavailable right now."));
    }
}

void WeatherController::beginComparisonForecast(const domain::WeatherCity &city,
                                                quint64 generation)
{
    const QString identity = domain::stableWeatherCityIdentity(city);
    if (identity.isEmpty() || !domain::isValidWeatherCoordinates(city.latitude, city.longitude)) {
        m_comparisonCities.setError(identity,
            QStringLiteral("The city coordinates are invalid."));
        return;
    }
    providers::ProviderRequest request;
    request.provider = providers::ProviderId::OpenMeteoForecast;
    request.url = forecastUrl(city.latitude, city.longitude);
    const QString freshnessKey = comparisonFreshnessKey(identity);
    if (!submitNetwork(Kind::ComparisonForecast, std::move(request), freshnessKey,
                       generation, city.latitude, city.longitude, false, city)) {
        m_comparisonCities.setError(identity,
            QStringLiteral("Weather for this city is unavailable right now."));
    }
}

void WeatherController::savePrimaryForecast(const domain::WeatherForecast &forecast,
                                            double latitude, double longitude,
                                            bool saveLocation, quint64 generation)
{
    const QString updatedAt = m_clock->nowUtc().toUTC().toString(Qt::ISODateWithMs);
    const storage::StorageCommand command = saveLocation
        ? services::WeatherService::savePrimaryLocation(latitude, longitude, forecast, updatedAt)
        : services::WeatherService::saveRefreshedForecast(forecast, updatedAt);
    const quint64 id = submitStorage(Kind::PrimarySave, command,
        QStringLiteral("weather-primary"), generation);
    if (id) {
        m_pending[id].latitude = latitude;
        m_pending[id].longitude = longitude;
        m_pending[id].saveLocation = saveLocation;
        m_pending[id].updatedAt = updatedAt;
        m_stagedPrimary.insert(id, forecast);
    } else {
        setPrimaryError(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                        QStringLiteral("The refreshed forecast could not be saved."));
    }
}

void WeatherController::onStorageFinished(quint64 id, const QVariantMap &values,
                                          domain::AppError error)
{
    const auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end()) return;
    const Pending pending = pendingIt.value();
    m_pending.erase(pendingIt);
    if (pending.kind == Kind::LoadComparison) {
        m_comparisonLoaded = true;
        if (error.isError()) {
            m_comparisonStorageError = safeMessage(
                error, QStringLiteral("Saved comparison cities could not be loaded."));
            emit comparisonChanged();
            updateComparisonLocalTimes();
            refreshAllComparisonCities();
            return;
        }
        m_comparisonCities.clear();
        for (const QVariant &value : values.value(QStringLiteral("cities")).toList()) {
            const QVariantMap saved = value.toMap();
            domain::WeatherCity city = weatherCityFromMap(saved);
            if (!domain::isValidWeatherCoordinates(city.latitude, city.longitude)
                || saved.value(QStringLiteral("identity")).toString()
                    != domain::stableWeatherCityIdentity(city)) {
                continue;
            }
            domain::WeatherForecast forecast;
            QString lastSuccessfulRefresh;
            const QString forecastJson = saved.value(QStringLiteral("forecastJson")).toString();
            const QString updatedAt = saved.value(QStringLiteral("updatedAt")).toString();
            if (!forecastJson.isEmpty()) {
                const auto cached = providers::parseWeatherForecast(forecastJson.toUtf8());
                if (cached.succeeded()) {
                    forecast = cached.value;
                    QDateTime cacheTime = QDateTime::fromString(updatedAt, Qt::ISODateWithMs);
                    if (!cacheTime.isValid())
                        cacheTime = QDateTime::fromString(updatedAt, Qt::ISODate);
                    if (cacheTime.isValid()) lastSuccessfulRefresh = updatedAt;
                }
            }
            m_comparisonCities.addCity(std::move(city), std::move(forecast),
                                       std::move(lastSuccessfulRefresh));
        }
        m_comparisonStorageError.clear();
        emit comparisonChanged();
        reconcileComparisonDate();
        updateComparisonLocalTimes();
        refreshAllComparisonCities();
        return;
    }
    if (pending.kind == Kind::SaveComparisonForecast) {
        if (m_comparisonCities.indexOfIdentity(pending.identity) < 0) return;
        if (error.isError()) {
            m_comparisonStorageError = safeMessage(
                error, QStringLiteral("A comparison forecast could not be cached."));
        } else {
            m_comparisonStorageError.clear();
        }
        emit comparisonChanged();
        return;
    }
    if (pending.kind == Kind::AddComparisonCity
        || pending.kind == Kind::RemoveComparisonCity
        || pending.kind == Kind::ReplaceComparisonCity
        || pending.kind == Kind::ReorderComparisonCities) {
        m_comparisonMutationPending = false;
        if (error.isError()) {
            m_comparisonStorageError = safeMessage(
                error, QStringLiteral("Saved comparison changes could not be completed."));
            emit comparisonChanged();
            return;
        }
        m_comparisonStorageError.clear();
        if (pending.kind == Kind::AddComparisonCity) {
            if (!m_comparisonCities.addCity(pending.city, pending.forecast, pending.updatedAt))
                m_comparisonStorageError = QStringLiteral("The saved city could not be restored into comparison.");
            reconcileComparisonDate();
            updateComparisonLocalTimes();
            emit cityChanged();
        } else if (pending.kind == Kind::ReplaceComparisonCity) {
            if (!m_comparisonCities.replaceCity(pending.identity, pending.city,
                                                pending.forecast, pending.updatedAt)) {
                m_comparisonStorageError = QStringLiteral("The replacement could not be restored into comparison.");
            }
            reconcileComparisonDate();
            updateComparisonLocalTimes();
        } else if (pending.kind == Kind::RemoveComparisonCity) {
            m_comparisonCities.removeCity(pending.identity);
            reconcileComparisonDate();
        } else {
            m_comparisonCities.moveCity(pending.identity, pending.destinationRow);
        }
        emit comparisonChanged();
        return;
    }
    if (pending.kind == Kind::LoadPrimary) {
        if (pending.generation != m_primaryGeneration) return;
        if (error.isError()) {
            m_primaryLoading = false;
            m_primaryRefreshing = false;
            setPrimaryError(error, QStringLiteral("Saved weather could not be loaded."));
            return;
        }
        if (!values.value(QStringLiteral("found")).toBool()) {
            m_hasLocation = false;
            if (m_locationAvailable) requestNativeLocation(pending.generation);
            else {
                m_primaryLoading = false;
                m_primaryRefreshing = false;
                m_primaryStatus.clear();
                m_primaryError = QStringLiteral("Device location is unavailable. You can still search for a city.");
                emit primaryChanged();
            }
            return;
        }
        const double latitude = values.value(QStringLiteral("latitude")).toDouble();
        const double longitude = values.value(QStringLiteral("longitude")).toDouble();
        if (!domain::isValidWeatherCoordinates(latitude, longitude)) {
            m_primaryLoading = false;
            m_primaryRefreshing = false;
            setPrimaryError(domain::AppError(domain::ErrorCategory::Storage),
                            QStringLiteral("Saved weather coordinates are invalid."));
            return;
        }
        m_hasLocation = true;
        m_primaryLatitude = latitude;
        m_primaryLongitude = longitude;
        const QString forecastJson = values.value(QStringLiteral("forecastJson")).toString();
        const auto cached = providers::parseWeatherForecast(forecastJson.toUtf8());
        if (cached.succeeded()) {
            publishPrimary(cached.value, values.value(QStringLiteral("updatedAt")).toString(), true);
        } else {
            m_primaryLocation = QStringLiteral("Your location");
            m_primaryUpdatedAt.clear();
            m_primaryStatus = QStringLiteral("Saved location found; forecast cache is unavailable.");
            m_primaryForecast.clear();
            m_primarySolarForecast = {};
            m_primarySolar.clear();
            m_primarySelectedDate.clear();
            m_primaryCurrent.clear();
            m_primaryToday.clear();
        }
        m_primaryError.clear();
        m_primaryLoading = false;
        m_primaryRefreshing = true;
        emit primaryChanged();
        beginPrimaryForecast(latitude, longitude, false, pending.generation);
        return;
    }
    if (pending.kind != Kind::PrimarySave || pending.generation != m_primaryGeneration) return;
    const domain::WeatherForecast forecast = m_stagedPrimary.take(id);
    m_primaryLoading = false;
    m_primaryRefreshing = false;
    if (error.isError()) {
        setPrimaryError(error, QStringLiteral("The forecast could not be saved."));
        return;
    }
    m_hasLocation = true;
    m_primaryLatitude = pending.latitude;
    m_primaryLongitude = pending.longitude;
    publishPrimary(forecast, pending.updatedAt, false);
    m_primaryError.clear();
    m_primaryStatus = QStringLiteral("Updated %1").arg(updatedAtText(m_primaryUpdatedAt));
    emit primaryChanged();
}

void WeatherController::onHttpsFinished(quint64 id, const QByteArray &body,
                                        domain::AppError error)
{
    const auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end()) return;
    const Pending pending = pendingIt.value();
    m_pending.erase(pendingIt);
    if (pending.kind == Kind::ComparisonForecast) {
        const QString identity = domain::stableWeatherCityIdentity(pending.city);
        if (pending.generation != m_comparisonGenerations.value(identity)
            || m_comparisonCities.indexOfIdentity(identity) < 0) return;
        if (error.isError()) {
            m_comparisonCities.setError(identity,
                safeMessage(error, QStringLiteral("The city forecast could not be loaded.")));
            return;
        }
        const quint64 parseId = submitParse(Kind::ComparisonForecastParse, body,
            comparisonFreshnessKey(identity), pending.generation,
            pending.latitude, pending.longitude, false, pending.city);
        if (!parseId) m_comparisonCities.setError(identity,
            QStringLiteral("The city forecast could not be processed."));
        return;
    }
    if (pending.kind == Kind::CityGeocode || pending.kind == Kind::CityGeocodeParse
        || pending.kind == Kind::CityForecast || pending.kind == Kind::CityForecastParse) {
        if (pending.generation != m_cityGeneration) return;
        if (error.isError()) {
            setCityError(error, QStringLiteral("The city forecast could not be loaded."));
            return;
        }
        const Kind parseKind = pending.kind == Kind::CityGeocode
            ? Kind::CityGeocodeParse : Kind::CityForecastParse;
        const quint64 parseId = submitParse(parseKind, body, QStringLiteral("weather-city"),
                    pending.generation, pending.latitude, pending.longitude,
                    pending.saveLocation, pending.city);
        if (!parseId) setCityError(
            domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
            QStringLiteral("The city forecast could not be processed."));
        return;
    }
    if (pending.generation != m_primaryGeneration) return;
    if (error.isError()) {
        setPrimaryError(error, QStringLiteral("The forecast could not be refreshed."));
        return;
    }
    const quint64 parseId = submitParse(Kind::PrimaryParse, body,
                QStringLiteral("weather-primary"), pending.generation,
                pending.latitude, pending.longitude, pending.saveLocation);
    if (!parseId) setPrimaryError(
        domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
        QStringLiteral("The refreshed forecast could not be processed."));
}

void WeatherController::onValueJobFinished(quint64 id, const QVariantMap &values,
                                            domain::AppError error)
{
    const auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end()) return;
    const Pending pending = pendingIt.value();
    m_pending.erase(pendingIt);
    if (pending.kind == Kind::CityGeocodeParse) {
        if (pending.generation != m_cityGeneration) return;
        if (error.isError()) {
            setCityError(error, QStringLiteral("City search could not be completed."));
            return;
        }
        m_cityCandidates.clear();
        for (const QVariant &value : values.value(QStringLiteral("candidates")).toList())
            m_cityCandidates.push_back(weatherCityFromMap(value.toMap()));
        if (m_cityCandidates.isEmpty()) {
            m_citySearching = false;
            m_cityError = QStringLiteral("No matching city was found.");
            emit cityChanged();
            emit comparisonChanged();
            return;
        }
        if (m_cityCandidates.size() == 1) {
            selectCityCandidate(0);
            return;
        }
        m_citySearching = false;
        m_cityError.clear();
        emit cityChanged();
        emit comparisonChanged();
        return;
    }
    if (pending.kind == Kind::CityForecastParse) {
        if (pending.generation != m_cityGeneration) return;
        if (error.isError()) {
            setCityError(error, QStringLiteral("Weather for this city could not be loaded."));
            return;
        }
        const domain::WeatherForecast forecast = weatherForecastFromValues(values);
        publishCity(pending.city, forecast);
        return;
    }
    if (pending.kind == Kind::ComparisonForecastParse) {
        const QString identity = domain::stableWeatherCityIdentity(pending.city);
        if (pending.generation != m_comparisonGenerations.value(identity)
            || m_comparisonCities.indexOfIdentity(identity) < 0) return;
        if (error.isError()) {
            m_comparisonCities.setError(identity,
                safeMessage(error, QStringLiteral("The forecast could not be refreshed.")));
            return;
        }
        const domain::WeatherForecast forecast = weatherForecastFromValues(values);
        const QString updatedAt = m_clock->nowUtc().toUTC().toString(Qt::ISODateWithMs);
        if (!m_comparisonCities.setForecast(identity, forecast, updatedAt)) {
            m_comparisonCities.setError(identity,
                QStringLiteral("The refreshed forecast could not be processed."));
            return;
        }
        reconcileComparisonDate();
        updateComparisonLocalTimes();
        const quint64 cacheId = submitStorage(Kind::SaveComparisonForecast,
            services::WeatherService::saveComparisonForecast(identity, forecast, updatedAt),
            QStringLiteral("weather-comparison-cache:%1").arg(identity), pending.generation);
        if (!cacheId) {
            m_comparisonStorageError = QStringLiteral("A comparison forecast could not be cached.");
            emit comparisonChanged();
        } else {
            m_pending[cacheId].identity = identity;
            m_pending[cacheId].updatedAt = updatedAt;
        }
        return;
    }
    if (pending.kind != Kind::PrimaryParse || pending.generation != m_primaryGeneration) return;
    if (error.isError()) {
        setPrimaryError(error, QStringLiteral("The forecast could not be refreshed."));
        return;
    }
    const domain::WeatherForecast forecast = weatherForecastFromValues(values);
    savePrimaryForecast(forecast, pending.latitude, pending.longitude,
                        pending.saveLocation, pending.generation);
}

void WeatherController::onNativeLocationReady(quint64 id, double latitude, double longitude,
                                               domain::AppError error)
{
    const auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end() || pendingIt->kind != Kind::NativeLocation) return;
    const Pending pending = pendingIt.value();
    m_pending.erase(pendingIt);
    m_locationOperation = 0;
    m_graph->completeOperation({id}, error);
    if (pending.generation != m_primaryGeneration) return;
    if (error.isCancelled()) return;
    if (error.isError()) {
        if (error.category() == domain::ErrorCategory::CapabilityUnavailable
            || error.category() == domain::ErrorCategory::Platform) {
            m_locationAvailable = false;
            m_graph->reportCapabilityUnavailable(QStringLiteral("native-location"), error);
        }
        m_primaryLoading = false;
        m_primaryRefreshing = false;
        m_primaryError = error.category() == domain::ErrorCategory::Timeout
            ? QStringLiteral("Finding device location took too long. You can still search for a city.")
            : QStringLiteral("Device location is unavailable. You can still search for a city.");
        emit primaryChanged();
        return;
    }
    if (!domain::isValidWeatherCoordinates(latitude, longitude)) {
        m_primaryLoading = false;
        m_primaryRefreshing = false;
        m_primaryError = QStringLiteral("The device returned invalid coordinates.");
        emit primaryChanged();
        return;
    }
    beginPrimaryForecast(latitude, longitude, true, pending.generation);
}

void WeatherController::publishPrimary(const domain::WeatherForecast &forecast,
                                       QString updatedAt, bool fromCache)
{
    m_primarySolarForecast = forecast;
    m_primarySelectedDate = availableForecastDate(forecast, m_primarySelectedDate);
    m_primarySolar = solarInfoForForecast(forecast, m_primarySelectedDate, m_clock->nowUtc());
    m_primaryForecast.replace(forecast.days, forecast.timezone);
    m_primaryCurrent = currentMap(forecast.current);
    m_primaryToday.clear();
    if (!forecast.days.isEmpty()) {
        const domain::WeatherDay &today = forecast.days.first();
        m_primaryToday = {{QStringLiteral("date"), today.date},
                          {QStringLiteral("condition"), models::WeatherForecastModel::conditionForCode(today.weatherCode)},
                          {QStringLiteral("icon"), models::WeatherForecastModel::iconForCode(today.weatherCode)},
                          {QStringLiteral("maximum"), today.maximumCelsius},
                          {QStringLiteral("minimum"), today.minimumCelsius},
                          {QStringLiteral("precipitation"), today.precipitationProbability}};
    }
    m_primaryLocation = forecastLocation(forecast);
    m_primaryUpdatedAt = updatedAtText(updatedAt);
    if (fromCache) {
        m_primaryStatus = m_primaryUpdatedAt.isEmpty()
            ? QStringLiteral("Showing saved forecast · update time unavailable")
            : QStringLiteral("Showing saved forecast · updated %1").arg(m_primaryUpdatedAt);
    } else {
        m_primaryStatus = m_primaryUpdatedAt.isEmpty()
            ? QStringLiteral("Forecast updated")
            : QStringLiteral("Updated %1").arg(m_primaryUpdatedAt);
    }
    emit primaryChanged();
}

void WeatherController::publishCity(const domain::WeatherCity &city,
                                    const domain::WeatherForecast &forecast)
{
    m_citySolarForecast = forecast;
    m_searchedCity = city;
    if (!forecast.timezone.isEmpty()) m_searchedCity.timezone = forecast.timezone;
    m_hasSearchedCity = true;
    m_cityUpdatedAtUtc = m_clock->nowUtc().toUTC().toString(Qt::ISODateWithMs);
    m_cityUpdatedAt = updatedAtText(m_cityUpdatedAtUtc);
    m_citySelectedDate = availableForecastDate(forecast, m_citySelectedDate);
    m_citySolar = solarInfoForForecast(forecast, m_citySelectedDate, m_clock->nowUtc());
    m_cityForecast.replace(forecast.days, forecast.timezone);
    m_cityCurrent = currentMap(forecast.current);
    m_cityLocation = cityLabel(city);
    m_cityError.clear();
    m_citySearching = false;
    emit cityChanged();
    emit comparisonChanged();
}

void WeatherController::updateSolarInfo()
{
    updateComparisonLocalTimes();
    const QDateTime nowUtc = m_clock->nowUtc();
    const QVariantMap primary = solarInfoForForecast(
        m_primarySolarForecast, m_primarySelectedDate, nowUtc);
    const QVariantMap city = solarInfoForForecast(m_citySolarForecast, m_citySelectedDate, nowUtc);
    bool primaryChangedValue = false;
    bool cityChangedValue = false;
    if (primary != m_primarySolar) {
        m_primarySolar = primary;
        primaryChangedValue = true;
    }
    if (city != m_citySolar) {
        m_citySolar = city;
        cityChangedValue = true;
    }
    if (primaryChangedValue) emit primaryChanged();
    if (cityChangedValue) emit cityChanged();
}

void WeatherController::updateComparisonLocalTimes()
{
    if (!m_clock) return;
    const QDateTime nowUtc = m_clock->nowUtc().toUTC();
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        const QModelIndex index = m_comparisonCities.index(row);
        const QString identity = m_comparisonCities.data(
            index, models::WeatherComparisonModel::IdentityRole).toString();
        const auto entry = m_comparisonCities.cityByIdentity(identity);
        if (!entry) continue;
        const QString timezone = entry->forecast.timezone.isEmpty()
            ? entry->city.timezone : entry->forecast.timezone;
        const QTimeZone timeZone(timezone.toUtf8());
        const QString localTime = timeZone.isValid()
            ? QLocale().toString(nowUtc.toTimeZone(timeZone), QLocale::ShortFormat)
            : QString{};
        m_comparisonCities.setLocalDateTime(identity, localTime);
    }
}

void WeatherController::reconcileComparisonDate()
{
    const QVariantList options = comparisonDateOptions();
    QString selectedDate;
    if (options.isEmpty()) m_comparisonDateUserSelected = false;
    if (!options.isEmpty() && m_comparisonDateUserSelected) {
        const auto selected = std::find_if(options.cbegin(), options.cend(),
            [this](const QVariant &value) {
                const QVariantMap option = value.toMap();
                return option.value(QStringLiteral("date")).toString()
                    == m_comparisonSelectedDate
                    && option.value(QStringLiteral("availableCount")).toInt() > 0;
            });
        if (selected != options.cend()) selectedDate = m_comparisonSelectedDate;
        else m_comparisonDateUserSelected = false;
    }

    if (selectedDate.isEmpty() && !options.isEmpty()) {
        // Provider dates are ISO calendar dates. Prefer the earliest date shared
        // by forecast-bearing cities; otherwise maximize coverage without substituting per city.
        const auto common = std::find_if(options.cbegin(), options.cend(),
            [](const QVariant &value) { return value.toMap().value(QStringLiteral("common")).toBool(); });
        if (common != options.cend()) {
            selectedDate = common->toMap().value(QStringLiteral("date")).toString();
        } else {
            int bestAvailability = -1;
            for (const QVariant &value : options)
                bestAvailability = std::max(bestAvailability,
                    value.toMap().value(QStringLiteral("availableCount")).toInt());
            const auto current = std::find_if(options.cbegin(), options.cend(),
                [this, bestAvailability](const QVariant &value) {
                    const QVariantMap option = value.toMap();
                    return option.value(QStringLiteral("date")).toString()
                            == m_comparisonSelectedDate
                        && option.value(QStringLiteral("availableCount")).toInt()
                            == bestAvailability;
                });
            const auto best = current != options.cend() ? current
                : std::find_if(options.cbegin(), options.cend(),
                    [bestAvailability](const QVariant &value) {
                        return value.toMap().value(QStringLiteral("availableCount")).toInt()
                            == bestAvailability;
                    });
            if (best != options.cend())
                selectedDate = best->toMap().value(QStringLiteral("date")).toString();
        }
    }

    const bool selectedDateChanged = m_comparisonSelectedDate != selectedDate;
    m_comparisonSelectedDate = selectedDate;
    bool cityDateChanged = false;
    for (int row = 0; row < m_comparisonCities.count(); ++row) {
        const QString identity = m_comparisonCities.data(
            m_comparisonCities.index(row), models::WeatherComparisonModel::IdentityRole).toString();
        const auto entry = m_comparisonCities.cityByIdentity(identity);
        if (!entry) continue;
        const bool available = std::any_of(entry->forecast.days.cbegin(),
            entry->forecast.days.cend(), [&selectedDate](const domain::WeatherDay &day) {
                return day.date == selectedDate;
            });
        cityDateChanged = m_comparisonCities.selectForecastDate(
            identity, available ? selectedDate : QString{}) || cityDateChanged;
    }
    if (selectedDateChanged || cityDateChanged) emit comparisonChanged();
}

void WeatherController::selectPrimaryForecastDate(QString date)
{
    if (date == m_primarySelectedDate) return;
    bool found = false;
    for (const domain::WeatherDay &day : m_primarySolarForecast.days) {
        if (day.date == date) {
            found = true;
            break;
        }
    }
    if (!found) return;
    m_primarySelectedDate = std::move(date);
    m_primarySolar = solarInfoForForecast(
        m_primarySolarForecast, m_primarySelectedDate, m_clock->nowUtc());
    emit primaryChanged();
}

void WeatherController::selectCityForecastDate(QString date)
{
    if (date == m_citySelectedDate) return;
    bool found = false;
    for (const domain::WeatherDay &day : m_citySolarForecast.days) {
        if (day.date == date) {
            found = true;
            break;
        }
    }
    if (!found) return;
    m_citySelectedDate = std::move(date);
    m_citySolar = solarInfoForForecast(
        m_citySolarForecast, m_citySelectedDate, m_clock->nowUtc());
    emit cityChanged();
}

void WeatherController::setPrimaryError(domain::AppError error, const QString &fallback)
{
    m_primaryLoading = false;
    m_primaryRefreshing = false;
    m_primaryError = safeMessage(error, fallback);
    if (m_primaryForecast.count() > 0) {
        m_primaryStatus = QStringLiteral("Refresh failed; showing saved forecast from %1")
            .arg(m_primaryUpdatedAt.isEmpty() ? QStringLiteral("an earlier update") : m_primaryUpdatedAt);
    }
    emit primaryChanged();
}

void WeatherController::setCityError(domain::AppError error, const QString &fallback)
{
    m_citySearching = false;
    m_cityError = safeMessage(error, fallback);
    m_cityForecast.clear();
    m_citySolarForecast = {};
    m_citySelectedDate.clear();
    m_cityCurrent.clear();
    m_citySolar.clear();
    m_cityLocation.clear();
    m_cityUpdatedAt.clear();
    m_cityUpdatedAtUtc.clear();
    m_cityCandidates.clear();
    emit cityChanged();
    emit comparisonChanged();
}

} // namespace something::controllers
