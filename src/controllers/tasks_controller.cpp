#include "tasks_controller.h"

#include "../application/application_lifecycle.h"
#include "../services/task_service.h"
#include "../domain/task_rules.h"

#include <QDate>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaType>
#include <QThread>

#include <algorithm>
#include <utility>

namespace something::controllers {
namespace {

QVariantMap taskMapFrom(const QVariantMap &values)
{
    return values.value(QStringLiteral("task")).toMap();
}

QString userError(domain::AppError error)
{
    if (error.category() == domain::ErrorCategory::Conflict)
        return QStringLiteral("This task changed elsewhere. Keep your draft or load the latest task.");
    return error.userMessage();
}

} // namespace

TasksController::TasksController(application::ApplicationServiceGraph *graph,
                                 const domain::Clock *clock, QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_clock(clock)
    , m_model()
    , m_observedLocalDate(m_clock ? m_clock->localDate() : QDate())
{
    Q_ASSERT(m_graph && m_clock);
    Q_ASSERT(QThread::currentThread() == thread());
    m_model.setToday(m_observedLocalDate);
    m_filterTimer.setSingleShot(true);
    m_filterTimer.setInterval(180);
    connect(&m_filterTimer, &QTimer::timeout, this, &TasksController::refresh);
    m_localDateTimer.setInterval(60000);
    connect(&m_localDateTimer, &QTimer::timeout, this, [this] {
        const QDate today = m_clock->localDate();
        if (today == m_observedLocalDate) return;
        m_observedLocalDate = today;
        m_model.setToday(today);
        emit stateChanged();
        const QString mode = m_filters.value(QStringLiteral("dateMode")).toString();
        if (mode == QStringLiteral("today") || mode == QStringLiteral("tomorrow")
            || mode == QStringLiteral("next7") || mode == QStringLiteral("overdue")) {
            refresh();
        }
    });
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &TasksController::onStorageFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &TasksController::onLifecycleChanged);
    if (auto *guiApplication = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(guiApplication, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) {
                m_localDateTimer.stop();
                return;
            }
            if (!m_pageVisible || !m_ready) return;
            m_localDateTimer.start();
            const QDate today = m_clock->localDate();
            m_observedLocalDate = today;
            m_model.setToday(today);
            const QString mode = m_filters.value(QStringLiteral("dateMode")).toString();
            if (mode == QStringLiteral("today") || mode == QStringLiteral("tomorrow")
                || mode == QStringLiteral("next7") || mode == QStringLiteral("overdue"))
                refresh();
            emit stateChanged();
        });
    }
    m_filters = {
        {QStringLiteral("query"), QString()},
        {QStringLiteral("dateMode"), QStringLiteral("any")},
        {QStringLiteral("exactDate"), QString()},
        {QStringLiteral("dueFrom"), QString()},
        {QStringLiteral("dueTo"), QString()},
        {QStringLiteral("completion"), QStringLiteral("all")},
        {QStringLiteral("priority"), QString()},
        {QStringLiteral("difficulty"), QString()},
        {QStringLiteral("tags"), QStringList{}},
        {QStringLiteral("sort"), QStringLiteral("created_at")},
        {QStringLiteral("direction"), QStringLiteral("desc")},
    };
    onLifecycleChanged(m_graph->lifecycle().state());
}

void TasksController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    const bool nowReady = state == application::ApplicationLifecycle::State::Running;
    if (m_ready != nowReady) {
        m_ready = nowReady;
        emit stateChanged();
    }
    if (state == application::ApplicationLifecycle::State::Failed) {
        setError(m_graph->lifecycle().startupError().userMessage());
    }
    if (nowReady && m_pageVisible) m_localDateTimer.start();
    else m_localDateTimer.stop();
    if (nowReady && m_model.count() == 0) refresh();
}

