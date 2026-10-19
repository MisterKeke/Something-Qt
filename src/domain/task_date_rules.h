#pragma once

#include "task.h"

#include <QDate>

namespace something::domain {

struct DashboardWeekRange final {
    QDate from;
    QDate through;
    int weekStartSundayZero{1};
};

// The persisted setting uses Sunday=0 through Saturday=6. Invalid stored
// values follow the repository's Monday default.
DashboardWeekRange dashboardWeekRange(QDate today, int weekStartSundayZero);
bool taskIsDueOn(const Task &task, QDate date);
bool taskBelongsInDashboardToday(const Task &task, QDate today);
bool taskBelongsInDashboardWeek(const Task &task, QDate from, QDate through);

} // namespace something::domain
