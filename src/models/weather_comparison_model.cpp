#include "weather_comparison_model.h"

#include "weather_forecast_model.h"

#include <QVariantList>

#include <algorithm>
#include <utility>

namespace something::models {

WeatherComparisonModel::WeatherComparisonModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int WeatherComparisonModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_cities.size();
}

QVariant WeatherComparisonModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_cities.size()) return {};
    const domain::WeatherComparisonCity &entry = m_cities.at(index.row());
    const domain::WeatherCity &city = entry.city;
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return city.name;
    case IdentityRole: return domain::stableWeatherCityIdentity(city);
    case GeocodingIdRole: return city.geocodingId;
    case AdministrativeAreaRole: return city.administrativeArea;
    case CountryRole: return city.country;
    case LatitudeRole: return city.latitude;
    case LongitudeRole: return city.longitude;
    case TimezoneRole:
        return entry.forecast.timezone.isEmpty() ? city.timezone : entry.forecast.timezone;
    case ForecastRole: return forecastMap(entry.forecast);
    case LastSuccessfulRefreshRole: return entry.lastSuccessfulRefresh;
    case LoadingRole: return entry.loading;
    case ErrorRole: return entry.error;
    case HasForecastRole: return !entry.forecast.days.isEmpty();
    default: return {};
    }
}

QHash<int, QByteArray> WeatherComparisonModel::roleNames() const
{
    return {{IdentityRole, "identity"}, {GeocodingIdRole, "geocodingId"},
            {NameRole, "name"}, {AdministrativeAreaRole, "administrativeArea"},
            {CountryRole, "country"}, {LatitudeRole, "latitude"},
            {LongitudeRole, "longitude"}, {TimezoneRole, "timezone"},
            {ForecastRole, "forecast"},
            {LastSuccessfulRefreshRole, "lastSuccessfulRefresh"},
            {LoadingRole, "loading"}, {ErrorRole, "error"},
            {HasForecastRole, "hasForecast"}};
}

int WeatherComparisonModel::indexOfIdentity(const QString &identity) const noexcept
{
    if (identity.isEmpty()) return -1;
    for (qsizetype row = 0; row < m_cities.size(); ++row) {
        if (domain::stableWeatherCityIdentity(m_cities.at(row).city) == identity)
            return static_cast<int>(row);
    }
    return -1;
}

bool WeatherComparisonModel::containsCity(const domain::WeatherCity &city) const noexcept
{
    return std::any_of(m_cities.cbegin(), m_cities.cend(),
        [&city](const domain::WeatherComparisonCity &entry) {
            return domain::isSameWeatherLocation(entry.city, city);
        });
}

bool WeatherComparisonModel::addCity(domain::WeatherCity city,
                                     domain::WeatherForecast forecast,
                                     QString lastSuccessfulRefresh,
                                     bool loading)
{
    city.name = city.name.trimmed();
    if (m_cities.size() >= maximumCities() || city.name.isEmpty()
        || !domain::isValidWeatherCoordinates(city.latitude, city.longitude)
        || domain::stableWeatherCityIdentity(city).isEmpty() || containsCity(city)) {
        return false;
    }
    if (!forecast.timezone.isEmpty()) city.timezone = forecast.timezone;

    const int row = m_cities.size();
    beginInsertRows({}, row, row);
    m_cities.push_back(domain::WeatherComparisonCity{
        std::move(city), std::move(forecast), std::move(lastSuccessfulRefresh), {}, loading});
    endInsertRows();
    emit countChanged();
    emit contentsChanged();
    return true;
}

bool WeatherComparisonModel::removeCity(const QString &identity)
{
    const int row = indexOfIdentity(identity);
    if (row < 0) return false;
    beginRemoveRows({}, row, row);
    m_cities.removeAt(row);
    endRemoveRows();
    emit countChanged();
    emit contentsChanged();
    return true;
}

bool WeatherComparisonModel::moveCity(const QString &identity, int destinationRow)
{
    const int sourceRow = indexOfIdentity(identity);
    if (sourceRow < 0 || destinationRow < 0 || destinationRow >= m_cities.size()
        || sourceRow == destinationRow) return false;
    const int destinationChild = destinationRow > sourceRow
        ? destinationRow + 1 : destinationRow;
    if (!beginMoveRows({}, sourceRow, sourceRow, {}, destinationChild)) return false;
    m_cities.move(sourceRow, destinationRow);
    endMoveRows();
    emit contentsChanged();
    return true;
}

