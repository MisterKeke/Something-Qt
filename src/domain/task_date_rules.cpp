#include "task_date_rules.h"

#include <Qt>

namespace something::domain {

DashboardWeekRange dashboardWeekRange(QDate today, int weekStartSundayZero)
{
    if (!today.isValid()) return {};
    if (weekStartSundayZero < 0 || weekStartSundayZero > 6)
        weekStartSundayZero = 1;
    const int todaySundayZero = today.dayOfWeek() % 7;
    const int daysFromWeekStart = (todaySundayZero - weekStartSundayZero + 7) % 7;
    return {today.addDays(1), today.addDays(6 - daysFromWeekStart), weekStartSundayZero};
}

bool taskIsDueOn(const Task &task, QDate date)
{
    if (!date.isValid() || task.dueDate.isEmpty()) return false;
    return QDate::fromString(task.dueDate, Qt::ISODate) == date;
}

bool taskBelongsInDashboardToday(const Task &task, QDate today)
{
    if (task.completed || !today.isValid() || task.dueDate.isEmpty()) return false;
    const QDate dueDate = QDate::fromString(task.dueDate, Qt::ISODate);
    return dueDate.isValid() && dueDate <= today;
}

bool taskBelongsInDashboardWeek(const Task &task, QDate from, QDate through)
{
    if (task.completed || !from.isValid() || !through.isValid() || from > through
        || task.dueDate.isEmpty()) return false;
    const QDate dueDate = QDate::fromString(task.dueDate, Qt::ISODate);
    return dueDate.isValid() && dueDate >= from && dueDate <= through;
}

} // namespace something::domain
