#pragma once

#include "../domain/weather_currency.h"

#include <QAbstractListModel>
#include <QVariantMap>
#include <QVector>

namespace something::models {

class WeatherComparisonModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdentityRole = Qt::UserRole + 1,
        GeocodingIdRole,
        NameRole,
        AdministrativeAreaRole,
        CountryRole,
        LatitudeRole,
        LongitudeRole,
        TimezoneRole,
        ForecastRole,
        LastSuccessfulRefreshRole,
        LoadingRole,
        ErrorRole,
        HasForecastRole,
    };
    Q_ENUM(Role)

    explicit WeatherComparisonModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_cities.size(); }

    static constexpr int maximumCities() noexcept { return 4; }

    int indexOfIdentity(const QString &identity) const noexcept;
    std::optional<domain::WeatherComparisonCity> cityByIdentity(
        const QString &identity) const;
    bool containsCity(const domain::WeatherCity &city) const noexcept;
    bool addCity(domain::WeatherCity city,
                 domain::WeatherForecast forecast = {},
                 QString lastSuccessfulRefresh = {},
                 bool loading = false);
    bool removeCity(const QString &identity);
    bool moveCity(const QString &identity, int destinationRow);
    bool setLoading(const QString &identity, bool loading);
    bool setError(const QString &identity, QString error);
    bool setForecast(const QString &identity, domain::WeatherForecast forecast,
                     QString lastSuccessfulRefresh);
    void clear();

signals:
    void countChanged();
    void contentsChanged();

private:
    QVariantMap forecastMap(const domain::WeatherForecast &forecast) const;
    void notifyRowChanged(int row, const QVector<int> &roles);

    QVector<domain::WeatherComparisonCity> m_cities;
};

} // namespace something::models
