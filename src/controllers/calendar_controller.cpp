#include "calendar_controller.h"

#include "../application/application_lifecycle.h"
#include "../services/calendar_service.h"
#include "../services/preferences_service.h"
#include "../services/task_service.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QHash>
#include <QLocale>
#include <QSet>
#include <QThread>

#include <utility>

namespace something::controllers {

CalendarController::CalendarController(application::ApplicationServiceGraph *graph,
                                       const domain::Clock *clock, QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_clock(clock)
    , m_observedLocalDate(m_clock ? m_clock->localDate() : QDate())
    , m_visibleYear(m_observedLocalDate.year())
{
    Q_ASSERT(m_graph && m_clock);
    Q_ASSERT(QThread::currentThread() == thread());
    m_tasks.setToday(m_observedLocalDate);
    updateCalendarModel();
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &CalendarController::onStorageFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &CalendarController::onLifecycleChanged);

    m_clockTimer.setInterval(30000);
    connect(&m_clockTimer, &QTimer::timeout, this, [this] {
        const QDate now = m_clock->localDate();
        if (now == m_observedLocalDate) return;
        m_observedLocalDate = now;
        updateCalendarModel();
        emit stateChanged();
        if (m_pageVisible) refreshScheduleData();
    });
    if (auto *guiApplication = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(guiApplication, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) {
                m_clockTimer.stop();
                return;
            }
            if (!m_pageVisible || !m_ready) return;
            m_clockTimer.start();
            m_observedLocalDate = m_clock->localDate();
            updateCalendarModel();
            emit stateChanged();
            refreshScheduleData();
        });
    }
    onLifecycleChanged(m_graph->lifecycle().state());
}

QString CalendarController::todayDate() const
{
    return m_clock->localDate().toString(Qt::ISODate);
}

void CalendarController::previousYear()
{
    if (m_visibleYear <= 1) return;
    --m_visibleYear;
    loadYearIndicators(true);
    updateCalendarModel();
    updateLabels();
    emit stateChanged();
}

void CalendarController::nextYear()
{
    if (m_visibleYear >= 9999) return;
    ++m_visibleYear;
    loadYearIndicators(true);
    updateCalendarModel();
    updateLabels();
    emit stateChanged();
}

void CalendarController::goToday()
{
    m_observedLocalDate = m_clock->localDate();
    const int previousYear = m_visibleYear;
    m_visibleYear = m_observedLocalDate.year();
    if (m_visibleYear != previousYear) loadYearIndicators(true);
    updateCalendarModel();
    updateLabels();
    emit stateChanged();
    // The reference Today action changes the visible year only.
}

void CalendarController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (!visible) {
        m_clockTimer.stop();
        return;
    }
    if (!m_ready) return;
    m_clockTimer.start();
    m_observedLocalDate = m_clock->localDate();
    updateCalendarModel();
    loadReleaseVisibility();
    loadYearIndicators();
    if (!m_selectedDate.isEmpty()) loadSelectedDate(false);
    emit stateChanged();
}

void CalendarController::selectDate(const QString &isoDate)
{
    const QDate date = QDate::fromString(isoDate, Qt::ISODate);
    if (!date.isValid() || date.toString(Qt::ISODate) != isoDate
        || isoDate == m_selectedDate) return;
    m_selectedDate = isoDate;
    updateCalendarModel();
    updateLabels();
    loadSelectedDate(true);
    emit stateChanged();
}

void CalendarController::refreshSelectedDate()
{
    refreshScheduleData();
}

void CalendarController::refreshScheduleData()
{
    if (!m_pageVisible || !m_ready) return;
    loadReleaseVisibility();
    loadYearIndicators();
    if (!m_selectedDate.isEmpty()) loadSelectedDate(false);
}