void TasksController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (!visible) {
        m_localDateTimer.stop();
        return;
    }
    if (!m_ready) return;
    m_localDateTimer.start();
    const QDate today = m_clock->localDate();
    if (today == m_observedLocalDate) return;
    m_observedLocalDate = today;
    m_model.setToday(today);
    const QString mode = m_filters.value(QStringLiteral("dateMode")).toString();
    if (mode == QStringLiteral("today") || mode == QStringLiteral("tomorrow")
        || mode == QStringLiteral("next7") || mode == QStringLiteral("overdue")) {
        refresh();
    }
    emit stateChanged();
}

void TasksController::refresh()
{
    m_filterTimer.stop();
    if (!m_ready || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running)
        return;
    if (m_listOperationId != 0) {
        m_graph->cancelOperation({m_listOperationId});
        m_pending.remove(m_listOperationId);
        m_listOperationId = 0;
    }
    const auto operation = m_graph->beginOperation(QStringLiteral("load tasks"),
                                                    QStringLiteral("task-list"));
    if (!operation.isValid()) return;
    m_listOperationId = operation.id.value;
    m_pending.insert(operation.id.value, {Action::List, 0});
    m_loading = true;
    setError({});
    emit stateChanged();
    const bool submitted = m_graph->submitStorage(operation,
        services::TaskService::list(currentFilter(), m_pageSize));
    if (!submitted && m_pending.contains(operation.id.value)) {
        m_pending.remove(operation.id.value);
        m_listOperationId = 0;
        m_loading = false;
        setError(QStringLiteral("Tasks could not be loaded."));
        emit stateChanged();
    }
}

void TasksController::setFilters(const QVariantMap &filters)
{
    if (m_listOperationId != 0) {
        m_graph->cancelOperation({m_listOperationId});
        m_pending.remove(m_listOperationId);
        m_listOperationId = 0;
        m_loading = false;
    }
    m_filters = filters;
    m_pageOffset = 0;
    m_hasMore = false;
    m_reconcileNextList = false;
    emit filtersChanged();
    emit stateChanged();
    m_filterTimer.start();
}

void TasksController::clearFilters()
{
    setFilters({
        {QStringLiteral("query"), QString()},
        {QStringLiteral("dateMode"), QStringLiteral("any")},
        {QStringLiteral("exactDate"), QString()},
        {QStringLiteral("dueFrom"), QString()},
        {QStringLiteral("dueTo"), QString()},
        {QStringLiteral("completion"), QStringLiteral("all")},
        {QStringLiteral("priority"), QString()},
        {QStringLiteral("difficulty"), QString()},
        {QStringLiteral("tags"), QStringList{}},
        {QStringLiteral("sort"), QStringLiteral("created_at")},
        {QStringLiteral("direction"), QStringLiteral("desc")},
    });
}

void TasksController::nextPage()
{
    if (!m_hasMore || m_loading) return;
    m_pageOffset += m_pageSize;
    emit stateChanged();
    refresh();
}

void TasksController::previousPage()
{
    if (m_pageOffset <= 0 || m_loading) return;
    m_pageOffset = std::max(0, m_pageOffset - m_pageSize);
    emit stateChanged();
    refresh();
}

