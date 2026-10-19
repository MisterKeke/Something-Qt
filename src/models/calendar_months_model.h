#pragma once

#include <QAbstractListModel>
#include <QDate>
#include <QVector>
#include <QVariantList>

namespace something::models {

class CalendarMonthsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { MonthNameRole = Qt::UserRole + 1, YearRole, DaysRole };
    Q_ENUM(Role)

    explicit CalendarMonthsModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_months.size(); }
    void setCalendar(int visibleYear, QDate today, QString selectedDate);

signals:
    void countChanged();

private:
    struct Month {
        QString name;
        int year{0};
        QVariantList days;
    };
    QVector<Month> m_months;
};

} // namespace something::models