bool WeatherComparisonModel::setLoading(const QString &identity, bool loading)
{
    const int row = indexOfIdentity(identity);
    if (row < 0) return false;
    domain::WeatherComparisonCity &entry = m_cities[row];
    if (entry.loading == loading && (!loading || entry.error.isEmpty())) return true;
    entry.loading = loading;
    if (loading) entry.error.clear();
    notifyRowChanged(row, {LoadingRole, ErrorRole});
    emit contentsChanged();
    return true;
}

bool WeatherComparisonModel::setError(const QString &identity, QString error)
{
    const int row = indexOfIdentity(identity);
    if (row < 0) return false;
    domain::WeatherComparisonCity &entry = m_cities[row];
    entry.loading = false;
    entry.error = std::move(error);
    notifyRowChanged(row, {LoadingRole, ErrorRole});
    emit contentsChanged();
    return true;
}

bool WeatherComparisonModel::setForecast(const QString &identity,
                                         domain::WeatherForecast forecast,
                                         QString lastSuccessfulRefresh)
{
    const int row = indexOfIdentity(identity);
    if (row < 0 || forecast.days.isEmpty()) return false;
    domain::WeatherComparisonCity &entry = m_cities[row];
    if (!forecast.timezone.isEmpty()) entry.city.timezone = forecast.timezone;
    entry.forecast = std::move(forecast);
    if (!lastSuccessfulRefresh.isEmpty())
        entry.lastSuccessfulRefresh = std::move(lastSuccessfulRefresh);
    entry.loading = false;
    entry.error.clear();
    notifyRowChanged(row, {TimezoneRole, ForecastRole, LastSuccessfulRefreshRole,
                           LoadingRole, ErrorRole, HasForecastRole});
    emit contentsChanged();
    return true;
}

void WeatherComparisonModel::clear()
{
    if (m_cities.isEmpty()) return;
    beginResetModel();
    m_cities.clear();
    endResetModel();
    emit countChanged();
    emit contentsChanged();
}

QVariantMap WeatherComparisonModel::forecastMap(
    const domain::WeatherForecast &forecast) const
{
    QVariantMap current;
    if (forecast.current.temperatureCelsius) {
        current.insert(QStringLiteral("temperature"), *forecast.current.temperatureCelsius);
        current.insert(QStringLiteral("hasTemperature"), true);
    }
    if (forecast.current.apparentTemperatureCelsius) {
        current.insert(QStringLiteral("feelsLike"), *forecast.current.apparentTemperatureCelsius);
        current.insert(QStringLiteral("hasFeelsLike"), true);
    }
    if (forecast.current.weatherCode) {
        current.insert(QStringLiteral("weatherCode"), *forecast.current.weatherCode);
        current.insert(QStringLiteral("condition"),
                       WeatherForecastModel::conditionForCode(*forecast.current.weatherCode));
        current.insert(QStringLiteral("icon"),
                       WeatherForecastModel::iconForCode(*forecast.current.weatherCode));
        current.insert(QStringLiteral("hasWeather"), true);
    }
    if (forecast.current.windKmh) {
        current.insert(QStringLiteral("wind"), *forecast.current.windKmh);
        current.insert(QStringLiteral("hasWind"), true);
    }

    QVariantList days;
    days.reserve(forecast.days.size());
    for (const domain::WeatherDay &day : forecast.days) {
        QVariantMap value{
            {QStringLiteral("date"), day.date},
            {QStringLiteral("weatherCode"), day.weatherCode},
            {QStringLiteral("condition"), WeatherForecastModel::conditionForCode(day.weatherCode)},
            {QStringLiteral("icon"), WeatherForecastModel::iconForCode(day.weatherCode)},
            {QStringLiteral("maximum"), day.maximumCelsius},
            {QStringLiteral("minimum"), day.minimumCelsius},
            {QStringLiteral("precipitationProbability"), day.precipitationProbability},
        };
        if (day.sunriseUtc && day.sunriseUtc->isValid())
            value.insert(QStringLiteral("sunriseUtc"), *day.sunriseUtc);
        if (day.sunsetUtc && day.sunsetUtc->isValid())
            value.insert(QStringLiteral("sunsetUtc"), *day.sunsetUtc);
        if (day.daylightDurationSeconds)
            value.insert(QStringLiteral("daylightDurationSeconds"),
                         *day.daylightDurationSeconds);
        days.push_back(std::move(value));
    }
    return {{QStringLiteral("current"), current},
            {QStringLiteral("days"), days},
            {QStringLiteral("timezone"), forecast.timezone}};
}

void WeatherComparisonModel::notifyRowChanged(int row, const QVector<int> &roles)
{
    const QModelIndex changed = index(row, 0);
    emit dataChanged(changed, changed, roles);
}

} // namespace something::models
