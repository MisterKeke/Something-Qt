#pragma once

#include "../domain/weather_currency.h"

#include <QAbstractListModel>

namespace something::models {

class WeatherForecastModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { DateRole = Qt::UserRole + 1, WeatherCodeRole, ConditionRole,
                IconRole, MaximumRole, MinimumRole, PrecipitationRole };
    Q_ENUM(Role)

    explicit WeatherForecastModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_days.size(); }
    void replace(QVector<domain::WeatherDay> days);
    void clear();

    static QString conditionForCode(int code);
    static QString iconForCode(int code);

signals:
    void countChanged();

private:
    QVector<domain::WeatherDay> m_days;
};

} // namespace something::models
