#include "dashboard_controller.h"

#include "../application/application_lifecycle.h"
#include "../domain/task_date_rules.h"
#include "../services/dashboard_service.h"
#include "../services/task_service.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QThread>
#include <QVariantList>

#include <utility>

namespace something::controllers {
namespace {

QVector<domain::Task> tasksFrom(const QVariantList &rows)
{
    QVector<domain::Task> tasks;
    tasks.reserve(rows.size());
    for (const QVariant &row : rows)
        tasks.push_back(services::TaskService::fromVariantMap(row.toMap()));
    return tasks;
}

} // namespace

DashboardController::DashboardController(application::ApplicationServiceGraph *graph,
                                         const domain::Clock *clock, QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_clock(clock)
    , m_observedLocalDate(m_clock ? m_clock->localDate() : QDate())
{
    Q_ASSERT(m_graph && m_clock);
    Q_ASSERT(QThread::currentThread() == thread());
    m_todayTasks.setToday(m_observedLocalDate);
    m_overdueTasks.setToday(m_observedLocalDate);
    m_weekTasks.setToday(m_observedLocalDate);
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &DashboardController::onStorageFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &DashboardController::onLifecycleChanged);

    m_clockTimer.setInterval(30000);
    connect(&m_clockTimer, &QTimer::timeout, this, [this] { checkClock(false); });
    if (auto *guiApplication = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(guiApplication, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                if (m_pageVisible && m_ready) {
                    m_clockTimer.start();
                    checkClock(true);
                }
            } else {
                m_clockTimer.stop();
            }
        });
    }
    onLifecycleChanged(m_graph->lifecycle().state());
}

QString DashboardController::todayDate() const
{
    return m_clock->localDate().toString(Qt::ISODate);
}

void DashboardController::refresh()
{
    if (!m_ready) return;
    // Each card starts and completes independently; an error in one source
    // cannot hold back local task cards or retained navigation.
    loadToday();
    loadWeek();
    loadFavoriteNews();
}

void DashboardController::reloadFavoriteNews()
{
    loadFavoriteNews();
}

void DashboardController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (!visible) {
        m_clockTimer.stop();
        return;
    }
    if (!m_ready) return;
    m_clockTimer.start();
    checkClock(false);
}

void DashboardController::setWeekStart(int sundayZero)
{
    if (sundayZero < 0 || sundayZero > 6) return;
    if (m_weekStartSundayZero == sundayZero) return;
    m_weekStartSundayZero = sundayZero;
    if (m_ready) loadWeek();
}

void DashboardController::refreshTaskSummaries()
{
    if (!m_ready) return;
    loadToday();
    loadWeek();
}

void DashboardController::onTaskMutation(qint64 taskId, const QVariantMap &currentTask,
                                          bool deleted)
{
    if (!m_ready || taskId <= 0) return;
    const bool wasToday = m_todayTasks.taskById(taskId) || m_overdueTasks.taskById(taskId);
    const bool wasWeek = m_weekTasks.taskById(taskId);
    const domain::Task updated = deleted
        ? domain::Task{}
        : services::TaskService::fromVariantMap(currentTask);
    const QDate today = m_clock->localDate();
    const bool isToday = !deleted && domain::taskBelongsInDashboardToday(updated, today);
    const domain::DashboardWeekRange currentWeek =
        domain::dashboardWeekRange(today, m_weekStartSundayZero);
    const bool isWeek = !deleted && domain::taskBelongsInDashboardWeek(
        updated, currentWeek.from, currentWeek.through);
    if (wasToday || isToday) loadToday();
    const bool initialWeekQueryPending = m_weekOperationId != 0 && !m_weekFrom.isValid();
    if (wasWeek || isWeek || initialWeekQueryPending) loadWeek();
}

void DashboardController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    m_ready = state == application::ApplicationLifecycle::State::Running;
    if (m_ready) {
        if (m_pageVisible) m_clockTimer.start();
        if (m_todayTasks.count() == 0 && m_overdueTasks.count() == 0) loadToday();
        if (m_weekTasks.count() == 0) loadWeek();
        if (m_favoriteNews.count() == 0) loadFavoriteNews();
    } else {
        m_clockTimer.stop();
    }
    if (state == application::ApplicationLifecycle::State::Failed) {
        const QString message = m_graph->lifecycle().startupError().userMessage();
        m_todayError = message;
        m_weekError = message;
        m_favoriteNewsError = message;
    }
    emit stateChanged();
}

void DashboardController::loadToday()
{
    if (!m_ready) return;
    cancelOperation(m_todayOperationId);
    const auto operation = m_graph->beginOperation(QStringLiteral("load dashboard today tasks"),
                                                    QStringLiteral("dashboard-today"));
    if (!operation.isValid()) return;
    m_todayOperationId = operation.id.value;
    m_pending.insert(operation.id.value, Action::Today);
    m_todayLoading = true;
    m_todayError.clear();
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::TaskService::dashboardToday(
            m_clock->localDate(), true, false))) {
        if (m_pending.remove(operation.id.value)) {
            m_todayOperationId = 0;
            m_todayLoading = false;
            m_todayError = QStringLiteral("Today's tasks could not be loaded.");
            emit stateChanged();
        }
    }
}

