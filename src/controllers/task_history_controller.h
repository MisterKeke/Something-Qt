#pragma once

#include "../application/application_service_graph.h"
#include "../models/task_history_model.h"
#include "../storage/repository_types.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <optional>

namespace something::controllers {

class TaskHistoryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::TaskHistoryModel *model READ model CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)

public:
    explicit TaskHistoryController(application::ApplicationServiceGraph *graph,
                                   QObject *parent = nullptr);

    models::TaskHistoryModel *model() noexcept { return &m_model; }
    bool ready() const noexcept { return m_ready; }
    bool loading() const noexcept { return m_loading; }
    bool hasMore() const noexcept { return m_hasMore; }
    QString errorMessage() const { return m_errorMessage; }

    Q_INVOKABLE void loadTaskHistory(qint64 taskId, const QString &eventType = {});
    Q_INVOKABLE void loadRecentHistory(const QString &taskTitleQuery = {},
                                       const QString &eventType = {});
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void setVisible(bool visible);

signals:
    void stateChanged();

private:
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    bool setEventType(const QString &name);
    void startLoad(bool append);
    void cancelCurrent();
    void setError(QString message);

    application::ApplicationServiceGraph *m_graph{nullptr};
    models::TaskHistoryModel m_model;
    storage::TaskHistoryCursor m_cursor;
    std::optional<domain::TaskHistoryEventType> m_eventType;
    qint64 m_taskId{0};
    quint64 m_operationId{0};
    QString m_taskTitleQuery;
    QString m_errorMessage;
    bool m_taskScoped{false};
    bool m_hasQuery{false};
    bool m_ready{false};
    bool m_visible{false};
    bool m_loading{false};
    bool m_hasMore{false};
    bool m_appendResult{false};
};

} // namespace something::controllers
