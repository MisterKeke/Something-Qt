#include "calendar_months_model.h"

#include <QLocale>
#include <QVariantMap>

namespace something::models {

CalendarMonthsModel::CalendarMonthsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int CalendarMonthsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_months.size();
}

QVariant CalendarMonthsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_months.size()) return {};
    const Month &month = m_months.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case MonthNameRole: return month.name;
    case YearRole: return month.year;
    case DaysRole: return month.days;
    default: return {};
    }
}

QHash<int, QByteArray> CalendarMonthsModel::roleNames() const
{
    return {{MonthNameRole, "monthName"}, {YearRole, "year"}, {DaysRole, "days"}};
}

void CalendarMonthsModel::setCalendar(int visibleYear, QDate today, QString selectedDate,
                                      QVariantMap markersByDate)
{
    const int previousCount = m_months.size();
    beginResetModel();
    m_months.clear();
    if (visibleYear > 0) {
        const int firstMonth = today.isValid() && visibleYear == today.year() ? today.month() : 1;
        const QLocale locale;
        for (int monthNumber = firstMonth; monthNumber <= 12; ++monthNumber) {
            const QDate first(visibleYear, monthNumber, 1);
            const int daysInMonth = first.daysInMonth();
            const int leadingBlanks = first.dayOfWeek() - 1; // fixed Monday-first grid
            QVariantList days;
            days.reserve(42);
            for (int cell = 0; cell < 42; ++cell) {
                const int dayNumber = cell - leadingBlanks + 1;
                if (dayNumber < 1 || dayNumber > daysInMonth) {
                    days.push_back(QVariantMap{{QStringLiteral("day"), 0},
                                               {QStringLiteral("date"), QString()},
                                               {QStringLiteral("today"), false},
                                               {QStringLiteral("selected"), false},
                                               {QStringLiteral("weekend"), false}});
                    continue;
                }
                const QDate date(visibleYear, monthNumber, dayNumber);
                const QString key = date.toString(Qt::ISODate);
                const int dayOfWeek = date.dayOfWeek();
                const QVariantMap markers = markersByDate.value(key).toMap();
                days.push_back(QVariantMap{{QStringLiteral("day"), dayNumber},
                                           {QStringLiteral("date"), key},
                                           {QStringLiteral("today"), date == today},
                                           {QStringLiteral("selected"), key == selectedDate},
                                           {QStringLiteral("weekend"), dayOfWeek == 6 || dayOfWeek == 7},
                                           {QStringLiteral("deadlineCount"), markers.value(QStringLiteral("deadlineCount"), 0)},
                                           {QStringLiteral("plannedWorkCount"), markers.value(QStringLiteral("plannedWorkCount"), 0)},
                                           {QStringLiteral("completedTaskCount"), markers.value(QStringLiteral("completedTaskCount"), 0)}});
            }
            m_months.push_back({locale.monthName(monthNumber, QLocale::LongFormat),
                                visibleYear, std::move(days)});
        }
    }
    endResetModel();
    if (previousCount != m_months.size()) emit countChanged();
}

} // namespace something::models