void CalendarController::setShowGameReleases(bool visible)
{
    if (!m_ready || !m_releaseVisibilityLoaded || m_releaseVisibilitySaving) return;
    if (visible == m_showGameReleases) return;

    m_visibilityBeforeSave = m_showGameReleases;
    m_showGameReleases = visible;
    if (!visible) {
        m_selectedReleaseEvents.clear();
        m_releaseEventsErrorMessage.clear();
    }
    updateLabels();
    m_releaseVisibilitySaving = true;
    m_releaseVisibilityError.clear();
    const auto operation = m_graph->beginOperation(
        QStringLiteral("save calendar release visibility"));
    if (!operation.isValid()) {
        m_showGameReleases = m_visibilityBeforeSave;
        m_releaseVisibilitySaving = false;
        m_releaseVisibilityError = QStringLiteral(
            "Calendar release visibility could not be saved.");
        emit stateChanged();
        return;
    }
    m_visibilityOperationId = operation.id.value;
    m_pending.insert(operation.id.value, Action::SaveReleaseVisibility);
    if (!m_graph->submitStorage(operation,
            services::PreferencesService::setGameReleasesVisibleInCalendar(visible))) {
        if (m_pending.remove(operation.id.value) > 0) {
            m_visibilityOperationId = 0;
            m_showGameReleases = m_visibilityBeforeSave;
            m_releaseVisibilitySaving = false;
            m_releaseVisibilityError = QStringLiteral(
                "Calendar release visibility could not be saved.");
        }
    }
    emit stateChanged();
}

void CalendarController::onTaskMutation(qint64 taskId, const QVariantMap &currentTask,
                                         bool deleted)
{
    Q_UNUSED(taskId);
    Q_UNUSED(currentTask);
    Q_UNUSED(deleted);
    refreshScheduleData();
}

void CalendarController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    m_ready = state == application::ApplicationLifecycle::State::Running;
    if (m_ready) {
        loadReleaseVisibility();
        if (m_pageVisible) {
            m_clockTimer.start();
            loadYearIndicators();
            if (!m_selectedDate.isEmpty()) loadSelectedDate(true);
        }
    } else {
        m_clockTimer.stop();
    }
    if (state == application::ApplicationLifecycle::State::Failed)
        setError(m_graph->lifecycle().startupError().userMessage());
    emit stateChanged();
}

void CalendarController::updateCalendarModel()
{
    m_months.setCalendar(m_visibleYear, m_observedLocalDate, m_selectedDate, m_dateMarkers);
    m_tasks.setToday(m_observedLocalDate);
    updateLabels();
}

void CalendarController::updateLabels()
{
    const QLocale locale;
    const int firstMonth = m_visibleYear == m_observedLocalDate.year()
        ? m_observedLocalDate.month() : 1;
    const QString start = locale.monthName(firstMonth, QLocale::LongFormat);
    const QString january = locale.monthName(1, QLocale::LongFormat);
    const QString december = locale.monthName(12, QLocale::LongFormat);
    m_rangeLabel = m_visibleYear == m_observedLocalDate.year()
        ? QStringLiteral("%1 %2 – %3 %2").arg(start).arg(m_visibleYear).arg(december)
        : QStringLiteral("%1 %2 – %3 %2").arg(january).arg(m_visibleYear).arg(december);
    if (m_selectedDate.isEmpty()) {
        m_selectedDateLabel.clear();
        m_summary.clear();
        return;
    }
    const QDate selected = QDate::fromString(m_selectedDate, Qt::ISODate);
    m_selectedDateLabel = locale.toString(selected, QStringLiteral("dddd, d MMMM yyyy"));
    if (!m_loading) {
        const int releaseCount = m_showGameReleases ? m_selectedReleaseEvents.size() : 0;
        m_summary = releaseCount > 0
            ? QStringLiteral("%1 task(s) and %2 game release(s) on this date")
                  .arg(m_tasks.count()).arg(releaseCount)
            : QStringLiteral("%1 task(s) on this date").arg(m_tasks.count());
    }
}