storage::TaskFilter TasksController::currentFilter() const
{
    storage::TaskFilter filter;
    filter.query = m_filters.value(QStringLiteral("query")).toString();
    filter.completion = m_filters.value(QStringLiteral("completion"), QStringLiteral("all")).toString();
    filter.priority = m_filters.value(QStringLiteral("priority")).toString();
    filter.difficulty = m_filters.value(QStringLiteral("difficulty")).toString();
    filter.tags = stringList(m_filters.value(QStringLiteral("tags")));
    filter.sort = m_filters.value(QStringLiteral("sort"), QStringLiteral("created_at")).toString();
    filter.direction = m_filters.value(QStringLiteral("direction"), QStringLiteral("desc")).toString();
    filter.limit = m_pageSize + 1;
    filter.offset = m_pageOffset;
    const QString dateMode = m_filters.value(QStringLiteral("dateMode"), QStringLiteral("any")).toString();
    const QDate today = m_clock->localDate();
    if (dateMode == QStringLiteral("exact")) {
        filter.dueDate = m_filters.value(QStringLiteral("exactDate")).toString();
    } else if (dateMode == QStringLiteral("range")) {
        filter.dueFrom = m_filters.value(QStringLiteral("dueFrom")).toString();
        filter.dueTo = m_filters.value(QStringLiteral("dueTo")).toString();
    } else if (dateMode == QStringLiteral("overdue")) {
        filter.overdue = true;
    } else if (dateMode == QStringLiteral("undated")) {
        filter.undated = true;
    } else if (dateMode == QStringLiteral("today")) {
        filter.dueDate = today.toString(Qt::ISODate);
    } else if (dateMode == QStringLiteral("tomorrow")) {
        filter.dueDate = today.addDays(1).toString(Qt::ISODate);
    } else if (dateMode == QStringLiteral("next7")) {
        filter.dueFrom = today.toString(Qt::ISODate);
        filter.dueTo = today.addDays(6).toString(Qt::ISODate);
    }
    return filter;
}

QStringList TasksController::stringList(const QVariant &value) const
{
    if (value.metaType().id() == QMetaType::QStringList) return value.toStringList();
    if (value.metaType().id() == QMetaType::QVariantList) {
        QStringList result;
        for (const QVariant &item : value.toList()) result.push_back(item.toString());
        return result;
    }
    const QString text = value.toString();
    if (text.isEmpty()) return {};
    QStringList result;
    for (const QString &part : text.split(QLatin1Char(','))) result.push_back(part.trimmed());
    return result;
}

storage::TaskInput TasksController::taskInput(const QVariantMap &draft) const
{
    storage::TaskInput input;
    input.title = draft.value(QStringLiteral("title")).toString();
    input.description = draft.value(QStringLiteral("description")).toString();
    input.dueDate = draft.value(QStringLiteral("dueDate")).toString();
    input.priority = draft.value(QStringLiteral("priority")).toString();
    if (draft.contains(QStringLiteral("difficulty")))
        input.difficulty = draft.value(QStringLiteral("difficulty")).toString();
    if (draft.contains(QStringLiteral("tags")))
        input.tags = stringList(draft.value(QStringLiteral("tags")));
    if (draft.contains(QStringLiteral("subtasks"))) {
        QVector<storage::SubtaskInput> subtasks;
        for (const QVariant &value : draft.value(QStringLiteral("subtasks")).toList()) {
            const QVariantMap row = value.toMap();
            subtasks.push_back({row.value(QStringLiteral("id")).toLongLong(),
                                row.value(QStringLiteral("title")).toString(),
                                row.value(QStringLiteral("completed"), row.value(QStringLiteral("done"))).toBool()});
        }
        input.subtasks = std::move(subtasks);
    }
    return input;
}

void TasksController::toggleTask(qint64 id, bool completed)
{
    if (!m_ready || m_mutationBusy) return;
    const domain::Task *task = m_model.taskById(id);
    if (!task) return;
    setTaskCompleted(id, task->revision, completed);
}

void TasksController::toggleTaskFromView(qint64 id, qint64 revision, bool completed)
{
    setTaskCompleted(id, revision, completed);
}

void TasksController::setTaskCompleted(qint64 id, qint64 revision, bool completed)
{
    if (!m_ready || m_mutationBusy || id <= 0 || revision <= 0) return;
    m_mutationBusy = true;
    setError({});
    emit stateChanged();
    const auto operation = m_graph->beginOperation(QStringLiteral("change task completion"));
    if (!operation.isValid()) { m_mutationBusy = false; emit stateChanged(); return; }
    m_pending.insert(operation.id.value, {Action::Complete, id});
    if (!m_graph->submitStorage(operation,
            services::TaskService::setCompleted(id, revision, completed))) {
        if (m_pending.contains(operation.id.value)) {
            m_pending.remove(operation.id.value);
            m_mutationBusy = false;
            setError(QStringLiteral("Task could not be updated."));
            emit stateChanged();
        }
    }
}