void DashboardController::loadWeek()
{
    if (!m_ready) return;
    cancelOperation(m_weekOperationId);
    const auto operation = m_graph->beginOperation(QStringLiteral("load dashboard week tasks"),
                                                    QStringLiteral("dashboard-week"));
    if (!operation.isValid()) return;
    m_weekOperationId = operation.id.value;
    m_pending.insert(operation.id.value, Action::Week);
    m_weekLoading = true;
    m_weekError.clear();
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::TaskService::dashboardWeek(
            m_clock->localDate(), false, false))) {
        if (m_pending.remove(operation.id.value)) {
            m_weekOperationId = 0;
            m_weekLoading = false;
            m_weekError = QStringLiteral("This week's tasks could not be loaded.");
            emit stateChanged();
        }
    }
}

void DashboardController::loadFavoriteNews()
{
    if (!m_ready) return;
    cancelOperation(m_favoriteNewsOperationId);
    const auto operation = m_graph->beginOperation(QStringLiteral("load saved favorite updates"),
                                                    QStringLiteral("dashboard-favorite-news"));
    if (!operation.isValid()) return;
    m_favoriteNewsOperationId = operation.id.value;
    m_pending.insert(operation.id.value, Action::FavoriteNews);
    m_favoriteNewsLoading = true;
    m_favoriteNewsError.clear();
    emit stateChanged();
    if (!m_graph->submitStorage(operation, services::DashboardService::recentFavoriteNews())) {
        if (m_pending.remove(operation.id.value)) {
            m_favoriteNewsOperationId = 0;
            m_favoriteNewsLoading = false;
            m_favoriteNewsError = QStringLiteral("Saved favorite updates could not be loaded.");
            emit stateChanged();
        }
    }
}

void DashboardController::checkClock(bool resumed)
{
    const QDate now = m_clock->localDate();
    if (!resumed && now == m_observedLocalDate) return;
    const bool dayChanged = now != m_observedLocalDate;
    m_observedLocalDate = now;
    m_todayTasks.setToday(now);
    m_overdueTasks.setToday(now);
    m_weekTasks.setToday(now);
    if (dayChanged || resumed) {
        loadToday();
        loadWeek();
    }
    emit stateChanged();
}

void DashboardController::onStorageFinished(quint64 operationId,
                                             const QVariantMap &values,
                                             domain::AppError error)
{
    const auto found = m_pending.find(operationId);
    if (found == m_pending.end()) return;
    const Action action = found.value();
    m_pending.erase(found);
    if (action == Action::Today) {
        if (m_todayOperationId == operationId) m_todayOperationId = 0;
        m_todayLoading = false;
        if (error.isError()) {
            m_todayError = error.userMessage();
        } else if (!error.isCancelled()) {
            m_todayTasks.setToday(m_clock->localDate());
            m_overdueTasks.setToday(m_clock->localDate());
            m_todayTasks.replacePage(tasksFrom(values.value(QStringLiteral("dueToday")).toList()));
            m_overdueTasks.replacePage(tasksFrom(values.value(QStringLiteral("overdue")).toList()));
            m_todayError.clear();
        }
    } else if (action == Action::Week) {
        if (m_weekOperationId == operationId) m_weekOperationId = 0;
        m_weekLoading = false;
        if (error.isError()) {
            m_weekError = error.userMessage();
        } else if (!error.isCancelled()) {
            m_weekFrom = QDate::fromString(values.value(QStringLiteral("fromDate")).toString(), Qt::ISODate);
            m_weekThrough = QDate::fromString(values.value(QStringLiteral("throughDate")).toString(), Qt::ISODate);
            m_weekStartSundayZero = values.value(QStringLiteral("weekStart"), 1).toInt();
            m_weekTasks.setToday(m_clock->localDate());
            m_weekTasks.replacePage(tasksFrom(values.value(QStringLiteral("items")).toList()));
            const QLocale locale;
            m_weekRangeLabel = m_weekFrom.isValid() && m_weekThrough.isValid()
                && m_weekFrom <= m_weekThrough
                ? QStringLiteral("%1 – %2").arg(locale.toString(m_weekFrom, QLocale::ShortFormat),
                                                 locale.toString(m_weekThrough, QLocale::ShortFormat))
                : QStringLiteral("No remaining dates in this week");
            m_weekError.clear();
        }
    } else {
        if (m_favoriteNewsOperationId == operationId) m_favoriteNewsOperationId = 0;
        m_favoriteNewsLoading = false;
        if (error.isError()) {
            m_favoriteNewsError = error.userMessage();
        } else if (!error.isCancelled()) {
            const QVariantList items = values.value(QStringLiteral("items")).toList();
            m_favoriteNews.replace(items);
            emit favoriteNewsItemsChanged(items);
            m_favoriteNewsError.clear();
        }
    }
    emit stateChanged();
}

void DashboardController::cancelOperation(quint64 &operationId)
{
    if (operationId == 0) return;
    m_graph->cancelOperation({operationId});
    m_pending.remove(operationId);
    operationId = 0;
}

} // namespace something::controllers
