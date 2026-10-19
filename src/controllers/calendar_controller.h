#pragma once

#include "../application/application_service_graph.h"
#include "../domain/clock.h"
#include "../models/calendar_months_model.h"
#include "../models/tasks_model.h"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace something::controllers {

class CalendarController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::CalendarMonthsModel *months READ months CONSTANT)
    Q_PROPERTY(something::models::TasksModel *tasks READ tasks CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(int visibleYear READ visibleYear NOTIFY stateChanged)
    Q_PROPERTY(QString todayDate READ todayDate NOTIFY stateChanged)
    Q_PROPERTY(QString selectedDate READ selectedDate NOTIFY stateChanged)
    Q_PROPERTY(QString selectedDateLabel READ selectedDateLabel NOTIFY stateChanged)
    Q_PROPERTY(QString rangeLabel READ rangeLabel NOTIFY stateChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)

public:
    CalendarController(application::ApplicationServiceGraph *graph,
                       const domain::Clock *clock, QObject *parent = nullptr);

    models::CalendarMonthsModel *months() noexcept { return &m_months; }
    models::TasksModel *tasks() noexcept { return &m_tasks; }
    bool ready() const noexcept { return m_ready; }
    bool loading() const noexcept { return m_loading; }
    int visibleYear() const noexcept { return m_visibleYear; }
    QString todayDate() const;
    QString selectedDate() const { return m_selectedDate; }
    QString selectedDateLabel() const { return m_selectedDateLabel; }
    QString rangeLabel() const { return m_rangeLabel; }
    QString summary() const { return m_summary; }
    QString errorMessage() const { return m_errorMessage; }

    Q_INVOKABLE void previousYear();
    Q_INVOKABLE void nextYear();
    Q_INVOKABLE void goToday();
    Q_INVOKABLE void setPageVisible(bool visible);
    Q_INVOKABLE void selectDate(const QString &isoDate);
    Q_INVOKABLE void refreshSelectedDate();

public slots:
    void onTaskMutation(qint64 taskId, const QVariantMap &currentTask, bool deleted);

signals:
    void stateChanged();

private:
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void updateCalendarModel();
    void updateLabels();
    void loadSelectedDate(bool clearRows);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void setError(QString message);

    application::ApplicationServiceGraph *m_graph{nullptr};
    const domain::Clock *m_clock{nullptr};
    models::CalendarMonthsModel m_months;
    models::TasksModel m_tasks;
    QTimer m_clockTimer;
    QHash<quint64, bool> m_pending;
    QDate m_observedLocalDate;
    int m_visibleYear{0};
    quint64 m_dateOperationId{0};
    bool m_ready{false};
    bool m_pageVisible{false};
    bool m_loading{false};
    QString m_selectedDate;
    QString m_selectedDateLabel;
    QString m_rangeLabel;
    QString m_summary;
    QString m_errorMessage;
};

} // namespace something::controllers
