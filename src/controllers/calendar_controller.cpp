#include "calendar_controller.h"

#include "../application/application_lifecycle.h"
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
    loadYearIndicators();
    if (!m_selectedDate.isEmpty()) loadSelectedDate(false);
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
    if (!m_loading) m_summary = QStringLiteral("%1 task(s) on this date").arg(m_tasks.count());
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
    if (!m_graph->submitStorage(operation, services::TaskService::calendarDateWithSchedules(
            QDate::fromString(m_selectedDate, Qt::ISODate)))) {
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
    if (!m_graph->submitStorage(operation, services::TaskService::calendarYear(m_visibleYear))) {
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
        for (auto it = markers.begin(); it != markers.end(); ++it)
            it.value().insert(QStringLiteral("completedTaskCount"),
                              completedByDate.value(it.key()).size());
        m_dateMarkers.clear();
        for (auto it = markers.cbegin(); it != markers.cend(); ++it)
            m_dateMarkers.insert(it.key(), it.value());
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
        m_selectedSchedules.clear();
        const QVariantList schedules = values.value(QStringLiteral("schedules")).toList();
        for (const QVariant &rowValue : schedules) {
            const QVariantMap schedule = rowValue.toMap();
            m_selectedSchedules.insert(QString::number(
                schedule.value(QStringLiteral("taskId")).toLongLong()), schedule);
        }
        setError({});
        m_summary = QStringLiteral("%1 task(s) on this date").arg(m_tasks.count());
    }
    emit stateChanged();
}

void CalendarController::setError(QString message)
{
    m_errorMessage = std::move(message);
}

} // namespace something::controllers
