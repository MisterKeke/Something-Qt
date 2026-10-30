#include "weather_controller.h"

#include "../providers/feature_parsers.h"
#include "../services/weather_service.h"

#include <QDateTime>
#include <QLocale>
#include <QThread>
#include <QUrlQuery>
#include <QTimeZone>

#include <algorithm>
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
        }
        return;
    }
    if (state == application::ApplicationLifecycle::State::ShuttingDown
        || state == application::ApplicationLifecycle::State::Failed
        || state == application::ApplicationLifecycle::State::Stopped) {
        m_ready = false;
        cancelPrimaryOperations();
        cancelCityOperations();
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
            && it->kind != Kind::CityForecastParse) cancelPending(id);
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
    return m_hasSearchedCity && !m_citySolarForecast.days.isEmpty()
        && m_comparisonCities.count() < models::WeatherComparisonModel::maximumCities()
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

bool WeatherController::addSearchedCityToComparison()
{
    if (!canAddSearchedCity()) return false;
    const bool added = m_comparisonCities.addCity(
        m_searchedCity, m_citySolarForecast, m_cityUpdatedAtUtc);
    if (added) {
        emit comparisonChanged();
        emit cityChanged();
    }
    return added;
}

void WeatherController::removeComparisonCity(QString identity)
{
    m_comparisonCities.removeCity(identity);
}

void WeatherController::moveComparisonCity(QString identity, int offset)
{
    if (offset != -1 && offset != 1) return;
    const int sourceRow = m_comparisonCities.indexOfIdentity(identity);
    if (sourceRow < 0) return;
    const int destinationRow = sourceRow + offset;
    if (destinationRow < 0 || destinationRow >= m_comparisonCities.count()) return;
    m_comparisonCities.moveCity(identity, destinationRow);
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
        for (const QVariant &value : values.value(QStringLiteral("days")).toList()) {
            forecast.days.push_back(weatherDayFromMap(value.toMap()));
        }
        publishCity(pending.city, forecast);
        return;
    }
    if (pending.kind != Kind::PrimaryParse || pending.generation != m_primaryGeneration) return;
    if (error.isError()) {
        setPrimaryError(error, QStringLiteral("The forecast could not be refreshed."));
        return;
    }
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
    for (const QVariant &value : values.value(QStringLiteral("days")).toList()) {
        forecast.days.push_back(weatherDayFromMap(value.toMap()));
    }
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