void CalendarController::loadReleaseVisibility()
{
    if (!m_ready || m_releaseVisibilityLoaded || m_releaseVisibilityLoading
        || m_releaseVisibilitySaving) return;
    const auto operation = m_graph->beginOperation(
        QStringLiteral("load calendar release visibility"));
    if (!operation.isValid()) {
        m_releaseVisibilityLoaded = true;
        m_releaseVisibilityError = QStringLiteral(
            "Calendar release visibility could not be loaded; releases are shown by default.");
        emit stateChanged();
        return;
    }
    m_visibilityOperationId = operation.id.value;
    m_pending.insert(operation.id.value, Action::LoadReleaseVisibility);
    m_releaseVisibilityLoading = true;
    if (!m_graph->submitStorage(operation,
            services::PreferencesService::loadGameReleasesVisibleInCalendar())) {
        if (m_pending.remove(operation.id.value) > 0) {
            m_visibilityOperationId = 0;
            m_releaseVisibilityLoading = false;
            m_releaseVisibilityLoaded = true;
            m_releaseVisibilityError = QStringLiteral(
                "Calendar release visibility could not be loaded; releases are shown by default.");
        }
    }
    emit stateChanged();
}

void CalendarController::loadSelectedDate(bool clearRows)
{
    if (!m_ready || m_selectedDate.isEmpty()) return;
    if (m_dateOperationId != 0) {
        m_graph->cancelOperation({m_dateOperationId});
        m_pending.remove(m_dateOperationId);
        m_dateOperationId = 0;
    }
    if (clearRows) m_tasks.replacePage({});
    if (clearRows) m_selectedSchedules.clear();
    if (clearRows) m_selectedReleaseEvents.clear();
    const auto operation = m_graph->beginOperation(QStringLiteral("load calendar date"),
                                                    QStringLiteral("calendar-date"));
    if (!operation.isValid()) {
        m_loading = false;
        setError(QStringLiteral("Tasks for this date could not be loaded."));
        emit stateChanged();
        return;
    }
    m_dateOperationId = operation.id.value;
    m_pending.insert(operation.id.value, Action::SelectedDate);
    m_loading = true;
    m_summary = QStringLiteral("Loading tasks for %1").arg(m_selectedDateLabel);
    setError({});
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::CalendarService::dateWithSchedules(
            QDate::fromString(m_selectedDate, Qt::ISODate),
            m_releaseVisibilityLoaded && m_showGameReleases))) {
        if (m_pending.remove(operation.id.value) > 0) {
            m_dateOperationId = 0;
            m_loading = false;
            setError(QStringLiteral("Tasks for this date could not be loaded."));
            emit stateChanged();
        }
    }
}

void CalendarController::loadYearIndicators(bool clearExisting)
{
    if (clearExisting) {
        m_dateMarkers.clear();
        updateCalendarModel();
    }
    if (!m_ready || !m_pageVisible) return;
    if (m_yearOperationId != 0) {
        m_graph->cancelOperation({m_yearOperationId});
        m_pending.remove(m_yearOperationId);
        m_yearOperationId = 0;
        m_markersLoading = false;
    }
    const auto operation = m_graph->beginOperation(QStringLiteral("load calendar year markers"),
                                                    QStringLiteral("calendar-year-markers"));
    if (!operation.isValid()) {
        m_markersLoading = false;
        m_markerErrorMessage = QStringLiteral("Calendar activity markers could not be loaded.");
        emit stateChanged();
        return;
    }
    m_yearOperationId = operation.id.value;
    m_pending.insert(operation.id.value, Action::YearMarkers);
    m_markersLoading = true;
    m_markerErrorMessage.clear();
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::CalendarService::year(
            m_visibleYear, m_releaseVisibilityLoaded && m_showGameReleases))) {
        if (m_pending.remove(operation.id.value) > 0) {
            m_yearOperationId = 0;
            m_markersLoading = false;
            m_markerErrorMessage = QStringLiteral("Calendar activity markers could not be loaded.");
            emit stateChanged();
        }
    }
}

