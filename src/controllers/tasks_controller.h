#pragma once

#include "../application/application_service_graph.h"
#include "../domain/clock.h"
#include "../models/tasks_model.h"
#include "../services/application_mutation_workflow.h"

#include <QHash>
#include <QDate>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

namespace something::controllers {

class TasksController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::TasksModel *model READ model CONSTANT)
    Q_PROPERTY(QVariantMap filters READ filters NOTIFY filtersChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool mutationBusy READ mutationBusy NOTIFY stateChanged)
    Q_PROPERTY(bool editorSaving READ editorSaving NOTIFY stateChanged)
    Q_PROPERTY(bool editorConflict READ editorConflict NOTIFY stateChanged)
    Q_PROPERTY(bool conflictReloading READ conflictReloading NOTIFY stateChanged)
    Q_PROPERTY(bool conflictReloadAvailable READ conflictReloadAvailable NOTIFY stateChanged)
    Q_PROPERTY(int pageNumber READ pageNumber NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString editorErrorMessage READ editorErrorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString conflictReloadMessage READ conflictReloadMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)
    Q_PROPERTY(QString todayDate READ todayDate NOTIFY stateChanged)

public:
    explicit TasksController(application::ApplicationServiceGraph *graph,
                             const domain::Clock *clock,
                             services::ApplicationMutationWorkflow *mutations,
                             QObject *parent = nullptr);

    models::TasksModel *model() noexcept { return &m_model; }
    QVariantMap filters() const { return m_filters; }
    bool ready() const noexcept { return m_ready; }
    bool loading() const noexcept { return m_loading; }
    bool mutationBusy() const noexcept { return m_mutationBusy; }
    bool editorSaving() const noexcept { return m_editorSaving; }
    bool editorConflict() const noexcept { return m_editorConflict; }
    bool conflictReloading() const noexcept { return m_conflictReloading; }
    bool conflictReloadAvailable() const noexcept { return m_conflictReloadAvailable; }
    int pageNumber() const noexcept { return m_pageOffset / m_pageSize + 1; }
    bool hasMore() const noexcept { return m_hasMore; }
    QString errorMessage() const { return m_errorMessage; }
    QString editorErrorMessage() const { return m_editorErrorMessage; }
    QString conflictReloadMessage() const { return m_conflictReloadMessage; }
    QString notification() const { return m_notification; }
    QString todayDate() const { return m_clock->localDate().toString(Qt::ISODate); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setFilters(const QVariantMap &filters);
    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE void toggleTask(qint64 id, bool completed);
    Q_INVOKABLE void toggleTaskFromView(qint64 id, qint64 revision, bool completed);
    Q_INVOKABLE void cyclePriority(qint64 id);
    Q_INVOKABLE void toggleSubtask(qint64 taskId, qint64 subtaskId, bool completed);
    Q_INVOKABLE void toggleSubtaskFromView(qint64 taskId, qint64 subtaskId,
                                          qint64 revision, bool completed);
    Q_INVOKABLE void saveTask(const QVariantMap &draft);
    Q_INVOKABLE void loadTaskForEditor(qint64 id);
    Q_INVOKABLE void deleteTask(qint64 id);
    Q_INVOKABLE void reloadConflictTask();
    Q_INVOKABLE void clearEditorState();
    Q_INVOKABLE void clearNotification();
    Q_INVOKABLE void setPageVisible(bool visible);

signals:
    void stateChanged();
    void filtersChanged();
    void editorSaveSucceeded();
    void conflictTaskLoaded(const QVariantMap &task);
    void editorTaskLoaded(const QVariantMap &task);
    void editorTaskLoadFailed(qint64 taskId, const QString &message);
    void notificationChanged();
    void taskMutationCommitted(qint64 taskId, const QVariantMap &currentTask, bool deleted);
    void taskMutationMayHaveCommitted();

private:
    enum class Action { List, Create, Update, Complete, Priority, Subtask, Delete,
                        ReloadConflict, EditorLoad };
    struct PendingAction {
        Action action{Action::List};
        qint64 taskId{0};
    };

    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onSharedMutationFinished(const services::MutationResult &result);
    void onSharedTaskCommitted(const QString &requestId, const QVariantMap &task, bool created);
    storage::TaskFilter currentFilter() const;
    storage::TaskInput taskInput(const QVariantMap &draft) const;
    QStringList stringList(const QVariant &value) const;
    void setError(QString message);
    void setEditorError(QString message);
    void setNotification(QString message);
    void refreshAfterMutation();
    void handleMutationError(const PendingAction &action, domain::AppError error);
    void setTaskCompleted(qint64 id, qint64 revision, bool completed);
    void setSubtaskCompleted(qint64 taskId, qint64 subtaskId,
                             qint64 revision, bool completed);

    application::ApplicationServiceGraph *m_graph{nullptr};
    services::ApplicationMutationWorkflow *m_mutations{nullptr};
    const domain::Clock *m_clock{nullptr};
    models::TasksModel m_model;
    QTimer m_filterTimer;
    QTimer m_localDateTimer;
    QDate m_observedLocalDate;
    QVariantMap m_filters;
    QHash<quint64, PendingAction> m_pending;
    QHash<QString, PendingAction> m_sharedPending;
    quint64 m_listOperationId{0};
    qint64 m_conflictTaskId{0};
    int m_pageSize{50};
    int m_pageOffset{0};
    bool m_ready{false};
    bool m_pageVisible{false};
    bool m_loading{false};
    bool m_mutationBusy{false};
    bool m_editorSaving{false};
    bool m_editorConflict{false};
    bool m_conflictReloading{false};
    bool m_conflictReloadAvailable{false};
    bool m_hasMore{false};
    bool m_reconcileNextList{false};
    bool m_externalMutationWhileHidden{false};
    QString m_errorMessage;
    QString m_editorErrorMessage;
    QString m_conflictReloadMessage;
    QString m_notification;
};

} // namespace something::controllers
