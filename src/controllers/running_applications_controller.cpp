#include "running_applications_controller.h"

#include "../application/application_lifecycle.h"

#include <QGuiApplication>
#include <QThread>

#include <utility>

namespace something::controllers {
namespace {

QVariantList windowsToVariantList(const QVector<platform::RunningWindow> &windows)
{
    QVariantList result;
    result.reserve(windows.size());
    for (const auto &window : windows) {
        result.push_back(QVariantMap{
            {QStringLiteral("id"), window.id},
            {QStringLiteral("title"), window.title},
            {QStringLiteral("processName"), window.processName},
            {QStringLiteral("iconDataUrl"), window.iconDataUrl},
            {QStringLiteral("minimized"), window.minimized},
            {QStringLiteral("active"), window.active}});
    }
    return result;
}

QVector<platform::RunningWindow> windowsFromVariantList(const QVariantList &values)
{
    QVector<platform::RunningWindow> result;
    result.reserve(values.size());
    for (const QVariant &value : values) {
        const QVariantMap row = value.toMap();
        result.push_back({row.value(QStringLiteral("id")).toString(),
                          row.value(QStringLiteral("title")).toString(),
                          row.value(QStringLiteral("processName")).toString(),
                          row.value(QStringLiteral("iconDataUrl")).toString(),
                          row.value(QStringLiteral("minimized")).toBool(),
                          row.value(QStringLiteral("active")).toBool()});
    }
    return result;
}

} // namespace

RunningApplicationsController::RunningApplicationsController(
    application::ApplicationServiceGraph *graph,
    platform::RunningWindowsPlatform *platform,
    QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_platform(platform)
    , m_windows(this)
{
    Q_ASSERT(m_graph && m_platform);
    Q_ASSERT(QThread::currentThread() == thread());
    m_pollTimer.setInterval(2800);
    m_pollTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &RunningApplicationsController::reload);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &RunningApplicationsController::onWorkerFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &RunningApplicationsController::onLifecycleChanged);
    if (auto *guiApplication = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        m_applicationActive = guiApplication->applicationState() == Qt::ApplicationActive;
        connect(guiApplication, &QGuiApplication::applicationStateChanged,
                this, &RunningApplicationsController::onApplicationStateChanged);
    } else {
        // QCoreApplication-only controller tests have no window activation
        // signal; the production GUI always takes the branch above.
        m_applicationActive = true;
    }
    onLifecycleChanged(m_graph->lifecycle().state());
}

void RunningApplicationsController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    const bool ready = state == application::ApplicationLifecycle::State::Running;
    if (m_ready != ready) m_ready = ready;
    if (!ready) cancelAll();
    updatePolling();
    if (ready && m_applicationActive && (m_pageVisible || m_dashboardVisible)) reload();
    emit stateChanged();
}

void RunningApplicationsController::onApplicationStateChanged(Qt::ApplicationState state)
{
    const bool active = state == Qt::ApplicationActive;
    if (m_applicationActive == active) return;
    m_applicationActive = active;
    if (!active) cancelList();
    updatePolling();
    if (active && (m_pageVisible || m_dashboardVisible) && m_ready) reload();
    emit stateChanged();
}

void RunningApplicationsController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (!visible && !m_dashboardVisible) cancelList();
    else m_notification.clear();
    updatePolling();
    if (visible && m_ready && m_applicationActive) reload();
    emit stateChanged();
}

void RunningApplicationsController::setDashboardVisible(bool visible)
{
    if (m_dashboardVisible == visible) return;
    m_dashboardVisible = visible;
    if (!visible && !m_pageVisible) cancelList();
    else if (visible && m_ready && m_applicationActive) reload();
    emit stateChanged();
}

void RunningApplicationsController::updatePolling()
{
    const bool shouldPoll = m_ready && m_pageVisible && m_applicationActive && supported();
    if (!shouldPoll) m_pollTimer.stop();
    else if (!m_pollTimer.isActive()) m_pollTimer.start();
}

