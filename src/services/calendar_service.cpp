#include "calendar_service.h"

#include "steam_release_service.h"
#include "task_service.h"

#include <QVariantList>

#include <utility>

namespace something::services {
namespace {

storage::StorageCommand combineCalendarData(storage::StorageCommand taskCommand,
                                             QDate startDate, QDate endDate,
                                             bool includeGameReleases)
{
    return [taskCommand = std::move(taskCommand), startDate, endDate,
            includeGameReleases](storage::StorageRepositories &repositories,
                                 const std::function<bool()> &cancelled) {
        storage::StorageReply reply = taskCommand(repositories, cancelled);
        if (reply.error.isError() || reply.error.isCancelled()) return reply;

        QVariantList releases;
        if (includeGameReleases) {
            const storage::StorageReply releaseReply = SteamReleaseService::calendarEvents(
                startDate, endDate)(repositories, cancelled);
            if (releaseReply.error.isCancelled()) return releaseReply;
            if (releaseReply.error.isError()) {
                reply.values.insert(QStringLiteral("releaseError"),
                                    QStringLiteral("Game release events could not be loaded."));
            } else {
                releases = releaseReply.values.value(QStringLiteral("releases")).toList();
            }
        }
        reply.values.insert(QStringLiteral("releases"), releases);
        return reply;
    };
}

} // namespace

storage::StorageCommand CalendarService::dateWithSchedules(QDate date,
                                                           bool includeGameReleases)
{
    return combineCalendarData(TaskService::calendarDateWithSchedules(date), date, date,
                               includeGameReleases);
}

storage::StorageCommand CalendarService::year(int year, bool includeGameReleases)
{
    const QDate startDate(year, 1, 1);
    const QDate endDate(year, 12, 31);
    return combineCalendarData(TaskService::calendarYear(year), startDate, endDate,
                               includeGameReleases);
}

} // namespace something::services
