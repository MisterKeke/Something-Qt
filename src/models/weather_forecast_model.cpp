#include "weather_forecast_model.h"

#include <QLocale>

#include <cmath>
#include <utility>

namespace something::models {

WeatherForecastModel::WeatherForecastModel(QObject *parent) : QAbstractListModel(parent) {}

int WeatherForecastModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_days.size();
}

QVariant WeatherForecastModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_days.size()) return {};
    const domain::WeatherDay &day = m_days.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case DateRole: return day.date;
    case WeatherCodeRole: return day.weatherCode;
    case ConditionRole: return conditionForCode(day.weatherCode);
    case IconRole: return iconForCode(day.weatherCode);
    case MaximumRole: return day.maximumCelsius;
    case MinimumRole: return day.minimumCelsius;
    case PrecipitationRole: return day.precipitationProbability;
    case SunriseRole: return locationTimeText(day.sunriseUtc);
    case SunsetRole: return locationTimeText(day.sunsetUtc);
    case DaylightDurationRole:
        return day.daylightDurationSeconds ? QVariant(*day.daylightDurationSeconds) : QVariant{};
    case HasSunriseRole:
        return day.sunriseUtc && day.sunriseUtc->isValid() && m_locationTimeZone.isValid();
    case HasSunsetRole:
        return day.sunsetUtc && day.sunsetUtc->isValid() && m_locationTimeZone.isValid();
    case HasDaylightDurationRole:
        return day.daylightDurationSeconds && std::isfinite(*day.daylightDurationSeconds);
    default: return {};
    }
}

QHash<int, QByteArray> WeatherForecastModel::roleNames() const
{
    return {{DateRole, "date"}, {WeatherCodeRole, "weatherCode"},
            {ConditionRole, "condition"}, {IconRole, "icon"},
            {MaximumRole, "maximum"}, {MinimumRole, "minimum"},
            {PrecipitationRole, "precipitationProbability"},
            {SunriseRole, "sunrise"}, {SunsetRole, "sunset"},
            {DaylightDurationRole, "daylightDurationSeconds"},
            {HasSunriseRole, "hasSunrise"}, {HasSunsetRole, "hasSunset"},
            {HasDaylightDurationRole, "hasDaylightDuration"}};
}

void WeatherForecastModel::replace(QVector<domain::WeatherDay> days, QString timezone)
{
    const int previousCount = m_days.size();
    beginResetModel();
    m_days = std::move(days);
    m_locationTimeZone = QTimeZone(timezone.toUtf8());
    endResetModel();
    if (previousCount != m_days.size()) emit countChanged();
}

void WeatherForecastModel::clear()
{
    if (m_days.isEmpty()) return;
    beginResetModel();
    m_days.clear();
    m_locationTimeZone = QTimeZone();
    endResetModel();
    emit countChanged();
}

QString WeatherForecastModel::locationTimeText(
    const std::optional<QDateTime> &timestamp) const
{
    if (!timestamp || !timestamp->isValid() || !m_locationTimeZone.isValid()) return {};
    const QDateTime localTime = timestamp->toUTC().toTimeZone(m_locationTimeZone);
    return QLocale().toString(localTime.time(), QLocale::ShortFormat);
}

QString WeatherForecastModel::conditionForCode(int code)
{
    if (code == 0) return QStringLiteral("Clear sky");
    if (code == 1 || code == 2) return QStringLiteral("Partly cloudy");
    if (code == 3) return QStringLiteral("Overcast");
    if (code == 45 || code == 48) return QStringLiteral("Foggy");
    if (code >= 51 && code <= 55) return QStringLiteral("Drizzle");
    if (code == 56 || code == 57) return QStringLiteral("Freezing drizzle");
    if (code >= 61 && code <= 65) return QStringLiteral("Rain");
    if (code == 66 || code == 67) return QStringLiteral("Freezing rain");
    if ((code >= 71 && code <= 75) || code == 77) return QStringLiteral("Snow");
    if (code >= 80 && code <= 82) return QStringLiteral("Rain showers");
    if (code == 85 || code == 86) return QStringLiteral("Snow showers");
    if (code == 95) return QStringLiteral("Thunderstorm");
    if (code == 96 || code == 99) return QStringLiteral("Thunderstorm with hail");
    return QStringLiteral("Weather unavailable");
}

QString WeatherForecastModel::iconForCode(int code)
{
    if (code == 0) return QString::fromUtf8("☀️");
    if (code == 1 || code == 2) return QString::fromUtf8("🌤️");
    if (code == 3) return QString::fromUtf8("☁️");
    if (code == 45 || code == 48) return QString::fromUtf8("🌫️");
    if (code >= 51 && code <= 67) return QString::fromUtf8("🌧️");
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return QString::fromUtf8("❄️");
    if ((code >= 80 && code <= 82) || code == 95 || code == 96 || code == 99)
        return QString::fromUtf8("⛈️");
    return QString::fromUtf8("🌡️");
}

} // namespace something::models