void RunningApplicationsController::reload()
{
    if (!m_ready || (!m_pageVisible && !m_dashboardVisible)
        || !m_applicationActive || !supported()) {
        if (!supported()) m_errorMessage = QStringLiteral("Running-window control is available on Windows.");
        emit stateChanged();
        return;
    }
    if (m_listOperation) return; // Polls and manual refresh coalesce while one snapshot is active.
    const application::OperationContext operation = m_graph->beginOperation(
        QStringLiteral("enumerate running application windows"));
    if (!operation.isValid()) return;
    Pending pending;
    pending.kind = Kind::List;
    pending.operation = operation;
    m_pending.insert(operation.id.value, pending);
    m_listOperation = operation.id.value;
    m_loading = m_windows.rowCount() == 0;
    m_errorMessage.clear();
    auto *native = m_platform;
    if (!m_graph->submitExpensiveValue(operation,
        [native](const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            domain::AppError error;
            const QVector<platform::RunningWindow> windows = native->list(cancellation, &error);
            result.error = error;
            if (!error.isError() && !error.isCancelled())
                result.values.insert(QStringLiteral("windows"), windowsToVariantList(windows));
            return result;
        })) {
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
    emit stateChanged();
}

void RunningApplicationsController::activateWindow(const QString &windowId)
{
    if (!m_ready || !supported() || m_activating || windowId.isEmpty()) return;
    if (m_windows.rowForId(windowId) < 0) {
        m_notification = QStringLiteral("This window is no longer available. Refresh the list.");
        emit stateChanged();
        return;
    }
    cancelList();
    const auto operation = m_graph->beginOperation(QStringLiteral("activate running application window"));
    if (!operation.isValid()) return;
    Pending pending;
    pending.kind = Kind::Activate;
    pending.operation = operation;
    pending.windowId = windowId;
    m_pending.insert(operation.id.value, pending);
    m_activating = true;
    m_errorMessage.clear();
    auto *native = m_platform;
    if (!m_graph->submitExpensiveValue(operation,
        [native, windowId](const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            const platform::WindowActionResult action = native->activate(windowId, cancellation);
            result.values.insert(QStringLiteral("action"), static_cast<int>(action));
            return result;
        })) {
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
    emit stateChanged();
}

void RunningApplicationsController::onWorkerFinished(quint64 id, const QVariantMap &values,
                                                      domain::AppError error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    if (error.isError()) {
        fail(id, error);
        return;
    }
    if (it->kind == Kind::List) {
        if (error.isCancelled()) {
            finish(id);
            return;
        }
        m_windows.replace(windowsFromVariantList(values.value(QStringLiteral("windows")).toList()));
        m_hasSnapshot = true;
        m_loading = false;
        m_errorMessage.clear();
        finish(id);
        return;
    }
    const auto action = static_cast<platform::WindowActionResult>(
        values.value(QStringLiteral("action")).toInt());
    switch (action) {
    case platform::WindowActionResult::Activated:
        m_notification = QStringLiteral("Window activated.");
        break;
    case platform::WindowActionResult::ActivationRefused:
        m_notification = QStringLiteral("Windows prevented foreground activation. Select the application from the taskbar.");
        break;
    case platform::WindowActionResult::Unavailable:
    case platform::WindowActionResult::InvalidId:
        m_notification = QStringLiteral("This window is no longer available. Refresh the list.");
        break;
    case platform::WindowActionResult::Cancelled:
        break;
    case platform::WindowActionResult::Unsupported:
        m_notification = QStringLiteral("Running-window control is available on Windows.");
        break;
    case platform::WindowActionResult::Failed:
        m_notification = QStringLiteral("The window could not be activated.");
        break;
    }
    finish(id);
    if ((m_pageVisible || m_dashboardVisible) && m_applicationActive) reload();
}

void RunningApplicationsController::finish(quint64 id)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Kind kind = it->kind;
    m_pending.erase(it);
    if (m_listOperation == id) m_listOperation = 0;
    if (kind == Kind::List) m_loading = false;
    else m_activating = false;
    m_graph->completeOperation({id});
    emit stateChanged();
}

void RunningApplicationsController::fail(quint64 id, domain::AppError error, QString message)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Kind kind = it->kind;
    m_pending.erase(it);
    if (m_listOperation == id) m_listOperation = 0;
    if (kind == Kind::List) m_loading = false;
    else m_activating = false;
    if (!error.isCancelled()) {
        if (!message.isEmpty()) m_errorMessage = std::move(message);
        else if (error.category() == domain::ErrorCategory::CapabilityUnavailable)
            m_errorMessage = QStringLiteral("Running-window control is available on Windows.");
        else m_errorMessage = QStringLiteral("Running applications could not be refreshed.");
    }
    m_graph->completeOperation({id}, error);
    emit stateChanged();
}

void RunningApplicationsController::cancelList()
{
    const quint64 id = m_listOperation;
    if (!id) return;
    m_listOperation = 0;
    m_pending.remove(id);
    m_graph->cancelOperation({id});
    m_loading = false;
    emit stateChanged();
}

void RunningApplicationsController::cancelAll()
{
    const auto pending = m_pending;
    m_pending.clear();
    m_listOperation = 0;
    m_loading = false;
    m_activating = false;
    m_pollTimer.stop();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        m_graph->cancelOperation({it.key()});
    emit stateChanged();
}

void RunningApplicationsController::clearMessages()
{
    if (m_errorMessage.isEmpty() && m_notification.isEmpty()) return;
    m_errorMessage.clear();
    m_notification.clear();
    emit stateChanged();
}

} // namespace something::controllers
