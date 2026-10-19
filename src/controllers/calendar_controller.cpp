#include "calendar_controller.h"

#include "../application/application_lifecycle.h"
#include "../domain/task_date_rules.h"
#include "../domain/task_rules.h"
#include "../services/task_service.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
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
        if (!m_selectedDate.isEmpty()) loadSelectedDate(false);
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
            if (!m_selectedDate.isEmpty()) loadSelectedDate(false);
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
    updateCalendarModel();
    updateLabels();
    emit stateChanged();
}

void CalendarController::nextYear()
{
    if (m_visibleYear >= 9999) return;
    ++m_visibleYear;
    updateCalendarModel();
    updateLabels();
    emit stateChanged();
}

void CalendarController::goToday()
{
    m_observedLocalDate = m_clock->localDate();
    m_visibleYear = m_observedLocalDate.year();
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
    if (!m_selectedDate.isEmpty()) loadSelectedDate(false);
}

void CalendarController::onTaskMutation(qint64 taskId, const QVariantMap &currentTask,
                                         bool deleted)
{
    if (m_selectedDate.isEmpty() || taskId <= 0) return;
    const bool alreadyListed = m_tasks.taskById(taskId) != nullptr;
    const domain::Task updated = deleted
        ? domain::Task{}
        : services::TaskService::fromVariantMap(currentTask);
    const bool nowSelected = !deleted && domain::taskIsDueOn(
        updated, QDate::fromString(m_selectedDate, Qt::ISODate));
    if (!alreadyListed && !nowSelected) return;

    if (m_dateOperationId != 0) {
        m_graph->cancelOperation({m_dateOperationId});
        m_pending.remove(m_dateOperationId);
        m_dateOperationId = 0;
        loadSelectedDate(false);
        return;
    }

    if (deleted || !nowSelected) {
        m_tasks.removeTask(taskId);
    } else {
        m_tasks.upsertByCreatedOrder(updated);
    }
    m_summary = QStringLiteral("%1 task(s) scheduled").arg(m_tasks.count());
    setError({});
    emit stateChanged();
}

void CalendarController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    m_ready = state == application::ApplicationLifecycle::State::Running;
    if (m_ready) {
        if (m_pageVisible) m_clockTimer.start();
        if (!m_selectedDate.isEmpty() && m_tasks.count() == 0) loadSelectedDate(true);
    } else {
        m_clockTimer.stop();
    }
    if (state == application::ApplicationLifecycle::State::Failed)
        setError(m_graph->lifecycle().startupError().userMessage());
    emit stateChanged();
}

void CalendarController::updateCalendarModel()
{
    m_months.setCalendar(m_visibleYear, m_observedLocalDate, m_selectedDate);
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
    if (!m_loading) m_summary = QStringLiteral("%1 task(s) scheduled").arg(m_tasks.count());
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
    const auto operation = m_graph->beginOperation(QStringLiteral("load calendar date"),
                                                    QStringLiteral("calendar-date"));
    if (!operation.isValid()) return;
    m_dateOperationId = operation.id.value;
    m_pending.insert(operation.id.value, true);
    m_loading = true;
    m_summary = QStringLiteral("Loading tasks for %1").arg(m_selectedDateLabel);
    setError({});
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::TaskService::calendarDate(
            QDate::fromString(m_selectedDate, Qt::ISODate)))) {
        if (m_pending.remove(operation.id.value) > 0) {
            m_dateOperationId = 0;
            m_loading = false;
            setError(QStringLiteral("Tasks for this date could not be loaded."));
            emit stateChanged();
        }
    }
}

void CalendarController::onStorageFinished(quint64 operationId, const QVariantMap &values,
                                           domain::AppError error)
{
    if (m_pending.remove(operationId) == 0) return;
    if (m_dateOperationId == operationId) m_dateOperationId = 0;
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
        setError({});
        m_summary = QStringLiteral("%1 task(s) scheduled").arg(m_tasks.count());
    }
    emit stateChanged();
}

void CalendarController::setError(QString message)
{
    m_errorMessage = std::move(message);
}

} // namespace something::controllers
