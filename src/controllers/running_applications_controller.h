#pragma once

#include "../application/application_service_graph.h"
#include "../models/running_windows_model.h"
#include "../platform/desktop_applications_platform.h"

#include <QGuiApplication>
#include <QTimer>
#include <QHash>

namespace something::controllers {

class RunningApplicationsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::RunningWindowsModel *windows READ windows CONSTANT)
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool hasSnapshot READ hasSnapshot NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool activating READ activating NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)

public:
    RunningApplicationsController(application::ApplicationServiceGraph *graph,
                                   platform::RunningWindowsPlatform *platform,
                                   QObject *parent = nullptr);

    models::RunningWindowsModel *windows() noexcept { return &m_windows; }
    bool supported() const noexcept { return m_platform && m_platform->supported(); }
    bool ready() const noexcept { return m_ready; }
    bool hasSnapshot() const noexcept { return m_hasSnapshot; }
    bool loading() const noexcept { return m_loading; }
    bool activating() const noexcept { return m_activating; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }

    Q_INVOKABLE void setPageVisible(bool visible);
    Q_INVOKABLE void setDashboardVisible(bool visible);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void activateWindow(const QString &windowId);
    Q_INVOKABLE void clearMessages();

signals:
    void stateChanged();

private:
    enum class Kind { List, Activate };
    struct Pending final {
        Kind kind{Kind::List};
        application::OperationContext operation;
        QString windowId;
    };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onApplicationStateChanged(Qt::ApplicationState state);
    void onWorkerFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void cancelList();
    void cancelAll();
    void finish(quint64 id);
    void fail(quint64 id, domain::AppError error, QString message = {});
    void updatePolling();

    application::ApplicationServiceGraph *m_graph{nullptr};
    platform::RunningWindowsPlatform *m_platform{nullptr};
    models::RunningWindowsModel m_windows;
    QTimer m_pollTimer;
    QHash<quint64, Pending> m_pending;
    quint64 m_listOperation{0};
    bool m_ready{false};
    bool m_pageVisible{false};
    bool m_dashboardVisible{false};
    bool m_applicationActive{false};
    bool m_hasSnapshot{false};
    bool m_loading{false};
    bool m_activating{false};
    QString m_errorMessage;
    QString m_notification;
};

} // namespace something::controllers