void TasksController::cyclePriority(qint64 id)
{
    if (!m_ready || m_mutationBusy) return;
    const domain::Task *task = m_model.taskById(id);
    if (!task) return;
    const QStringList order{QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
    const QString currentPriority = task->priority.isEmpty()
        ? QStringLiteral("medium") : task->priority.toLower();
    const int current = order.indexOf(currentPriority);
    const QString next = order.at((current < 0 ? 0 : current + 1) % order.size());
    storage::TaskInput input;
    input.title = task->title;
    input.description = task->description;
    input.dueDate = task->dueDate;
    input.priority = next;
    // Omitted difficulty, tags and subtasks preserve those parts of the task.
    m_mutationBusy = true;
    setError({});
    emit stateChanged();
    const auto operation = m_graph->beginOperation(QStringLiteral("change task priority"));
    if (!operation.isValid()) { m_mutationBusy = false; emit stateChanged(); return; }
    m_pending.insert(operation.id.value, {Action::Priority, id});
    if (!m_graph->submitStorage(operation,
            services::TaskService::update(id, task->revision, std::move(input)))) {
        if (m_pending.contains(operation.id.value)) {
            m_pending.remove(operation.id.value);
            m_mutationBusy = false;
            setError(QStringLiteral("Task priority could not be updated."));
            emit stateChanged();
        }
    }
}

void TasksController::toggleSubtask(qint64 taskId, qint64 subtaskId, bool completed)
{
    if (!m_ready || m_mutationBusy) return;
    const domain::Task *task = m_model.taskById(taskId);
    if (!task) return;
    setSubtaskCompleted(taskId, subtaskId, task->revision, completed);
}

void TasksController::toggleSubtaskFromView(qint64 taskId, qint64 subtaskId,
                                             qint64 revision, bool completed)
{
    setSubtaskCompleted(taskId, subtaskId, revision, completed);
}

void TasksController::setSubtaskCompleted(qint64 taskId, qint64 subtaskId,
                                          qint64 revision, bool completed)
{
    if (!m_ready || m_mutationBusy || taskId <= 0 || subtaskId <= 0 || revision <= 0) return;
    m_mutationBusy = true;
    setError({});
    emit stateChanged();
    const auto operation = m_graph->beginOperation(QStringLiteral("change subtask completion"));
    if (!operation.isValid()) { m_mutationBusy = false; emit stateChanged(); return; }
    m_pending.insert(operation.id.value, {Action::Subtask, taskId});
    if (!m_graph->submitStorage(operation, services::TaskService::setSubtaskCompleted(
            taskId, subtaskId, revision, completed))) {
        if (m_pending.contains(operation.id.value)) {
            m_pending.remove(operation.id.value);
            m_mutationBusy = false;
            setError(QStringLiteral("Subtask could not be updated."));
            emit stateChanged();
        }
    }
}

void TasksController::saveTask(const QVariantMap &draft)
{
    if (!m_ready || m_mutationBusy) return;
    const qint64 id = draft.value(QStringLiteral("id")).toLongLong();
    storage::TaskInput input = taskInput(draft);
    if (id <= 0 && !input.difficulty) input.difficulty = QString{};
    // Validate omitted child metadata against the existing task's effective
    // difficulty, while leaving the field omitted for the repository merge.
    const bool preserveDifficulty = id > 0 && !input.difficulty;
    if (preserveDifficulty) {
        if (const domain::Task *current = m_model.taskById(id))
            input.difficulty = current->difficulty;
    }
    const domain::AppError validation = domain::normalizeTaskInput(input);
    if (validation.isError()) {
        setEditorError(validation.userMessage());
        emit stateChanged();
        return;
    }
    if (preserveDifficulty) input.difficulty.reset();
    m_mutationBusy = true;
    m_editorSaving = true;
    m_editorConflict = false;
    m_conflictReloadAvailable = false;
    m_conflictReloadMessage.clear();
    m_conflictTaskId = id;
    setEditorError({});
    setError({});
    emit stateChanged();
    const auto operation = m_graph->beginOperation(
        id > 0 ? QStringLiteral("update task") : QStringLiteral("create task"));
    if (!operation.isValid()) { m_mutationBusy = false; m_editorSaving = false; emit stateChanged(); return; }
    const Action action = id > 0 ? Action::Update : Action::Create;
    m_pending.insert(operation.id.value, {action, id});
    const auto command = id > 0
        ? services::TaskService::update(id, draft.value(QStringLiteral("revision")).toLongLong(),
                                       std::move(input))
        : services::TaskService::create(std::move(input));
    if (!m_graph->submitStorage(operation, command)
        && m_pending.contains(operation.id.value)) {
        m_pending.remove(operation.id.value);
        m_mutationBusy = false;
        m_editorSaving = false;
        setEditorError(QStringLiteral("The task could not be saved."));
        emit stateChanged();
    }
}

void TasksController::deleteTask(qint64 id)
{
    if (!m_ready || m_mutationBusy) return;
    const domain::Task *task = m_model.taskById(id);
    if (!task) return;
    m_mutationBusy = true;
    setError({});
    emit stateChanged();
    const auto operation = m_graph->beginOperation(QStringLiteral("delete task"));
    if (!operation.isValid()) { m_mutationBusy = false; emit stateChanged(); return; }
    m_pending.insert(operation.id.value, {Action::Delete, id});
    if (!m_graph->submitStorage(operation,
            services::TaskService::remove(id, task->revision))) {
        if (m_pending.contains(operation.id.value)) {
            m_pending.remove(operation.id.value);
            m_mutationBusy = false;
            setError(QStringLiteral("Task could not be deleted."));
            emit stateChanged();
        }
    }
}

void TasksController::reloadConflictTask()
{
    if (!m_ready || !m_editorConflict || m_conflictTaskId <= 0 || m_conflictReloading) return;
    m_conflictReloading = true;
    m_conflictReloadAvailable = false;
    m_conflictReloadMessage.clear();
    emit stateChanged();
    const auto operation = m_graph->beginOperation(QStringLiteral("reload conflicting task"));
    if (!operation.isValid()) { m_conflictReloading = false; emit stateChanged(); return; }
    m_pending.insert(operation.id.value, {Action::ReloadConflict, m_conflictTaskId});
    if (!m_graph->submitStorage(operation, services::TaskService::get(m_conflictTaskId))
        && m_pending.contains(operation.id.value)) {
        m_pending.remove(operation.id.value);
        m_conflictReloading = false;
        m_conflictReloadMessage = QStringLiteral("The latest task could not be loaded.");
        emit stateChanged();
    }
}

void TasksController::clearEditorState()
{
    if (m_editorSaving) return;
    setEditorError({});
    m_editorConflict = false;
    m_conflictReloading = false;
    m_conflictReloadAvailable = false;
    m_conflictReloadMessage.clear();
    m_conflictTaskId = 0;
    emit stateChanged();
}

void TasksController::onStorageFinished(quint64 operationId, const QVariantMap &values,
                                        domain::AppError error)
{
    const auto it = m_pending.find(operationId);
    if (it == m_pending.end()) return;
    const PendingAction action = it.value();
    m_pending.erase(it);
    if (action.action == Action::List) {
        if (m_listOperationId == operationId) m_listOperationId = 0;
        m_loading = false;
        if (error.isError()) {
            setError(error.userMessage());
        } else if (!error.isCancelled()) {
            QVector<domain::Task> rows;
            const QVariantList taskRows = values.value(QStringLiteral("tasks")).toList();
            rows.reserve(taskRows.size());
            for (const QVariant &row : taskRows)
                rows.push_back(services::TaskService::fromVariantMap(row.toMap()));
            m_model.setToday(m_clock->localDate());
            if (m_reconcileNextList) m_model.reconcilePage(std::move(rows));
            else m_model.replacePage(std::move(rows));
            m_reconcileNextList = false;
            m_hasMore = values.value(QStringLiteral("hasMore")).toBool();
            setError({});
        }
        emit stateChanged();
        return;
    }

    if (action.action == Action::ReloadConflict) {
        m_conflictReloading = false;
        if (error.isError()) {
            m_conflictReloadAvailable = false;
            m_conflictReloadMessage = error.category() == domain::ErrorCategory::NotFound
                ? QStringLiteral("This task was deleted. Your draft is still available.")
                : QStringLiteral("The latest task could not be loaded. Your draft is still available.");
        } else if (!error.isCancelled()) {
            m_conflictReloadAvailable = true;
            m_conflictReloadMessage = QStringLiteral("The latest version is ready. Choose whether to replace your draft.");
            emit conflictTaskLoaded(taskMapFrom(values));
        }
        emit stateChanged();
        return;
    }

    m_mutationBusy = false;
    if (action.action == Action::Create || action.action == Action::Update)
        m_editorSaving = false;
    if (error.isError()) {
        handleMutationError(action, error);
        emit stateChanged();
        return;
    }
    if (error.isCancelled()) {
        emit stateChanged();
        return;
    }

    if (action.action == Action::Delete) {
        m_model.removeTask(action.taskId);
        emit taskMutationCommitted(action.taskId, {}, true);
        setNotification(QStringLiteral("Task deleted."));
    } else {
        const QVariantMap taskMap = taskMapFrom(values);
        const domain::Task task = services::TaskService::fromVariantMap(taskMap);
        m_model.applyTask(task);
        emit taskMutationCommitted(task.id, taskMap, false);
        if (action.action == Action::Create || action.action == Action::Update) {
            if (action.action == Action::Create) m_pageOffset = 0;
            emit editorSaveSucceeded();
            setEditorError({});
            m_editorConflict = false;
            m_conflictReloadAvailable = false;
            setNotification(action.action == Action::Create
                ? QStringLiteral("Task added.") : QStringLiteral("Task saved."));
        } else if (action.action == Action::Priority) {
            setNotification(QStringLiteral("Priority updated."));
        }
    }
    refreshAfterMutation();
    emit stateChanged();
}

void TasksController::handleMutationError(const PendingAction &action, domain::AppError error)
{
    const bool editorAction = action.action == Action::Create || action.action == Action::Update;
    if (editorAction) {
        setEditorError(userError(error));
        if (action.action == Action::Update && error.category() == domain::ErrorCategory::Conflict) {
            m_editorConflict = true;
            m_conflictTaskId = action.taskId;
            m_conflictReloadAvailable = false;
            m_conflictReloadMessage.clear();
        }
    } else {
        setError(userError(error));
        if (error.category() == domain::ErrorCategory::Conflict) {
            m_reconcileNextList = true;
            refresh();
        }
    }
}

void TasksController::refreshAfterMutation()
{
    m_reconcileNextList = true;
    m_filterTimer.start(0);
}

void TasksController::setError(QString message)
{
    if (m_errorMessage == message) return;
    m_errorMessage = std::move(message);
    emit stateChanged();
}

void TasksController::setEditorError(QString message)
{
    if (m_editorErrorMessage == message) return;
    m_editorErrorMessage = std::move(message);
    emit stateChanged();
}

void TasksController::setNotification(QString message)
{
    if (m_notification == message) return;
    m_notification = std::move(message);
    emit notificationChanged();
    emit stateChanged();
}

void TasksController::clearNotification()
{
    setNotification({});
}

} // namespace something::controllers
