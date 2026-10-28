#pragma once

#include "../application/application_service_graph.h"
#include "../domain/clock.h"
#include "../models/dashboard_news_model.h"
#include "../models/tasks_model.h"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace something::controllers {

class DashboardController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::TasksModel *todayTasks READ todayTasks CONSTANT)
    Q_PROPERTY(something::models::TasksModel *overdueTasks READ overdueTasks CONSTANT)
    Q_PROPERTY(something::models::TasksModel *weekTasks READ weekTasks CONSTANT)
    Q_PROPERTY(something::models::DashboardNewsModel *favoriteNews READ favoriteNews CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool todayLoading READ todayLoading NOTIFY stateChanged)
    Q_PROPERTY(bool weekLoading READ weekLoading NOTIFY stateChanged)
    Q_PROPERTY(bool favoriteNewsLoading READ favoriteNewsLoading NOTIFY stateChanged)
    Q_PROPERTY(QString todayError READ todayError NOTIFY stateChanged)
    Q_PROPERTY(QString weekError READ weekError NOTIFY stateChanged)
    Q_PROPERTY(QString favoriteNewsError READ favoriteNewsError NOTIFY stateChanged)
    Q_PROPERTY(QString todayDate READ todayDate NOTIFY stateChanged)
    Q_PROPERTY(QString weekRangeLabel READ weekRangeLabel NOTIFY stateChanged)

public:
    DashboardController(application::ApplicationServiceGraph *graph,
                        const domain::Clock *clock, QObject *parent = nullptr);

    models::TasksModel *todayTasks() noexcept { return &m_todayTasks; }
    models::TasksModel *overdueTasks() noexcept { return &m_overdueTasks; }
    models::TasksModel *weekTasks() noexcept { return &m_weekTasks; }
    models::DashboardNewsModel *favoriteNews() noexcept { return &m_favoriteNews; }
    bool ready() const noexcept { return m_ready; }
    bool todayLoading() const noexcept { return m_todayLoading; }
    bool weekLoading() const noexcept { return m_weekLoading; }
    bool favoriteNewsLoading() const noexcept { return m_favoriteNewsLoading; }
    QString todayError() const { return m_todayError; }
    QString weekError() const { return m_weekError; }
    QString favoriteNewsError() const { return m_favoriteNewsError; }
    QString todayDate() const;
    QString weekRangeLabel() const { return m_weekRangeLabel; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reloadFavoriteNews();
    Q_INVOKABLE void setPageVisible(bool visible);

public slots:
    void setWeekStart(int sundayZero);
    void refreshTaskSummaries();

    void onTaskMutation(qint64 taskId, const QVariantMap &currentTask, bool deleted);

signals:
    void stateChanged();
    void navigateRequested(const QString &routeId);
    void favoriteNewsItemsChanged(const QVariantList &items);

private:
    enum class Action { Today, Week, FavoriteNews };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void loadToday();
    void loadWeek();
    void loadFavoriteNews();
    void checkClock(bool resumed);
    void cancelOperation(quint64 &operationId);

    application::ApplicationServiceGraph *m_graph{nullptr};
    const domain::Clock *m_clock{nullptr};
    models::TasksModel m_todayTasks;
    models::TasksModel m_overdueTasks;
    models::TasksModel m_weekTasks;
    models::DashboardNewsModel m_favoriteNews;
    QTimer m_clockTimer;
    QHash<quint64, Action> m_pending;
    QDate m_observedLocalDate;
    QDate m_weekFrom;
    QDate m_weekThrough;
    int m_weekStartSundayZero{1};
    quint64 m_todayOperationId{0};
    quint64 m_weekOperationId{0};
    quint64 m_favoriteNewsOperationId{0};
    bool m_ready{false};
    bool m_pageVisible{false};
    bool m_todayLoading{false};
    bool m_weekLoading{false};
    bool m_favoriteNewsLoading{false};
    QString m_todayError;
    QString m_weekError;
    QString m_favoriteNewsError;
    QString m_weekRangeLabel;
};

} // namespace something::controllers