void CalendarController::onStorageFinished(quint64 operationId, const QVariantMap &values,
                                           domain::AppError error)
{
    const auto pending = m_pending.find(operationId);
    if (pending == m_pending.end()) return;
    const Action action = pending.value();
    m_pending.erase(pending);

    if (action == Action::LoadReleaseVisibility) {
        if (m_visibilityOperationId != operationId) return;
        m_visibilityOperationId = 0;
        m_releaseVisibilityLoading = false;
        m_releaseVisibilityLoaded = true;
        if (error.isError() || error.isCancelled()) {
            m_showGameReleases = true;
            m_releaseVisibilityError = QStringLiteral(
                "Calendar release visibility could not be loaded; releases are shown by default.");
        } else {
            m_showGameReleases = values.value(QStringLiteral("showGameReleases")).toBool();
            m_releaseVisibilityError.clear();
        }
        if (m_pageVisible) refreshScheduleData();
        emit stateChanged();
        return;
    }

    if (action == Action::SaveReleaseVisibility) {
        if (m_visibilityOperationId != operationId) return;
        m_visibilityOperationId = 0;
        m_releaseVisibilitySaving = false;
        if (error.isError() || error.isCancelled()) {
            m_showGameReleases = m_visibilityBeforeSave;
            if (error.isError())
                m_releaseVisibilityError = QStringLiteral(
                    "Calendar release visibility could not be saved.");
        } else {
            m_showGameReleases = values.value(QStringLiteral("showGameReleases")).toBool();
            m_releaseVisibilityError.clear();
        }
        if (m_pageVisible) refreshScheduleData();
        emit stateChanged();
        return;
    }

    if (action == Action::YearMarkers) {
        if (m_yearOperationId != operationId) return;
        m_yearOperationId = 0;
        m_markersLoading = false;
        if (error.isError()) {
            m_markerErrorMessage = error.userMessage();
            emit stateChanged();
            return;
        }
        if (error.isCancelled()) {
            emit stateChanged();
            return;
        }

        const QDate yearStart(m_visibleYear, 1, 1);
        const QDate yearEnd(m_visibleYear, 12, 31);
        QHash<qint64, QVariantMap> tasksById;
        QHash<QString, QVariantMap> markers;
        QHash<QString, QSet<qint64>> completedByDate;
        const QVariantList taskRows = values.value(QStringLiteral("tasks")).toList();
        for (const QVariant &rowValue : taskRows) {
            const QVariantMap row = rowValue.toMap();
            const qint64 taskId = row.value(QStringLiteral("id")).toLongLong();
            tasksById.insert(taskId, row);
            const QString dueText = row.value(QStringLiteral("dueDate")).toString();
            const QDate dueDate = QDate::fromString(dueText, Qt::ISODate);
            if (dueDate.isValid() && dueDate >= yearStart && dueDate <= yearEnd) {
                QVariantMap marker = markers.value(dueText);
                marker.insert(QStringLiteral("deadlineCount"),
                              marker.value(QStringLiteral("deadlineCount")).toInt() + 1);
                marker.insert(QStringLiteral("plannedWorkCount"),
                              marker.value(QStringLiteral("plannedWorkCount")).toInt());
                marker.insert(QStringLiteral("completedTaskCount"),
                              marker.value(QStringLiteral("completedTaskCount")).toInt());
                markers.insert(dueText, marker);
                if (row.value(QStringLiteral("completed")).toBool())
                    completedByDate[dueText].insert(taskId);
            }
        }
        const QVariantList scheduleRows = values.value(QStringLiteral("schedules")).toList();
        for (const QVariant &rowValue : scheduleRows) {
            const QVariantMap schedule = rowValue.toMap();
            const qint64 taskId = schedule.value(QStringLiteral("taskId")).toLongLong();
            const QString plannedText = schedule.value(QStringLiteral("plannedDate")).toString();
            const QDate plannedDate = QDate::fromString(plannedText, Qt::ISODate);
            if (!plannedDate.isValid() || plannedDate < yearStart || plannedDate > yearEnd)
                continue;
            QVariantMap marker = markers.value(plannedText);
            marker.insert(QStringLiteral("deadlineCount"),
                          marker.value(QStringLiteral("deadlineCount")).toInt());
            marker.insert(QStringLiteral("plannedWorkCount"),
                          marker.value(QStringLiteral("plannedWorkCount")).toInt() + 1);
            marker.insert(QStringLiteral("completedTaskCount"),
                          marker.value(QStringLiteral("completedTaskCount")).toInt());
            markers.insert(plannedText, marker);
            const auto task = tasksById.constFind(taskId);
            if (task != tasksById.cend() && task.value().value(QStringLiteral("completed")).toBool())
                completedByDate[plannedText].insert(taskId);
        }
        const QVariantList releaseRows = values.value(QStringLiteral("releases")).toList();
        for (const QVariant &rowValue : releaseRows) {
            const QVariantMap release = rowValue.toMap();
            if (release.value(QStringLiteral("datePrecision")).toString()
                    != QStringLiteral("exact_day")) {
                continue;
            }
            const QString exactText = release.value(QStringLiteral("exactDate")).toString();
            const QDate exactDate = QDate::fromString(exactText, Qt::ISODate);
            if (!exactDate.isValid() || exactDate < yearStart || exactDate > yearEnd
                || exactDate.toString(Qt::ISODate) != exactText) {
                continue;
            }
            QVariantMap marker = markers.value(exactText);
            marker.insert(QStringLiteral("deadlineCount"),
                          marker.value(QStringLiteral("deadlineCount")).toInt());
            marker.insert(QStringLiteral("plannedWorkCount"),
                          marker.value(QStringLiteral("plannedWorkCount")).toInt());
            marker.insert(QStringLiteral("completedTaskCount"),
                          marker.value(QStringLiteral("completedTaskCount")).toInt());
            marker.insert(QStringLiteral("gameReleaseCount"),
                          marker.value(QStringLiteral("gameReleaseCount")).toInt() + 1);
            markers.insert(exactText, marker);
        }
        for (auto it = markers.begin(); it != markers.end(); ++it)
            it.value().insert(QStringLiteral("completedTaskCount"),
                              completedByDate.value(it.key()).size());
        m_dateMarkers.clear();
        for (auto it = markers.cbegin(); it != markers.cend(); ++it)
            m_dateMarkers.insert(it.key(), it.value());
        m_releaseEventsErrorMessage = values.value(QStringLiteral("releaseError")).toString();
        m_markerErrorMessage.clear();
        updateCalendarModel();
        emit stateChanged();
        return;
    }

    if (m_dateOperationId != operationId) return;
    m_dateOperationId = 0;
    m_loading = false;
    if (error.isError()) {
        setError(error.userMessage());
    } else if (!error.isCancelled()) {
        QVector<domain::Task> tasks;
        const QVariantList rows = values.value(QStringLiteral("tasks")).toList();
        tasks.reserve(rows.size());
        for (const QVariant &row : rows)
            tasks.push_back(services::TaskService::fromVariantMap(row.toMap()));
        m_tasks.setToday(m_clock->localDate());
        m_tasks.replacePage(std::move(tasks));
        m_selectedReleaseEvents.clear();
        for (const QVariant &row : values.value(QStringLiteral("releases")).toList()) {
            const QVariantMap release = row.toMap();
            if (release.value(QStringLiteral("datePrecision")).toString()
                    != QStringLiteral("exact_day")
                || release.value(QStringLiteral("exactDate")).toString() != m_selectedDate) {
                continue;
            }
            m_selectedReleaseEvents.push_back(release);
        }
        m_selectedSchedules.clear();
        const QVariantList schedules = values.value(QStringLiteral("schedules")).toList();
        for (const QVariant &rowValue : schedules) {
            const QVariantMap schedule = rowValue.toMap();
            m_selectedSchedules.insert(QString::number(
                schedule.value(QStringLiteral("taskId")).toLongLong()), schedule);
        }
        m_releaseEventsErrorMessage = values.value(QStringLiteral("releaseError")).toString();
        setError({});
        const int releaseCount = m_showGameReleases ? m_selectedReleaseEvents.size() : 0;
        m_summary = releaseCount > 0
            ? QStringLiteral("%1 task(s) and %2 game release(s) on this date")
                  .arg(m_tasks.count()).arg(releaseCount)
            : QStringLiteral("%1 task(s) on this date").arg(m_tasks.count());
    }
    if (!m_showGameReleases) m_releaseEventsErrorMessage.clear();
    emit stateChanged();
}

void CalendarController::setError(QString message)
{
    m_errorMessage = std::move(message);
}

} // namespace something::controllers
